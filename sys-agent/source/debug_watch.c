#include <string.h>
#include <stdio.h>

#include "debug_watch.h"

/*
 * Hardware watchpoint support (generic in-process debugging primitive).
 *
 * The NPDM already grants every debug syscall (svcDebugActiveProcess,
 * svcGetDebugEvent, svcContinueDebugEvent, svcGetDebugThreadContext,
 * svcSetHardwareBreakPoint, svcReadDebugProcessMemory) and force_debug, so
 * this module attaches the current application directly and links a data
 * watchpoint through a context-IDR breakpoint, following Atmosphere
 * dmnt.gen2's HardwareWatchPointManager
 * (dmnt2_hardware_watchpoint.cpp + dmnt2_hardware_breakpoint.cpp).
 *
 * Review notes (2026-08-23, external expert):
 * - Hardware debug registers are per-CPU; the watch thread must migrate to
 *   every core and treat "actually running on the target core" as a
 *   postcondition before programming the register.
 * - The context-IDR slot is discovered at runtime (never an invariant); the
 *   discovered slots are reported by `debug watch-status`.
 * - The process may be stopped at a pending debug exception when we detach
 *   (hit limit / stop / timeout); explicitly continue that pending event
 *   before clearing the breakpoints and closing the handle.
 * - `svcContinueDebugEvent` flags keep the symbolic Atmosphere names instead
 *   of a magic number.
 * - Initial debug events (CreateProcess / CreateThread / DebuggerAttached)
 *   are drained and continued before the watchpoint is armed.
 * - A duration failsafe detaches the session even if the control client
 *   disconnects.
 */

#define DEBUG_WATCH_THREAD_STACK_SIZE 0x10000
#define DEBUG_WATCH_THREAD_PRIORITY 0x30
#define DEBUG_WATCH_EVENT_TIMEOUT_NS 20000000LL
#define DEBUG_WATCH_PIN_RETRY_US 100LL
#define DEBUG_WATCH_PIN_ATTEMPTS 64
#define DEBUG_WATCH_DEFAULT_DURATION_SECONDS 60
#define DEBUG_WATCH_CORE_COUNT 4
#define DEBUG_WATCH_DEFAULT_CORE 3

/* svcSetHardwareBreakPoint register bank (same indices as dmnt.gen2). */
#define HW_BP_REGISTER_I0 0
#define HW_BP_REGISTER_I15 15
#define HW_BP_REGISTER_D0 16
#define HW_BP_REGISTER_D15 31

/*
 * svcContinueDebugEvent flags, mirroring ams::svc::ContinueFlag in the
 * Atmosphere tree (dmnt.gen2 uses ExceptionHandled | EnableExceptionEvent |
 * ContinueAll to resume every thread after an exception).
 */
#define DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED (1u << 0)
#define DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT (1u << 1)
#define DEBUG_CONTINUE_FLAG_CONTINUE_ALL (1u << 2)
#define DEBUG_CONTINUE_FLAGS \
    (DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED \
     | DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT \
     | DEBUG_CONTINUE_FLAG_CONTINUE_ALL)

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadStarted;
    bool active;
    bool armed;
    bool stopRequested;
    bool hasHit;
    u64 processId;
    u64 watchAddress;
    u64 watchSize;
    u32 maxHits;
    u32 hitCount;
    u32 ctxSlot;
    u32 wpSlot;
    u64 durationSeconds;
    u64 startedTick;
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastDataAddress;
    u64 lastThreadId;
    DebugWatchHit lastHit;
    Result lastError;
    char stage[24];
    char hint[160];
} DebugWatchState;

static DebugWatchState watchState;

static void setError(const char* stage, Result rc)
{
    mutexLock(&watchState.mutex);
    watchState.lastError = rc;
    strncpy(watchState.stage, stage, sizeof(watchState.stage) - 1);
    watchState.stage[sizeof(watchState.stage) - 1] = 0;
    watchState.hint[0] = 0;
    mutexUnlock(&watchState.mutex);
}

static void setStage(const char* stage)
{
    mutexLock(&watchState.mutex);
    strncpy(watchState.stage, stage, sizeof(watchState.stage) - 1);
    watchState.stage[sizeof(watchState.stage) - 1] = 0;
    watchState.hint[0] = 0;
    mutexUnlock(&watchState.mutex);
}

