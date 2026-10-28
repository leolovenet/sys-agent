#include <string.h>

#include "debug_hwbp.h"
#include "debug_multitrace.h"
#include "debug_process.h"
#include "debug_trace.h"
#include "debug_watch.h"
#include "dmnt_client.h"

#define DEBUG_MULTITRACE_THREAD_STACK_SIZE 0x10000
#define DEBUG_MULTITRACE_THREAD_PRIORITY 0x30
#define DEBUG_MULTITRACE_EVENT_TIMEOUT_NS 20000000LL
#define DEBUG_MULTITRACE_DEFAULT_DURATION_SECONDS 60
#define DEBUG_MULTITRACE_SLOTS_MAX 16

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadStarted;
    bool active;
    bool armed;
    bool everArmed;
    bool stopRequested;
    bool hasEvent;
    u64 processIds[DEBUG_MULTITRACE_PROCESS_COUNT];
    u64 addresses[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 targetHits[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 contextSlots[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 instructionSlots[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 maxHits;
    u64 hitCount;
    u64 durationSeconds;
    u64 startedTick;
    u32 ringSize;
    u32 ringCount;
    u32 ringNext;
    u32 argsCount;
    u32 ignoredHits;
    u64 lastSequence;
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastThreadId;
    u32 lastCore;
    DebugTraceEvent lastEvent;
    DebugTraceEvent ring[DEBUG_TRACE_RING_MAX];
    Result lastError;
    char stage[32];
} DebugMultiTraceState;

static DebugMultiTraceState multiState;

static void setError(const char* stage, Result rc)
{
    mutexLock(&multiState.mutex);
    multiState.lastError = rc;
    strncpy(multiState.stage, stage, sizeof(multiState.stage) - 1);
    multiState.stage[sizeof(multiState.stage) - 1] = 0;
    mutexUnlock(&multiState.mutex);
}

static bool stopRequested(void)
{
    mutexLock(&multiState.mutex);
    const bool stop = multiState.stopRequested;
    mutexUnlock(&multiState.mutex);
    return stop;
}

static bool durationExpired(void)
{
    mutexLock(&multiState.mutex);
    const u64 duration = multiState.durationSeconds;
    const u64 started = multiState.startedTick;
    mutexUnlock(&multiState.mutex);
    if (duration == 0)
        return false;
    return (armGetSystemTick() - started)
        >= (u64)armGetSystemTickFreq() * duration;
}

static bool hitLimitReached(void)
{
    mutexLock(&multiState.mutex);
    const bool done = debugTraceHitLimitReached(multiState.hitCount,
        multiState.maxHits);
    mutexUnlock(&multiState.mutex);
    return done;
}

static bool detachDmntForApplication(u64 pid)
{
    u64 appPid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&appPid)) || appPid != pid)
        return false;
    if (R_FAILED(dmntClientInitialize()))
        return false;
    bool attached = false;
    if (R_FAILED(dmntClientHasProcess(&attached)) || !attached)
        return false;
    return R_SUCCEEDED(dmntClientForceClose());
}

static Result drainInitialEvents(Handle debugHandle, bool* processExited)
{
    const u64 deadline = armGetSystemTick() + armGetSystemTickFreq();
    bool attachedSeen = false;
    while (true) {
        s32 index = -1;
        Result rc = svcWaitSynchronization(&index, &debugHandle, 1,
            DEBUG_MULTITRACE_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut)) {
            if (attachedSeen || armGetSystemTick() >= deadline)
                break;
            continue;
        }
        if (R_FAILED(rc))
            return rc;
        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
            if (event.type == DebugEventType_ExitProcess) {
                *processExited = true;
                return 0;
            }
            if (event.type == DebugEventType_Exception
                && event.info.exception.type
                    == DebugException_DebuggerAttached) {
                attachedSeen = true;
            }
        }
    }
    while (true) {
        DebugEventInfo event;
        Result eventRc = svcGetDebugEvent(&event, debugHandle);
        if (R_FAILED(eventRc))
            break;
        if (event.type == DebugEventType_ExitProcess) {
            *processExited = true;
            return 0;
        }
    }
    return 0;
}

