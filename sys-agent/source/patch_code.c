#include "patch_code.h"

#include <string.h>

#include "debug_watch.h"
#include "dmnt_client.h"

#define PATCH_CODE_EVENT_TIMEOUT_NS 20000000LL
#define PATCH_CODE_MAX_THREADS 0x80
#define PATCH_CODE_ERR_EXITED MAKERESULT(Module_Libnx, 0x2A1)

/* Same continue flags as debug_watch (dmnt.gen2 semantics). */
#define PATCH_CONTINUE_FLAG_EXCEPTION_HANDLED (1u << 0)
#define PATCH_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT (1u << 1)
#define PATCH_CONTINUE_FLAG_CONTINUE_ALL (1u << 2)
#define PATCH_CONTINUE_FLAGS \
    (PATCH_CONTINUE_FLAG_EXCEPTION_HANDLED \
     | PATCH_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT \
     | PATCH_CONTINUE_FLAG_CONTINUE_ALL)

static Result resumeSession(Handle debugHandle)
{
    u64 thread_ids[] = { 0 };
    return svcContinueDebugEvent(debugHandle, PATCH_CONTINUE_FLAGS,
        thread_ids, 1);
}

/* Drain the initial attach events; the kernel requires the event queue to be
 * empty before svcContinueDebugEvent succeeds. */
static Result drainInitialEvents(Handle debugHandle)
{
    const u64 deadline = armGetSystemTick() + armGetSystemTickFreq(); /* 1 s */
    bool attachedSeen = false;

    while (true) {
        s32 index = -1;
        Result rc = svcWaitSynchronization(&index, &debugHandle, 1,
            PATCH_CODE_EVENT_TIMEOUT_NS);
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
            if (event.type == DebugEventType_ExitProcess)
                return PATCH_CODE_ERR_EXITED;
            if (event.type == DebugEventType_CreateProcess)
                attachedSeen = true;
        }
    }
}

/* Stop the process with svcBreakDebugProcess and consume the break event so
 * the process is left stopped (contexts and debug memory become readable). */
static Result pauseProcess(Handle debugHandle)
{
    Result rc = svcBreakDebugProcess(debugHandle);
    if (R_FAILED(rc))
        return rc;

    for (s32 attempt = 0; attempt < 50; attempt++) {
        s32 index = -1;
        rc = svcWaitSynchronization(&index, &debugHandle, 1,
            PATCH_CODE_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc))
            return rc;

        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
            if (event.type == DebugEventType_ExitProcess)
                return PATCH_CODE_ERR_EXITED;
        }
        /* The break event has been consumed; the process is now stopped. */
        return 0;
    }
    return KERNELRESULT(TimedOut);
}

static u64 distanceToRange(u64 pc, u64 address, u64 size)
{
    if (pc < address)
        return address - pc;
    if (pc < address + size)
        return 0;
    return pc - (address + size);
}

Result patchCodeRun(u64 address, const u8* expected, u64 expectedSize,
    bool skipVerify, const u8* patch, u64 patchSize, bool checkPc, u64 pid,
    PatchCodeResult* out)
{
    memset(out, 0, sizeof(*out));

    if (patch == NULL || patchSize == 0 || patchSize > PATCH_CODE_MAX_SIZE)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!skipVerify && (expected == NULL || expectedSize != patchSize))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (debugWatchIsActive())
        return KERNELRESULT(OwnedByAnotherProcess);

    /* gen1 (dmnt:cht) owns the single debug handle when attached; close it so
     * we can attach directly.  It re-attaches automatically after detach. */
    if (R_SUCCEEDED(dmntClientInitialize())) {
        bool attached = false;
        if (R_SUCCEEDED(dmntClientHasProcess(&attached)) && attached) {
            dmntClientForceClose();
            out->dmntClosed = true;
        }
    }

    Result rc = 0;
    if (pid == 0)
        rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0) {
        if (R_SUCCEEDED(rc))
            rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        return rc;
    }

    Handle debugHandle = INVALID_HANDLE;
    bool attachedHandle = false;
    bool paused = false;
    bool resumed = false;

    rc = svcDebugActiveProcess(&debugHandle, pid);
    if (R_FAILED(rc))
        return rc;
    attachedHandle = true;

    rc = drainInitialEvents(debugHandle);
    if (R_FAILED(rc))
        goto cleanup;

    rc = pauseProcess(debugHandle);
    if (R_FAILED(rc))
        goto cleanup;
    paused = true;

    out->patchSize = patchSize;

    /* Inspect every thread's PC while the process is stopped. */
    u64 threadIds[PATCH_CODE_MAX_THREADS];
    s32 threadCount = 0;
    rc = svcGetThreadList(&threadCount, threadIds,
        PATCH_CODE_MAX_THREADS, debugHandle);
    if (R_FAILED(rc))
        goto cleanup;

    u64 bestDist = UINT64_MAX;
    s32 i = 0;
    for (i = 0; i < threadCount && i < PATCH_CODE_MAX_THREADS; i++) {
        ThreadContext ctx;
        if (R_FAILED(svcGetDebugThreadContext(&ctx, debugHandle,
                threadIds[i], RegisterGroup_CpuAll)))
            continue;
        out->pausedThreads++;
        const u64 pc = ctx.pc.x;
        const u64 dist = distanceToRange(pc, address, patchSize);
        if (dist < bestDist) {
            bestDist = dist;
            out->nearestPc = pc;
        }
        if (checkPc && pc >= address && pc < address + patchSize) {
            out->status = PatchCodeBusyPcInRange;
            out->pcHitThread = threadIds[i];
            out->pcHitValue = pc;
            goto cleanup;
        }
    }

    /* Verify the original bytes, write, and read back.  A failed read is a
     * hard error: never proceed (or report success) without being able to
     * verify the memory. */
    rc = svcReadDebugProcessMemory(out->oldBytes, debugHandle, address,
        patchSize);
    if (R_FAILED(rc))
        goto cleanup;
    if (!skipVerify && memcmp(out->oldBytes, expected, patchSize) != 0) {
        out->status = PatchCodeExpectedMismatch;
        goto cleanup;
    }

    rc = svcWriteDebugProcessMemory(debugHandle, patch, address, patchSize);
    if (R_FAILED(rc))
        goto cleanup;

    memset(out->newBytes, 0, sizeof(out->newBytes));
    rc = svcReadDebugProcessMemory(out->newBytes, debugHandle, address,
        patchSize);
    if (R_FAILED(rc))
        goto cleanup;
    if (memcmp(out->newBytes, patch, patchSize) != 0) {
        out->status = PatchCodeReadbackFailed;
        goto cleanup;
    }

    out->status = PatchCodeOk;

cleanup:
    /* Resume must never be skipped once the process was stopped.  Closing
     * the debug handle also resumes the target, but do it explicitly for a
     * deterministic state machine. */
    if (attachedHandle && paused && !resumed) {
        while (true) {
            DebugEventInfo event;
            if (R_FAILED(svcGetDebugEvent(&event, debugHandle)))
                break;
        }
        if (R_SUCCEEDED(resumeSession(debugHandle)))
            resumed = true;
    }
    if (attachedHandle && debugHandle != INVALID_HANDLE)
        svcCloseHandle(debugHandle);

    return rc;
}