static void setDebugHandleInUseHint(void)
{
    mutexLock(&watchState.mutex);
    strncpy(watchState.hint,
        "debug handle still owned by another debugger (e.g. gdbstub); "
        "gen1/dmnt:cht conflicts are auto-closed, so check that "
        "enable_standalone_gdbstub is disabled",
        sizeof(watchState.hint) - 1);
    watchState.hint[sizeof(watchState.hint) - 1] = 0;
    mutexUnlock(&watchState.mutex);
}

static bool isValidWatchPoint(u64 address, u64 size)
{
    if (size == 0)
        return false;
    if (size <= 8) {
        if ((address & ~7ULL) != ((address + size - 1) & ~7ULL))
            return false;
    } else {
        if (size > 0x80000000ULL)
            return false;
        if ((size & (size - 1)) != 0)
            return false;
        if ((address & (size - 1)) != 0)
            return false;
    }
    return true;
}

/* Pin the current thread to a single core and verify we actually run there. */
static Result pinToCore(const Handle current, s32 core)
{
    Result rc = svcSetThreadCoreMask(current, core, 1u << core);
    if (R_FAILED(rc))
        return rc;

    s32 attempt = 0;
    while ((s32)svcGetCurrentProcessorNumber() != core) {
        if (++attempt >= DEBUG_WATCH_PIN_ATTEMPTS)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        svcSleepThread(DEBUG_WATCH_PIN_RETRY_US * 1000LL);
    }
    return 0;
}

/*
 * Program one hardware-breakpoint register on all four cores.  Every core
 * transition is verified with svcGetCurrentProcessorNumber; on failure the
 * register is rolled back on the cores already programmed, and the original
 * affinity is restored before returning.
 */
static Result setBreakPointOnAllCores(u32 reg, u64 ctrl, u64 value)
{
    const Handle current = threadGetCurHandle();
    s32 originalCore = -1;
    u64 originalMask = 0;
    svcGetThreadCoreMask(&originalCore, &originalMask, current);

    Result rc = 0;
    s32 core = 0;
    s32 programmed = -1;
    for (core = 0; core < DEBUG_WATCH_CORE_COUNT; core++) {
        rc = pinToCore(current, core);
        if (R_FAILED(rc))
            break;
        rc = svcSetHardwareBreakPoint(reg, ctrl, value);
        if (R_FAILED(rc))
            break;
        programmed = core;
    }

    if (R_FAILED(rc) && programmed >= 0) {
        /* Roll back the partially programmed register. */
        s32 rollback = 0;
        for (rollback = 0; rollback <= programmed; rollback++) {
            if (R_SUCCEEDED(pinToCore(current, rollback)))
                svcSetHardwareBreakPoint(reg, 0, 0);
        }
    }

    /* Restore the affinity the thread had before this call. */
    if (originalCore >= 0 && originalMask != 0)
        svcSetThreadCoreMask(current, originalCore, (u32)originalMask);
    else
        svcSetThreadCoreMask(current, DEBUG_WATCH_DEFAULT_CORE,
            1u << DEBUG_WATCH_DEFAULT_CORE);
    return rc;
}

static Result setDataBreakPoint(u32 reg, u32 ctx, u64 address, u64 size,
    bool read, bool write)
{
    const u8 lsc = (read ? 1 : 0) | (write ? 2 : 0);
    if (lsc != 0 && !isValidWatchPoint(address, size))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    /* Determine bas/mask exactly like dmnt.gen2 SetDataBreakPoint. */
    u8 bas = 0, mask = 0;
    if (size <= 8) {
        bas = (u8)(((1u << size) - 1) << (address & 7));
        address = address & ~7ULL;
    } else {
        bas = 0xFF;
        mask = (u8)__builtin_popcountll((unsigned long long)(size - 1));
    }

    const u64 dbgbcr = ((u64)mask << 24) | ((u64)ctx << 16)
        | ((u64)bas << 5) | ((u64)lsc << 3) | ((lsc != 0) ? 1 : 0);
    return setBreakPointOnAllCores(reg, dbgbcr, address);
}