static void captureEvent(Handle debugHandle, const DebugEventInfo* event,
    u32 targetIndex, u64 targetAddress, u32 argsCount,
    DebugTraceEvent* out)
{
    memset(out, 0, sizeof(*out));
    out->threadId = event->thread_id;
    out->pc = (u64)event->info.exception.address;
    out->targetIndex = targetIndex;
    out->targetAddress = targetAddress;
    out->tick = armGetSystemTick();
    out->argsCount = argsCount;

    ThreadContext context;
    Result contextRc = svcGetDebugThreadContext(&context, debugHandle,
        event->thread_id, RegisterGroup_CpuAll);
    out->contextResult = (u32)contextRc;
    if (R_SUCCEEDED(contextRc)) {
        for (u32 i = 0; i < DEBUG_TRACE_ARGS_MAX && i < 29; i++)
            out->x[i] = context.cpu_gprs[i].x;
        out->lr = context.lr;
        out->sp = context.sp;
        out->pc = context.pc.x;
        if (out->pc != 0
            && R_SUCCEEDED(svcReadDebugProcessMemory(out->insn, debugHandle,
                out->pc, sizeof(out->insn))))
            out->insnBytes = sizeof(out->insn);
    }

    u64 coreValue = 0;
    u32 coreType = 0;
    Result coreRc = svcGetDebugThreadParam(&coreValue, &coreType, debugHandle,
        event->thread_id, DebugThreadParam_CurrentCore);
    out->coreResult = (u32)coreRc;
    if (R_SUCCEEDED(coreRc))
        out->core = coreType;
}

static void recordEvent(DebugTraceEvent* event)
{
    mutexLock(&multiState.mutex);
    multiState.hitCount++;
    event->sequence = multiState.hitCount;
    if (event->targetIndex < DEBUG_MULTITRACE_PROCESS_COUNT)
        multiState.targetHits[event->targetIndex]++;
    multiState.lastEvent = *event;
    multiState.hasEvent = true;
    if (multiState.ringSize > 0) {
        multiState.ring[multiState.ringNext] = *event;
        multiState.ringNext =
            (multiState.ringNext + 1) % multiState.ringSize;
        if (multiState.ringCount < multiState.ringSize)
            multiState.ringCount++;
    }
    multiState.lastSequence = event->sequence;
    multiState.lastPc = event->pc;
    multiState.lastLr = event->lr;
    multiState.lastSp = event->sp;
    multiState.lastThreadId = event->threadId;
    multiState.lastCore = event->core;
    mutexUnlock(&multiState.mutex);
}

static void recordIgnoredHit(void)
{
    mutexLock(&multiState.mutex);
    multiState.ignoredHits++;
    mutexUnlock(&multiState.mutex);
}

