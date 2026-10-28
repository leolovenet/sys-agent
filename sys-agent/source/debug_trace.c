#include <string.h>

#include "debug_hwbp.h"
#include "debug_multitrace.h"
#include "debug_trace.h"
#include "debug_watch.h"
#include "dmnt_client.h"

#define DEBUG_TRACE_THREAD_STACK_SIZE 0x10000
#define DEBUG_TRACE_THREAD_PRIORITY 0x30
#define DEBUG_TRACE_EVENT_TIMEOUT_NS 20000000LL
#define DEBUG_TRACE_DEFAULT_DURATION_SECONDS 60

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadStarted;
    bool active;
    bool armed;
    bool everArmed;
    bool stopRequested;
    bool hasEvent;
    bool dmntClosed;
    u64 processId;
    u64 address;
    u32 targetCount;
    u64 targetAddresses[DEBUG_TRACE_TARGET_MAX];
    u32 targetHits[DEBUG_TRACE_TARGET_MAX];
    u32 instructionSlots[DEBUG_TRACE_TARGET_MAX];
    u32 maxHits;
    u64 hitCount;
    u64 durationSeconds;
    u64 startedTick;
    u64 durationStartTick;
    bool durationFromFirstHit;
    u32 ringSize;
    u32 ringCount;
    u32 ringNext;
    u32 argsCount;
    u32 contextSlot;
    u32 instructionSlot;
    u32 ignoredHits;
    u32 snapshotCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 targetSnapshotCounts[DEBUG_TRACE_TARGET_MAX];
    DebugTraceSnapshotSpec targetSnapshots[DEBUG_TRACE_TARGET_MAX]
        [DEBUG_TRACE_SNAPSHOT_MAX];
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastThreadId;
    u32 lastCore;
    DebugTraceEvent lastEvent;
    DebugTraceEvent ring[DEBUG_TRACE_RING_MAX];
    Result lastError;
    char stage[32];
    char hint[160];
} DebugTraceState;

static DebugTraceState traceState;

static void setError(const char* stage, Result rc)
{
    mutexLock(&traceState.mutex);
    traceState.lastError = rc;
    strncpy(traceState.stage, stage, sizeof(traceState.stage) - 1);
    traceState.stage[sizeof(traceState.stage) - 1] = 0;
    traceState.hint[0] = 0;
    mutexUnlock(&traceState.mutex);
}

static void setStage(const char* stage)
{
    mutexLock(&traceState.mutex);
    strncpy(traceState.stage, stage, sizeof(traceState.stage) - 1);
    traceState.stage[sizeof(traceState.stage) - 1] = 0;
    traceState.hint[0] = 0;
    mutexUnlock(&traceState.mutex);
}

static void setDebugHandleInUseHint(void)
{
    mutexLock(&traceState.mutex);
    strncpy(traceState.hint,
        "debug handle still owned by another debugger (for example the "
        "standalone gdbstub); close it and retry",
        sizeof(traceState.hint) - 1);
    traceState.hint[sizeof(traceState.hint) - 1] = 0;
    mutexUnlock(&traceState.mutex);
}

static bool stopRequested(void)
{
    mutexLock(&traceState.mutex);
    const bool stop = traceState.stopRequested;
    mutexUnlock(&traceState.mutex);
    return stop;
}

static bool durationExpired(void)
{
    mutexLock(&traceState.mutex);
    const u64 duration = traceState.durationSeconds;
    const u64 started = traceState.durationFromFirstHit
        ? traceState.durationStartTick : traceState.startedTick;
    const bool hasStarted = !traceState.durationFromFirstHit
        || traceState.durationStartTick != 0;
    mutexUnlock(&traceState.mutex);
    if (duration == 0)
        return false;
    if (!hasStarted)
        return false;
    return (armGetSystemTick() - started)
        >= (u64)armGetSystemTickFreq() * duration;
}

static bool hitLimitReached(void)
{
    mutexLock(&traceState.mutex);
    const bool done = debugTraceHitLimitReached(traceState.hitCount,
        traceState.maxHits);
    mutexUnlock(&traceState.mutex);
    return done;
}

/*
 * Closing dmnt:cht is only valid for the foreground application. System
 * processes have their own debug ownership and must not disturb the game.
 */
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

/*
 * Drain the fresh attach's synthetic events without resuming. The trace
 * breakpoint is armed while the target is still stopped, then the single
 * resume starts the observation window.
 */