static Result setContextBreakPoint(u32 ctxReg, Handle debugHandle)
{
    const u64 dbgbcr = (0x3ULL << 20) | (0x0ULL << 16) | (0xFULL << 5) | 1;
    return setBreakPointOnAllCores(ctxReg, dbgbcr, (u64)debugHandle);
}

/* Resume the debug session exactly like dmnt.gen2's DebugProcess::Continue:
 * ExceptionHandled | EnableExceptionEvent | ContinueAll with a valid (dummy)
 * one-element thread-id list. */
static Result resumeDebugSession(Handle debugHandle)
{
    u64 thread_ids[] = { 0 };
    return svcContinueDebugEvent(debugHandle, DEBUG_CONTINUE_FLAGS,
        thread_ids, 1);
}

static Result clearBreakPointOnAllCores(u32 reg)
{
    return setBreakPointOnAllCores(reg, 0, 0);
}

/*
 * Discover a context-IDR-capable breakpoint slot at runtime.  The walk starts
 * at the highest valid instruction-breakpoint register and picks the highest
 * slot that accepts a context value; data watchpoints link to the slot after
 * it (this is dmnt.gen2's GetWatchPointContextRegister behaviour).  The
 * discovered slot is a property of the current HOS/Mariko, not a constant.
 */
static Result probeContextBreakpointSlot(u32* outCtxSlot)
{
    s32 lastBp = -1;
    s32 i = 0;
    for (i = HW_BP_REGISTER_I0; i <= HW_BP_REGISTER_I15; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        lastBp = i;
    }
    if (lastBp < 0)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    const u64 ctxDbgbcr = (0x3ULL << 20) | (0x0ULL << 16) | (0xFULL << 5) | 1;
    s32 firstCtx = -1;
    for (i = lastBp; i >= HW_BP_REGISTER_I0; i--) {
        Result rc = svcSetHardwareBreakPoint((u32)i, ctxDbgbcr,
            (u64)CUR_PROCESS_HANDLE);
        svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc)) {
            /* A failure other than invalid-handle ends the context range. */
            if (rc != KERNELRESULT(InvalidHandle))
                break;
        }
        firstCtx = i;
    }
    if (firstCtx < 0)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    *outCtxSlot = (u32)(firstCtx + 1);
    return 0;
}