static void multiTraceThreadMain(void* arg)
{
    (void)arg;
    Handle handles[DEBUG_MULTITRACE_PROCESS_COUNT] = {
        INVALID_HANDLE, INVALID_HANDLE,
    };
    bool attached[DEBUG_MULTITRACE_PROCESS_COUNT] = { false, false };
    bool resumed[DEBUG_MULTITRACE_PROCESS_COUNT] = { false, false };
    bool exited[DEBUG_MULTITRACE_PROCESS_COUNT] = { false, false };
    bool pending[DEBUG_MULTITRACE_PROCESS_COUNT] = { false, false };
    bool armed = false;
    Result rc = 0;
    Result cleanupRc = 0;
    u64 pids[DEBUG_MULTITRACE_PROCESS_COUNT];
    u64 addresses[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 argsCount = 0;
    u32 context1 = 0;
    u32 context2 = 0;
    u32 slots[DEBUG_MULTITRACE_PROCESS_COUNT] = { 0, 0 };

    mutexLock(&multiState.mutex);
    memcpy(pids, multiState.processIds, sizeof(pids));
    memcpy(addresses, multiState.addresses, sizeof(addresses));
    argsCount = multiState.argsCount;
    mutexUnlock(&multiState.mutex);

    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++)
        detachDmntForApplication(pids[i]);

    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        rc = svcDebugActiveProcess(&handles[i], pids[i]);
        if (R_FAILED(rc)) {
            setError("debugActiveProcess", rc);
            goto cleanup;
        }
        attached[i] = true;
    }

    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        rc = drainInitialEvents(handles[i], &exited[i]);
        if (R_FAILED(rc)) {
            setError("drainInitial", rc);
            goto cleanup;
        }
        if (exited[i]) {
            setError("exitProcess", MAKERESULT(Module_Libnx,
                LibnxError_NotFound));
            goto cleanup;
        }
    }

    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        rc = debugProcessResume(handles[i]);
        if (R_FAILED(rc)) {
            setError("resume", rc);
            goto cleanup;
        }
        resumed[i] = true;
    }

    rc = debugHwBpProbeContextSlots(&context1, &context2);
    if (R_FAILED(rc)) {
        setError("probeContext", rc);
        goto cleanup;
    }
    u32 allSlots[DEBUG_MULTITRACE_SLOTS_MAX];
    u32 allCount = 0;
    rc = debugHwBpProbeInstructionSlots(context1, allSlots,
        DEBUG_MULTITRACE_SLOTS_MAX, &allCount);
    if (R_FAILED(rc)) {
        setError("probeInstruction", rc);
        goto cleanup;
    }
    u32 slotCount = 0;
    for (u32 i = 0; i < allCount
        && slotCount < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        if (allSlots[i] != context2)
            slots[slotCount++] = allSlots[i];
    }
    if (slotCount < DEBUG_MULTITRACE_PROCESS_COUNT) {
        setError("instructionSlots", MAKERESULT(Module_Libnx,
            LibnxError_NotFound));
        goto cleanup;
    }

    mutexLock(&multiState.mutex);
    multiState.contextSlots[0] = context1;
    multiState.contextSlots[1] = context2;
    multiState.instructionSlots[0] = slots[0];
    multiState.instructionSlots[1] = slots[1];
    mutexUnlock(&multiState.mutex);

    rc = debugHwBpArmDualInstructionTrace(context1, handles[0],
        slots[0], addresses[0], context2, handles[1],
        slots[1], addresses[1]);
    if (R_FAILED(rc)) {
        setError("armTrace", rc);
        goto cleanup;
    }
    armed = true;

    mutexLock(&multiState.mutex);
    multiState.armed = true;
    multiState.everArmed = true;
    multiState.startedTick = armGetSystemTick();
    multiState.lastError = 0;
    multiState.stage[0] = 0;
    mutexUnlock(&multiState.mutex);

    while (!stopRequested() && !durationExpired()) {
        s32 signaled = -1;
        rc = svcWaitSynchronization(&signaled, handles,
            DEBUG_MULTITRACE_PROCESS_COUNT, DEBUG_MULTITRACE_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc)) {
            setError("waitEvent", rc);
            break;
        }
        if (signaled < 0 || signaled >= DEBUG_MULTITRACE_PROCESS_COUNT) {
            setError("waitEventIndex", MAKERESULT(Module_Libnx,
                LibnxError_BadInput));
            break;
        }

        const u32 processIndex = (u32)signaled;
        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, handles[processIndex]);
            if (R_FAILED(eventRc))
                break;
            pending[processIndex] = true;
            if (event.type == DebugEventType_ExitProcess) {
                exited[processIndex] = true;
                setError("exitProcess", MAKERESULT(Module_Libnx,
                    LibnxError_NotFound));
                goto cleanup;
            }
            if (event.type == DebugEventType_Exception
                && event.info.exception.type == DebugException_BreakPoint
                && event.info.exception.specific.break_point.type
                    == BreakPointType_HardwareInstruction) {
                u32 targetIndex = UINT32_MAX;
                for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
                    if (addresses[i] == (u64)event.info.exception.address) {
                        targetIndex = i;
                        break;
                    }
                }
                if (targetIndex != UINT32_MAX
                    && targetIndex == processIndex) {
                    DebugTraceEvent captured;
                    captureEvent(handles[processIndex], &event, targetIndex,
                        addresses[targetIndex], argsCount, &captured);
                    recordEvent(&captured);
                    if (hitLimitReached()) {
                        mutexLock(&multiState.mutex);
                        strncpy(multiState.stage, "hitsReached",
                            sizeof(multiState.stage) - 1);
                        multiState.stage[sizeof(multiState.stage) - 1] = 0;
                        mutexUnlock(&multiState.mutex);
                        goto cleanup;
                    }
                } else {
                    recordIgnoredHit();
                }
            }
        }
        rc = debugHwBpContinueAll(handles[processIndex]);
        if (R_FAILED(rc)) {
            setError("continue", rc);
            break;
        }
        pending[processIndex] = false;
    }
    rc = 0;