static Result drainInitialEvents(Handle debugHandle, bool* processExited)
{
    const u64 deadline = armGetSystemTick() + armGetSystemTickFreq();
    bool attachedSeen = false;

    while (true) {
        s32 index = -1;
        Result rc = svcWaitSynchronization(&index, &debugHandle, 1,
            DEBUG_TRACE_EVENT_TIMEOUT_NS);
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
    const DebugTraceSnapshotSpec* snapshots, u32 snapshotCount,
    DebugTraceEvent* out)
{
    memset(out, 0, sizeof(*out));
    out->threadId = event->thread_id;
    out->pc = (u64)event->info.exception.address;
    out->targetIndex = targetIndex;
    out->targetAddress = targetAddress;
    out->tick = armGetSystemTick();
    out->argsCount = argsCount;
    out->snapshotCount = snapshotCount;

    ThreadContext context;
    Result contextRc = svcGetDebugThreadContext(&context, debugHandle,
        event->thread_id, RegisterGroup_CpuAll);
    out->contextResult = (u32)contextRc;
    if (R_SUCCEEDED(contextRc)) {
        u64 gprs[31];
        for (u32 i = 0; i < DEBUG_TRACE_ARGS_MAX && i < 29; i++)
            out->x[i] = context.cpu_gprs[i].x;
        for (u32 i = 0; i < 29; i++)
            gprs[i] = context.cpu_gprs[i].x;
        gprs[29] = context.fp;
        gprs[30] = context.lr;
        out->lr = context.lr;
        out->sp = context.sp;
        out->pc = context.pc.x;

        if (out->pc >= 8) {
            u8 instruction[DEBUG_TRACE_INSN_BYTES];
            if (R_SUCCEEDED(svcReadDebugProcessMemory(instruction, debugHandle,
                    out->pc, sizeof(instruction)))) {
                memcpy(out->insn, instruction, sizeof(instruction));
                out->insnBytes = sizeof(instruction);
            }
        }

        for (u32 i = 0; i < snapshotCount
            && i < DEBUG_TRACE_SNAPSHOT_MAX; i++) {
            u64 address = 0;
            if (!debugTraceComputeSnapshotAddress(&snapshots[i], gprs,
                    context.sp, context.pc.x, context.lr, &address)) {
                out->snapshotResults[i] =
                    (u32)MAKERESULT(Module_Libnx, LibnxError_BadInput);
                continue;
            }
            out->snapshotAddresses[i] = address;
            Result readRc = svcReadDebugProcessMemory(out->snapshots[i],
                debugHandle, address, snapshots[i].size);
            out->snapshotResults[i] = (u32)readRc;
            if (R_SUCCEEDED(readRc))
                out->snapshotBytes[i] = snapshots[i].size;
        }
    } else {
        for (u32 i = 0; i < snapshotCount
            && i < DEBUG_TRACE_SNAPSHOT_MAX; i++)
            out->snapshotResults[i] = (u32)contextRc;
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
    mutexLock(&traceState.mutex);
    traceState.hitCount++;
    if (traceState.durationFromFirstHit
        && traceState.durationStartTick == 0)
        traceState.durationStartTick = event->tick;
    event->sequence = traceState.hitCount;
    if (event->targetIndex < traceState.targetCount)
        traceState.targetHits[event->targetIndex]++;
    traceState.lastEvent = *event;
    traceState.hasEvent = true;
    if (traceState.ringSize > 0) {
        traceState.ring[traceState.ringNext] = *event;
        traceState.ringNext = (traceState.ringNext + 1)
            % traceState.ringSize;
        if (traceState.ringCount < traceState.ringSize)
            traceState.ringCount++;
    }
    traceState.lastPc = event->pc;
    traceState.lastLr = event->lr;
    traceState.lastSp = event->sp;
    traceState.lastThreadId = event->threadId;
    traceState.lastCore = event->core;
    mutexUnlock(&traceState.mutex);
}

static void recordIgnoredHit(void)
{
    mutexLock(&traceState.mutex);
    traceState.ignoredHits++;
    mutexUnlock(&traceState.mutex);
}

static void traceThreadMain(void* arg)
{
    (void)arg;
    Handle debugHandle = INVALID_HANDLE;
    bool attached = false;
    bool resumed = false;
    bool processExited = false;
    bool eventPending = false;
    bool traceArmed = false;
    Result rc = 0;
    Result cleanupRc = 0;
    char completionStage[sizeof(traceState.stage)];
    u64 pid = 0;
    u32 argsCount = 0;
    u32 targetCount = 0;
    u32 snapshotCount = 0;
    u64 targets[DEBUG_TRACE_TARGET_MAX];
    u32 instructionSlots[DEBUG_TRACE_TARGET_MAX];
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 targetSnapshotCounts[DEBUG_TRACE_TARGET_MAX];
    DebugTraceSnapshotSpec targetSnapshots[DEBUG_TRACE_TARGET_MAX]
        [DEBUG_TRACE_SNAPSHOT_MAX];

    mutexLock(&traceState.mutex);
    pid = traceState.processId;
    argsCount = traceState.argsCount;
    targetCount = traceState.targetCount;
    snapshotCount = traceState.snapshotCount;
    memcpy(targetSnapshotCounts, traceState.targetSnapshotCounts,
        sizeof(targetSnapshotCounts));
    memcpy(targetSnapshots, traceState.targetSnapshots,
        sizeof(targetSnapshots));
    memcpy(targets, traceState.targetAddresses, sizeof(targets));
    memcpy(snapshots, traceState.snapshots, sizeof(snapshots));
    mutexUnlock(&traceState.mutex);

    if (pid == 0) {
        setError("pid", MAKERESULT(Module_Libnx, LibnxError_BadInput));
        goto cleanup;
    }

    const bool dmntClosed = detachDmntForApplication(pid);
    mutexLock(&traceState.mutex);
    traceState.dmntClosed = dmntClosed;
    mutexUnlock(&traceState.mutex);

    rc = svcDebugActiveProcess(&debugHandle, pid);
    if (R_FAILED(rc)) {
        setError("debugActiveProcess", rc);
        if (rc == KERNELRESULT(AlreadyExists))
            setDebugHandleInUseHint();
        goto cleanup;
    }
    attached = true;

    rc = drainInitialEvents(debugHandle, &processExited);
    if (R_FAILED(rc)) {
        setError("drainInitial", rc);
        goto cleanup;
    }
    if (processExited) {
        setStage("exitProcess");
        rc = 0;
        goto cleanup;
    }

    u32 contextSlot = 0;
    u32 slotCount = 0;
    rc = debugHwBpProbeContextSlots(&contextSlot, 0);
    if (R_FAILED(rc)) {
        setError("probeContext", rc);
        goto cleanup;
    }
    rc = debugHwBpProbeInstructionSlots(contextSlot, instructionSlots,
        DEBUG_TRACE_TARGET_MAX, &slotCount);
    if (R_FAILED(rc)) {
        setError("probeInstructionSlots", rc);
        goto cleanup;
    }
    if (slotCount < targetCount) {
        setError("targetSlots",
            MAKERESULT(Module_Libnx, LibnxError_NotFound));
        goto cleanup;
    }

    mutexLock(&traceState.mutex);
    traceState.contextSlot = contextSlot;
    traceState.instructionSlot = instructionSlots[0];
    memcpy(traceState.instructionSlots, instructionSlots,
        sizeof(instructionSlots));
    mutexUnlock(&traceState.mutex);

    rc = debugHwBpArmInstructionTrace(contextSlot, debugHandle,
        instructionSlots, targets, targetCount);
    if (R_FAILED(rc)) {
        setError("armTrace", rc);
        goto cleanup;
    }
    traceArmed = true;

    rc = debugHwBpContinueAll(debugHandle);
    if (R_FAILED(rc)) {
        setError("resume", rc);
        goto cleanup;
    }
    resumed = true;

    mutexLock(&traceState.mutex);
    traceState.armed = true;
    traceState.everArmed = true;
    traceState.startedTick = armGetSystemTick();
    traceState.lastError = 0;
    traceState.stage[0] = 0;
    traceState.hint[0] = 0;
    mutexUnlock(&traceState.mutex);

    while (!stopRequested()) {
        if (durationExpired()) {
            setStage("durationReached");
            break;
        }
        s32 index = -1;
        rc = svcWaitSynchronization(&index, &debugHandle, 1,
            DEBUG_TRACE_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc)) {
            setError("waitEvent", rc);
            break;
        }

        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
            eventPending = true;

            if (event.type == DebugEventType_ExitProcess) {
                processExited = true;
                setStage("exitProcess");
                rc = 0;
                goto cleanup;
            }

            if (event.type == DebugEventType_Exception
                && event.info.exception.type == DebugException_BreakPoint
                && event.info.exception.specific.break_point.type
                    == BreakPointType_HardwareInstruction) {
                const u32 targetIndex = debugTraceFindTarget(targets,
                    targetCount,
                    (u64)event.info.exception.address);
                if (targetIndex == UINT32_MAX) {
                    recordIgnoredHit();
                    continue;
                }
                DebugTraceEvent captured;
                const u32 eventSnapshotCount = targetSnapshotCounts[targetIndex]
                    ? targetSnapshotCounts[targetIndex] : snapshotCount;
                const DebugTraceSnapshotSpec* eventSnapshots =
                    targetSnapshotCounts[targetIndex]
                    ? targetSnapshots[targetIndex] : snapshots;
                captureEvent(debugHandle, &event, targetIndex,
                    targets[targetIndex], argsCount, eventSnapshots,
                    eventSnapshotCount, &captured);
                recordEvent(&captured);
                if (hitLimitReached()) {
                    setStage("hitsReached");
                    rc = 0;
                    goto cleanup;
                }
                rc = debugHwBpStepOverInstruction(debugHandle,
                    event.thread_id, instructionSlots[targetIndex],
                    contextSlot, targets[targetIndex]);
                if (R_FAILED(rc)) {
                    setError("stepInstruction", rc);
                    goto cleanup;
                }
            }
        }

        rc = debugHwBpContinueAll(debugHandle);
        if (R_FAILED(rc)) {
            setError("continue", rc);
            break;
        }
        eventPending = false;
    }
    /* A stop request or duration expiry normally leaves the last wait result
     * as TimedOut. That is the loop tick, not a cleanup failure. */
    rc = 0;

cleanup:
    mutexLock(&traceState.mutex);
    memset(completionStage, 0, sizeof(completionStage));
    const size_t completionLength = strnlen(traceState.stage,
        sizeof(traceState.stage) - 1);
    memcpy(completionStage, traceState.stage, completionLength);
    mutexUnlock(&traceState.mutex);
    setStage("cleanup");
    if (attached && !processExited)
        debugHwBpQuiesce(debugHandle, !resumed || eventPending);

    if (traceArmed) {
        u32 registers[DEBUG_TRACE_TARGET_MAX + 1];
        registers[0] = traceState.contextSlot;
        for (u32 i = 0; i < targetCount; i++)
            registers[i + 1] = instructionSlots[i];
        Result clearRc = debugHwBpClearRegistersAllCores(registers,
            targetCount + 1);
        if (R_FAILED(clearRc) && R_SUCCEEDED(cleanupRc))
            cleanupRc = clearRc;
    }
    if (attached && debugHandle != INVALID_HANDLE)
        svcCloseHandle(debugHandle);

    mutexLock(&traceState.mutex);
    traceState.active = false;
    traceState.armed = false;
    traceState.stopRequested = false;
    if (R_FAILED(cleanupRc) && traceState.lastError == 0) {
        traceState.lastError = cleanupRc;
        strncpy(traceState.stage, "cleanup", sizeof(traceState.stage) - 1);
        traceState.stage[sizeof(traceState.stage) - 1] = 0;
    } else if (R_SUCCEEDED(cleanupRc)) {
        strncpy(traceState.stage, completionStage,
            sizeof(traceState.stage) - 1);
        traceState.stage[sizeof(traceState.stage) - 1] = 0;
    }
    if (R_FAILED(rc) && traceState.lastError == 0)
        traceState.lastError = rc;
    mutexUnlock(&traceState.mutex);
    threadExit();
}