static Result probeDataRegister(u32* outDataReg)
{
    s32 i = 0;
    for (i = HW_BP_REGISTER_D0; i <= HW_BP_REGISTER_D15; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        *outDataReg = (u32)i;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

/*
 * Consume the initial debug events of a fresh attach (CreateProcess,
 * CreateThread, DebuggerAttached, ...).  The kernel requires the debug-event
 * queue to be empty before svcContinueDebugEvent succeeds, so this function
 * only drains the queue; the caller resumes the session exactly once after
 * the watchpoint is armed.
 */
static Result drainInitialEvents(Handle debugHandle, bool* processExited)
{
    const u64 deadline = armGetSystemTick() + armGetSystemTickFreq(); /* 1 s */
    bool attachedSeen = false;

    while (true) {
        s32 index = -1;
        Result rc = svcWaitSynchronization(&index, &debugHandle, 1,
            DEBUG_WATCH_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut)) {
            if (attachedSeen || armGetSystemTick() >= deadline)
                return 0;
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
                && event.info.exception.type == DebugException_DebuggerAttached) {
                attachedSeen = true;
            }
        }
    }

    /* Final drain pass so the queue is definitely empty before the single
     * resume (the target is still stopped here, so no new events can arrive
     * in between). */
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

static void recordHit(const DebugWatchHit* hit)
{
    mutexLock(&watchState.mutex);
    watchState.lastHit = *hit;
    watchState.hasHit = true;
    watchState.lastPc = hit->pc;
    watchState.lastLr = hit->x[30];
    watchState.lastSp = hit->sp;
    watchState.lastDataAddress = hit->dataAddress;
    watchState.lastThreadId = hit->threadId;
    watchState.hitCount++;
    mutexUnlock(&watchState.mutex);
}

static bool stopRequested(void)
{
    mutexLock(&watchState.mutex);
    const bool stop = watchState.stopRequested;
    mutexUnlock(&watchState.mutex);
    return stop;
}

static bool durationExpired(void)
{
    mutexLock(&watchState.mutex);
    const u64 duration = watchState.durationSeconds;
    const u64 started = watchState.startedTick;
    mutexUnlock(&watchState.mutex);
    if (duration == 0)
        return false;
    return (armGetSystemTick() - started)
        >= (u64)armGetSystemTickFreq() * duration;
}

static bool hitLimitReached(void)
{
    mutexLock(&watchState.mutex);
    const bool done = watchState.hitCount >= watchState.maxHits;
    mutexUnlock(&watchState.mutex);
    return done;
}

static void captureHit(Handle debugHandle, const DebugEventInfo* event,
    DebugWatchHit* hit)
{
    memset(hit, 0, sizeof(*hit));
    hit->dataAddress = (u64)event->info.exception.address;
    hit->threadId = event->thread_id;

    ThreadContext ctx;
    if (R_SUCCEEDED(svcGetDebugThreadContext(&ctx, debugHandle,
            event->thread_id, RegisterGroup_CpuAll))) {
        s32 i = 0;
        for (i = 0; i < 29; i++)
            hit->x[i] = ctx.cpu_gprs[i].x;
        hit->x[29] = ctx.fp;
        hit->x[30] = ctx.lr;
        hit->sp = ctx.sp;
        hit->pc = ctx.pc.x;

        /* Read the instruction window at pc-8 .. pc+3 to verify the writer
         * with base_reg + immediate == watched address. */
        if (hit->pc >= 8) {
            u8 insn[12];
            if (R_SUCCEEDED(svcReadDebugProcessMemory(insn, debugHandle,
                    hit->pc - 8, sizeof(insn)))) {
                memcpy(hit->insn, insn, sizeof(insn));
                hit->insnBytes = sizeof(insn);
            }
        }

        /* Snapshot the stack while the target is still stopped at the
         * exception. The fp window (x29 upward) holds the saved frame
         * pointers / return addresses of every caller, so the host can walk
         * the call chain without racing the resumed process. */
        if (hit->x[29] != 0) {
            u8 stack[0x200];
            if (R_SUCCEEDED(svcReadDebugProcessMemory(stack, debugHandle,
                    hit->x[29], sizeof(stack)))) {
                memcpy(hit->fpStack, stack, sizeof(stack));
                hit->fpStackBytes = sizeof(stack);
            }
        }
        if (hit->sp != 0) {
            u8 stack[0x100];
            if (R_SUCCEEDED(svcReadDebugProcessMemory(stack, debugHandle,
                    hit->sp, sizeof(stack)))) {
                memcpy(hit->spStack, stack, sizeof(stack));
                hit->spStackBytes = sizeof(stack);
            }
        }
    }
}

static void debugWatchThreadMain(void* arg)
{
    (void)arg;
    Handle debugHandle = INVALID_HANDLE;
    u32 ctxSlot = 0, wpSlot = 0;
    bool attached = false;
    bool processExited = false;
    bool eventPending = false;
    bool ctxArmed = false;
    bool dataArmed = false;
    Result rc = 0;

    u64 pid = 0;
    rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0) {
        if (R_SUCCEEDED(rc))
            rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        setError("pmdmnt", rc);
        goto cleanup;
    }

    rc = svcDebugActiveProcess(&debugHandle, pid);
    if (R_FAILED(rc)) {
        setError("debugActiveProcess", rc);
        if (rc == KERNELRESULT(AlreadyExists))
            setDebugHandleInUseHint();
        goto cleanup;
    }
    attached = true;

    mutexLock(&watchState.mutex);
    watchState.processId = pid;
    mutexUnlock(&watchState.mutex);

    /* Drain the initial debug events; the single resume happens after the
     * watchpoint is armed below. */
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

    rc = probeContextBreakpointSlot(&ctxSlot);
    if (R_FAILED(rc)) {
        setError("probeContext", rc);
        goto cleanup;
    }
    rc = probeDataRegister(&wpSlot);
    if (R_FAILED(rc)) {
        setError("probeData", rc);
        goto cleanup;
    }

    mutexLock(&watchState.mutex);
    watchState.ctxSlot = ctxSlot;
    watchState.wpSlot = wpSlot;
    mutexUnlock(&watchState.mutex);

    rc = setContextBreakPoint(ctxSlot, debugHandle);
    if (R_FAILED(rc)) {
        setError("setContextBp", rc);
        goto cleanup;
    }
    ctxArmed = true;

    {
        u64 address = 0, size = 0;
        mutexLock(&watchState.mutex);
        address = watchState.watchAddress;
        size = watchState.watchSize;
        mutexUnlock(&watchState.mutex);
        rc = setDataBreakPoint(wpSlot, ctxSlot, address, size, false, true);
    }
    if (R_FAILED(rc)) {
        setError("setDataBp", rc);
        goto cleanup;
    }
    dataArmed = true;

    /* The initial events have been drained; resume the process exactly once
     * with the watchpoint already armed. */
    rc = resumeDebugSession(debugHandle);
    if (R_FAILED(rc)) {
        setError("resume", rc);
        goto cleanup;
    }

    mutexLock(&watchState.mutex);
    watchState.armed = true;
    watchState.lastError = 0;
    watchState.stage[0] = 0;
    watchState.hint[0] = 0;
    mutexUnlock(&watchState.mutex);

    while (!stopRequested() && !durationExpired()) {
        s32 index = -1;
        rc = svcWaitSynchronization(&index, &debugHandle, 1,
            DEBUG_WATCH_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc)) {
            setError("waitEvent", rc);
            break;
        }

        /* Consume every pending event first; svcContinueDebugEvent only
         * succeeds when the queue is empty. */
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
                    == BreakPointType_HardwareData) {
                DebugWatchHit hit;
                captureHit(debugHandle, &event, &hit);
                recordHit(&hit);
                if (hitLimitReached()) {
                    setStage("hitsReached");
                    rc = 0;
                    /* eventPending stays true: cleanup resumes the pending
                     * exception before detaching. */
                    goto cleanup;
                }
            }
        }

        /* Queue drained; resume the session once. */
        rc = resumeDebugSession(debugHandle);
        if (R_FAILED(rc)) {
            setError("continue", rc);
            break;
        }
        eventPending = false;
    }