cleanup:
    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        if (attached[i] && !exited[i]) {
            Result quiesceRc = debugHwBpQuiesce(handles[i],
                !resumed[i] || pending[i]);
            if (R_FAILED(quiesceRc) && R_SUCCEEDED(cleanupRc))
                cleanupRc = quiesceRc;
        }
    }
    if (armed) {
        const u32 registers[4] = {
            context1, context2, slots[0], slots[1],
        };
        Result clearRc = debugHwBpClearRegistersAllCores(registers, 4);
        if (R_FAILED(clearRc) && R_SUCCEEDED(cleanupRc))
            cleanupRc = clearRc;
    }
    for (u32 i = 0; i < DEBUG_MULTITRACE_PROCESS_COUNT; i++) {
        if (attached[i])
            debugProcessClose(handles[i]);
    }

    mutexLock(&multiState.mutex);
    multiState.active = false;
    multiState.armed = false;
    multiState.stopRequested = false;
    if (R_FAILED(cleanupRc) && multiState.lastError == 0) {
        multiState.lastError = cleanupRc;
        strncpy(multiState.stage, "cleanup",
            sizeof(multiState.stage) - 1);
        multiState.stage[sizeof(multiState.stage) - 1] = 0;
    }
    if (R_FAILED(rc) && multiState.lastError == 0)
        multiState.lastError = rc;
    mutexUnlock(&multiState.mutex);
    threadExit();
}

void debugMultiTraceInitialize(void)
{
    memset(&multiState, 0, sizeof(multiState));
    mutexInit(&multiState.mutex);
    multiState.durationSeconds =
        DEBUG_MULTITRACE_DEFAULT_DURATION_SECONDS;
}

bool debugMultiTraceStart(const DebugMultiTraceConfig* config)
{
    if (config == NULL || config->processIds[0] == 0
        || config->processIds[1] == 0
        || config->processIds[0] == config->processIds[1]
        || config->addresses[0] == 0 || config->addresses[1] == 0
        || config->addresses[0] == config->addresses[1]
        || (config->addresses[0] & 3) != 0
        || (config->addresses[1] & 3) != 0
        || (config->maxHits == 0 && config->ringSize == 0)
        || config->ringSize > DEBUG_TRACE_RING_MAX
        || config->argsCount > DEBUG_TRACE_ARGS_MAX) {
        setError("invalid", MAKERESULT(Module_Libnx,
            LibnxError_BadInput));
        return false;
    }
    if (debugWatchIsActive() || debugTraceIsActive()) {
        setError("ownedByOtherTracer",
            KERNELRESULT(OwnedByAnotherProcess));
        return false;
    }

    mutexLock(&multiState.mutex);
    if (multiState.active) {
        multiState.lastError = KERNELRESULT(OwnedByAnotherProcess);
        strncpy(multiState.stage, "alreadyActive",
            sizeof(multiState.stage) - 1);
        multiState.stage[sizeof(multiState.stage) - 1] = 0;
        mutexUnlock(&multiState.mutex);
        return false;
    }
    if (multiState.threadStarted) {
        mutexUnlock(&multiState.mutex);
        threadWaitForExit(&multiState.thread);
        threadClose(&multiState.thread);
        mutexLock(&multiState.mutex);
        multiState.threadStarted = false;
    }
    memcpy(multiState.processIds, config->processIds,
        sizeof(multiState.processIds));
    memcpy(multiState.addresses, config->addresses,
        sizeof(multiState.addresses));
    memset(multiState.targetHits, 0, sizeof(multiState.targetHits));
    memset(multiState.contextSlots, 0, sizeof(multiState.contextSlots));
    memset(multiState.instructionSlots, 0,
        sizeof(multiState.instructionSlots));
    multiState.maxHits = config->maxHits;
    multiState.durationSeconds = config->durationSeconds;
    multiState.hitCount = 0;
    multiState.ringSize = config->ringSize;
    multiState.ringCount = 0;
    multiState.ringNext = 0;
    multiState.argsCount = config->argsCount;
    multiState.ignoredHits = 0;
    multiState.hasEvent = false;
    multiState.lastSequence = 0;
    multiState.lastPc = 0;
    multiState.lastLr = 0;
    multiState.lastSp = 0;
    multiState.lastThreadId = 0;
    multiState.lastCore = 0;
    multiState.lastError = 0;
    multiState.stage[0] = 0;
    multiState.stopRequested = false;
    multiState.active = true;
    multiState.armed = false;
    multiState.everArmed = false;
    multiState.threadStarted = true;
    mutexUnlock(&multiState.mutex);

    Result rc = threadCreate(&multiState.thread, multiTraceThreadMain,
        NULL, NULL, DEBUG_MULTITRACE_THREAD_STACK_SIZE,
        DEBUG_MULTITRACE_THREAD_PRIORITY, -2);
    if (R_FAILED(rc)) {
        mutexLock(&multiState.mutex);
        multiState.active = false;
        multiState.threadStarted = false;
        multiState.lastError = rc;
        mutexUnlock(&multiState.mutex);
        return false;
    }
    rc = threadStart(&multiState.thread);
    if (R_FAILED(rc)) {
        mutexLock(&multiState.mutex);
        multiState.active = false;
        multiState.threadStarted = false;
        multiState.lastError = rc;
        mutexUnlock(&multiState.mutex);
        threadClose(&multiState.thread);
        return false;
    }
    return true;
}