void debugTraceInitialize(void)
{
    memset(&traceState, 0, sizeof(traceState));
    mutexInit(&traceState.mutex);
    traceState.durationSeconds = DEBUG_TRACE_DEFAULT_DURATION_SECONDS;
}

bool debugTraceStart(const DebugTraceConfig* config)
{
    if (config == 0)
        return false;
    if (config->processId == 0) {
        mutexLock(&traceState.mutex);
        traceState.lastError =
            MAKERESULT(Module_Libnx, LibnxError_BadInput);
        traceState.stage[0] = 0;
        mutexUnlock(&traceState.mutex);
        return false;
    }

    DebugTraceConfigModel model = {
        .targetCount = config->targetCount,
        .maxHits = config->maxHits,
        .durationSeconds = config->durationSeconds,
        .ringSize = config->ringSize,
        .argsCount = config->argsCount,
        .snapshotCount = config->snapshotCount,
    };
    memcpy(model.addresses, config->addresses, sizeof(model.addresses));
    memcpy(model.snapshots, config->snapshots, sizeof(model.snapshots));

    if (debugWatchIsActive() || debugMultiTraceIsActive()) {
        mutexLock(&traceState.mutex);
        traceState.lastError = KERNELRESULT(OwnedByAnotherProcess);
        strncpy(traceState.stage, "watchActive",
            sizeof(traceState.stage) - 1);
        traceState.stage[sizeof(traceState.stage) - 1] = 0;
        mutexUnlock(&traceState.mutex);
        return false;
    }

    mutexLock(&traceState.mutex);
    if (traceState.active) {
        mutexUnlock(&traceState.mutex);
        return false;
    }
    if (traceState.threadStarted) {
        mutexUnlock(&traceState.mutex);
        threadWaitForExit(&traceState.thread);
        threadClose(&traceState.thread);
        mutexLock(&traceState.mutex);
        traceState.threadStarted = false;
    }
    if (!debugTraceConfigIsValid(&model)) {
        traceState.lastError =
            MAKERESULT(Module_Libnx, LibnxError_BadInput);
        mutexUnlock(&traceState.mutex);
        return false;
    }
    for (u32 target = 0; target < config->targetCount; target++) {
        if (config->targetSnapshotCounts[target]
                > DEBUG_TRACE_SNAPSHOT_MAX) {
            traceState.lastError =
                MAKERESULT(Module_Libnx, LibnxError_BadInput);
            mutexUnlock(&traceState.mutex);
            return false;
        }
        for (u32 snapshot = 0;
            snapshot < config->targetSnapshotCounts[target]; snapshot++) {
            if (!debugTraceSnapshotSpecIsValid(
                    &config->targetSnapshots[target][snapshot])) {
                traceState.lastError =
                    MAKERESULT(Module_Libnx, LibnxError_BadInput);
                mutexUnlock(&traceState.mutex);
                return false;
            }
        }
    }

    traceState.processId = config->processId;
    traceState.targetCount = config->targetCount;
    memcpy(traceState.targetAddresses, config->addresses,
        sizeof(traceState.targetAddresses));
    memset(traceState.targetHits, 0, sizeof(traceState.targetHits));
    memset(traceState.instructionSlots, 0,
        sizeof(traceState.instructionSlots));
    traceState.address = config->addresses[0];
    traceState.maxHits = config->maxHits;
    traceState.durationSeconds = config->durationSeconds;
    traceState.startedTick = 0;
    traceState.durationStartTick = 0;
    traceState.durationFromFirstHit = config->durationFromFirstHit;
    traceState.hitCount = 0;
    traceState.ringSize = config->ringSize;
    traceState.ringCount = 0;
    traceState.ringNext = 0;
    traceState.argsCount = config->argsCount;
    traceState.contextSlot = 0;
    traceState.instructionSlot = 0;
    traceState.ignoredHits = 0;
    traceState.snapshotCount = config->snapshotCount;
    memcpy(traceState.snapshots, config->snapshots,
        sizeof(traceState.snapshots));
    memcpy(traceState.targetSnapshotCounts, config->targetSnapshotCounts,
        sizeof(traceState.targetSnapshotCounts));
    memcpy(traceState.targetSnapshots, config->targetSnapshots,
        sizeof(traceState.targetSnapshots));
    traceState.hasEvent = false;
    traceState.dmntClosed = false;
    traceState.lastPc = 0;
    traceState.lastLr = 0;
    traceState.lastSp = 0;
    traceState.lastThreadId = 0;
    traceState.lastCore = 0;
    traceState.lastError = 0;
    traceState.stage[0] = 0;
    traceState.hint[0] = 0;
    traceState.stopRequested = false;
    traceState.active = true;
    traceState.armed = false;
    traceState.everArmed = false;
    traceState.threadStarted = true;
    mutexUnlock(&traceState.mutex);

    Result rc = threadCreate(&traceState.thread, traceThreadMain, 0, 0,
        DEBUG_TRACE_THREAD_STACK_SIZE, DEBUG_TRACE_THREAD_PRIORITY, -2);
    if (R_FAILED(rc)) {
        mutexLock(&traceState.mutex);
        traceState.active = false;
        traceState.threadStarted = false;
        traceState.lastError = rc;
        mutexUnlock(&traceState.mutex);
        return false;
    }
    rc = threadStart(&traceState.thread);
    if (R_FAILED(rc)) {
        mutexLock(&traceState.mutex);
        traceState.active = false;
        traceState.threadStarted = false;
        traceState.lastError = rc;
        mutexUnlock(&traceState.mutex);
        threadClose(&traceState.thread);
        return false;
    }
    return true;
}