cleanup:
    /* If the target may still be stopped on events we consumed but did not
     * resume (hit limit / stop / timeout / setup failure), drain the queue
     * and resume it explicitly so detach has a deterministic state machine.
     * This is best effort; closing the debug handle also resumes the target. */
    if (attached && !processExited && eventPending) {
        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
        }
        resumeDebugSession(debugHandle);
    }

    if (ctxArmed)
        clearBreakPointOnAllCores(ctxSlot);
    if (dataArmed)
        clearBreakPointOnAllCores(wpSlot);

    if (attached && debugHandle != INVALID_HANDLE)
        svcCloseHandle(debugHandle);

    mutexLock(&watchState.mutex);
    watchState.active = false;
    watchState.armed = false;
    watchState.stopRequested = false;
    if (R_FAILED(rc) && watchState.lastError == 0)
        watchState.lastError = rc;
    mutexUnlock(&watchState.mutex);
    threadExit();
}

void debugWatchInitialize(void)
{
    memset(&watchState, 0, sizeof(watchState));
    mutexInit(&watchState.mutex);
    watchState.lastError = 0;
    watchState.stage[0] = 0;
    watchState.durationSeconds = DEBUG_WATCH_DEFAULT_DURATION_SECONDS;
}

bool debugWatchStart(u64 address, u64 size, u32 maxHits, u64 durationSeconds)
{
    mutexLock(&watchState.mutex);
    if (watchState.active) {
        mutexUnlock(&watchState.mutex);
        return false;
    }
    if (watchState.threadStarted) {
        /* A previous session's thread already exited; reap it before
         * starting a new one. */
        mutexUnlock(&watchState.mutex);
        threadWaitForExit(&watchState.thread);
        threadClose(&watchState.thread);
        mutexLock(&watchState.mutex);
        watchState.threadStarted = false;
    }
    if (size == 0 || !isValidWatchPoint(address, size)) {
        watchState.lastError = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        mutexUnlock(&watchState.mutex);
        return false;
    }

    watchState.watchAddress = address;
    watchState.watchSize = size;
    watchState.maxHits = maxHits > 0 ? maxHits : 1;
    watchState.durationSeconds = durationSeconds;
    watchState.startedTick = armGetSystemTick();
    watchState.hitCount = 0;
    watchState.hasHit = false;
    watchState.lastPc = 0;
    watchState.lastLr = 0;
    watchState.lastSp = 0;
    watchState.lastDataAddress = 0;
    watchState.lastThreadId = 0;
    watchState.processId = 0;
    watchState.ctxSlot = 0;
    watchState.wpSlot = 0;
    watchState.lastError = 0;
    watchState.stage[0] = 0;
    watchState.hint[0] = 0;
    watchState.stopRequested = false;
    watchState.active = true;
    watchState.armed = false;
    watchState.threadStarted = true;
    mutexUnlock(&watchState.mutex);

    Result rc = threadCreate(&watchState.thread, debugWatchThreadMain, NULL,
        NULL, DEBUG_WATCH_THREAD_STACK_SIZE, DEBUG_WATCH_THREAD_PRIORITY, -2);
    if (R_FAILED(rc)) {
        mutexLock(&watchState.mutex);
        watchState.active = false;
        watchState.threadStarted = false;
        watchState.lastError = rc;
        mutexUnlock(&watchState.mutex);
        return false;
    }
    rc = threadStart(&watchState.thread);
    if (R_FAILED(rc)) {
        mutexLock(&watchState.mutex);
        watchState.active = false;
        watchState.threadStarted = false;
        watchState.lastError = rc;
        mutexUnlock(&watchState.mutex);
        threadClose(&watchState.thread);
        return false;
    }
    return true;
}