void debugMultiTraceStop(void)
{
    mutexLock(&multiState.mutex);
    const bool running = multiState.active || multiState.threadStarted;
    multiState.stopRequested = true;
    mutexUnlock(&multiState.mutex);
    if (!running)
        return;
    threadWaitForExit(&multiState.thread);
    threadClose(&multiState.thread);
    mutexLock(&multiState.mutex);
    multiState.threadStarted = false;
    multiState.active = false;
    multiState.armed = false;
    multiState.stopRequested = false;
    mutexUnlock(&multiState.mutex);
}

bool debugMultiTraceGetStatus(DebugMultiTraceStatus* out)
{
    memset(out, 0, sizeof(*out));
    mutexLock(&multiState.mutex);
    out->active = multiState.active;
    out->armed = multiState.armed;
    out->everArmed = multiState.everArmed;
    out->stale = !multiState.active
        && (multiState.hitCount != 0 || multiState.addresses[0] != 0);
    out->memoryBlocked = multiState.active;
    memcpy(out->processIds, multiState.processIds,
        sizeof(out->processIds));
    memcpy(out->addresses, multiState.addresses, sizeof(out->addresses));
    memcpy(out->targetHits, multiState.targetHits,
        sizeof(out->targetHits));
    memcpy(out->contextSlots, multiState.contextSlots,
        sizeof(out->contextSlots));
    memcpy(out->instructionSlots, multiState.instructionSlots,
        sizeof(out->instructionSlots));
    out->hitCount = multiState.hitCount;
    out->durationSeconds = multiState.durationSeconds;
    out->startedTick = multiState.startedTick;
    out->maxHits = multiState.maxHits;
    out->ringSize = multiState.ringSize;
    out->ringCount = multiState.ringCount;
    out->argsCount = multiState.argsCount;
    out->ignoredHits = multiState.ignoredHits;
    out->lastSequence = multiState.lastSequence;
    out->lastPc = multiState.lastPc;
    out->lastLr = multiState.lastLr;
    out->lastSp = multiState.lastSp;
    out->lastThreadId = multiState.lastThreadId;
    out->lastCore = multiState.lastCore;
    out->lastError = multiState.lastError;
    strncpy(out->stage, multiState.stage, sizeof(out->stage) - 1);
    out->stage[sizeof(out->stage) - 1] = 0;
    mutexUnlock(&multiState.mutex);
    return true;
}

bool debugMultiTraceGetEvent(u32 index, DebugTraceEvent* out)
{
    mutexLock(&multiState.mutex);
    if (!multiState.hasEvent) {
        mutexUnlock(&multiState.mutex);
        return false;
    }
    if (multiState.ringSize == 0) {
        if (index != 0) {
            mutexUnlock(&multiState.mutex);
            return false;
        }
        *out = multiState.lastEvent;
        mutexUnlock(&multiState.mutex);
        return true;
    }
    const u32 stored = debugTraceRingOldestIndex(multiState.ringNext,
        multiState.ringSize, multiState.ringCount, index);
    if (stored == UINT32_MAX) {
        mutexUnlock(&multiState.mutex);
        return false;
    }
    *out = multiState.ring[stored];
    mutexUnlock(&multiState.mutex);
    return true;
}

bool debugMultiTraceIsActive(void)
{
    mutexLock(&multiState.mutex);
    const bool active = multiState.active;
    mutexUnlock(&multiState.mutex);
    return active;
}