void debugTraceStop(void)
{
    mutexLock(&traceState.mutex);
    const bool running = traceState.active || traceState.threadStarted;
    traceState.stopRequested = true;
    mutexUnlock(&traceState.mutex);
    if (!running)
        return;

    threadWaitForExit(&traceState.thread);
    threadClose(&traceState.thread);

    mutexLock(&traceState.mutex);
    traceState.threadStarted = false;
    traceState.active = false;
    traceState.armed = false;
    traceState.stopRequested = false;
    mutexUnlock(&traceState.mutex);
}

bool debugTraceGetStatus(DebugTraceStatus* out)
{
    memset(out, 0, sizeof(*out));
    mutexLock(&traceState.mutex);
    out->active = traceState.active;
    out->armed = traceState.armed;
    out->everArmed = traceState.everArmed;
    out->stale = !traceState.active
        && (traceState.address != 0 || traceState.hitCount != 0);
    out->memoryBlocked = traceState.active;
    out->dmntClosed = traceState.dmntClosed;
    out->processId = traceState.processId;
    out->address = traceState.address;
    out->targetCount = traceState.targetCount;
    memcpy(out->targetAddresses, traceState.targetAddresses,
        sizeof(out->targetAddresses));
    memcpy(out->targetHits, traceState.targetHits,
        sizeof(out->targetHits));
    memcpy(out->instructionSlots, traceState.instructionSlots,
        sizeof(out->instructionSlots));
    out->maxHits = traceState.maxHits;
    out->hitCount = traceState.hitCount;
    out->durationSeconds = traceState.durationSeconds;
    out->startedTick = traceState.startedTick;
    out->durationStartTick = traceState.durationStartTick;
    out->durationFromFirstHit = traceState.durationFromFirstHit;
    out->ringSize = traceState.ringSize;
    out->ringCount = traceState.ringCount;
    out->argsCount = traceState.argsCount;
    out->contextSlot = traceState.contextSlot;
    out->instructionSlot = traceState.instructionSlot;
    out->ignoredHits = traceState.ignoredHits;
    out->snapshotCount = traceState.snapshotCount;
    memcpy(out->snapshots, traceState.snapshots,
        sizeof(out->snapshots));
    memcpy(out->targetSnapshotCounts, traceState.targetSnapshotCounts,
        sizeof(out->targetSnapshotCounts));
    memcpy(out->targetSnapshots, traceState.targetSnapshots,
        sizeof(out->targetSnapshots));
    out->lastSequence = traceState.hasEvent
        ? traceState.lastEvent.sequence : 0;
    out->lastPc = traceState.lastPc;
    out->lastLr = traceState.lastLr;
    out->lastSp = traceState.lastSp;
    out->lastThreadId = traceState.lastThreadId;
    out->lastCore = traceState.lastCore;
    out->lastError = traceState.lastError;
    strncpy(out->stage, traceState.stage, sizeof(out->stage) - 1);
    out->stage[sizeof(out->stage) - 1] = 0;
    strncpy(out->hint, traceState.hint, sizeof(out->hint) - 1);
    out->hint[sizeof(out->hint) - 1] = 0;
    mutexUnlock(&traceState.mutex);
    return true;
}

bool debugTraceGetEvent(u32 index, DebugTraceEvent* out)
{
    mutexLock(&traceState.mutex);
    if (!traceState.hasEvent) {
        mutexUnlock(&traceState.mutex);
        return false;
    }
    if (traceState.ringSize == 0) {
        if (index != 0) {
            mutexUnlock(&traceState.mutex);
            return false;
        }
        *out = traceState.lastEvent;
        mutexUnlock(&traceState.mutex);
        return true;
    }

    const u32 stored = debugTraceRingOldestIndex(traceState.ringNext,
        traceState.ringSize, traceState.ringCount, index);
    if (stored == UINT32_MAX) {
        mutexUnlock(&traceState.mutex);
        return false;
    }
    *out = traceState.ring[stored];
    mutexUnlock(&traceState.mutex);
    return true;
}

bool debugTraceIsActive(void)
{
    mutexLock(&traceState.mutex);
    const bool active = traceState.active;
    mutexUnlock(&traceState.mutex);
    return active;
}