void debugWatchStop(void)
{
    mutexLock(&watchState.mutex);
    const bool running = watchState.active || watchState.threadStarted;
    watchState.stopRequested = true;
    mutexUnlock(&watchState.mutex);
    if (!running)
        return;

    threadWaitForExit(&watchState.thread);
    threadClose(&watchState.thread);

    mutexLock(&watchState.mutex);
    watchState.threadStarted = false;
    watchState.active = false;
    watchState.armed = false;
    watchState.stopRequested = false;
    mutexUnlock(&watchState.mutex);
}

bool debugWatchGetStatus(DebugWatchStatus* out)
{
    mutexLock(&watchState.mutex);
    out->active = watchState.active;
    out->armed = watchState.armed;
    out->processId = watchState.processId;
    out->watchAddress = watchState.watchAddress;
    out->watchSize = watchState.watchSize;
    out->maxHits = watchState.maxHits;
    out->hitCount = watchState.hitCount;
    out->ctxSlot = watchState.ctxSlot;
    out->wpSlot = watchState.wpSlot;
    out->durationSeconds = watchState.durationSeconds;
    out->lastPc = watchState.lastPc;
    out->lastLr = watchState.lastLr;
    out->lastSp = watchState.lastSp;
    out->lastDataAddress = watchState.lastDataAddress;
    out->lastThreadId = watchState.lastThreadId;
    out->lastError = watchState.lastError;
    strncpy(out->stage, watchState.stage, sizeof(out->stage) - 1);
    out->stage[sizeof(out->stage) - 1] = 0;
    strncpy(out->hint, watchState.hint, sizeof(out->hint) - 1);
    out->hint[sizeof(out->hint) - 1] = 0;
    mutexUnlock(&watchState.mutex);
    return true;
}

bool debugWatchGetLastHit(DebugWatchHit* out)
{
    mutexLock(&watchState.mutex);
    if (!watchState.hasHit) {
        mutexUnlock(&watchState.mutex);
        return false;
    }
    *out = watchState.lastHit;
    mutexUnlock(&watchState.mutex);
    return true;
}

bool debugWatchIsActive(void)
{
    mutexLock(&watchState.mutex);
    const bool active = watchState.active;
    mutexUnlock(&watchState.mutex);
    return active;
}

Result debugWatchResolveMainBase(u64* outMainBase)
{
    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0)
        return R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);

    LoaderModuleInfo modules[2];
    s32 count = 0;
    rc = ldrDmntGetProcessModuleInfo(pid, modules, 2, &count);
    if (R_FAILED(rc) || count <= 0)
        return R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    const LoaderModuleInfo* mainModule = &modules[count == 2 ? 1 : 0];
    *outMainBase = mainModule->base_address;
    return 0;
}
