#include <string.h>

#include "debug_hwbp_layout.h"
#include "debug_process.h"

Result debugHwBpProbeLayout(u64 pid1, u64 pid2, DebugHwBpLayout* out)
{
    if (out == NULL)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(out, 0, sizeof(*out));
    out->processId1 = pid1;
    out->processId2 = pid2;
    if (pid1 == 0 || pid2 == 0 || pid1 == pid2)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Handle debug1 = INVALID_HANDLE;
    Handle debug2 = INVALID_HANDLE;
    bool dmntClosed = false;
    bool armed1 = false;
    bool armed2 = false;
    Result rc = 0;

    rc = debugProcessOpen(pid1, &debug1, &dmntClosed);
    if (R_FAILED(rc))
        goto cleanup;
    rc = debugProcessOpen(pid2, &debug2, &dmntClosed);
    if (R_FAILED(rc))
        goto cleanup;

    out->drainResult = debugProcessDrain(debug1);
    if (R_FAILED(out->drainResult)) {
        rc = out->drainResult;
        goto cleanup;
    }
    out->drainResult = debugProcessDrain(debug2);
    if (R_FAILED(out->drainResult)) {
        rc = out->drainResult;
        goto cleanup;
    }

    out->resumeResult = debugProcessResume(debug1);
    if (R_FAILED(out->resumeResult)) {
        rc = out->resumeResult;
        goto cleanup;
    }
    out->resumeResult = debugProcessResume(debug2);
    if (R_FAILED(out->resumeResult)) {
        rc = out->resumeResult;
        goto cleanup;
    }
    u32 firstContext = 0;
    u32 secondContext = 0;
    rc = debugHwBpProbeContextSlots(&firstContext, &secondContext);
    if (R_FAILED(rc))
        goto cleanup;
    out->contextSlot1 = firstContext;
    out->contextSlot2 = secondContext;

    u32 allSlots[DEBUG_HWBP_LAYOUT_SLOTS_MAX];
    u32 allCount = 0;
    rc = debugHwBpProbeInstructionSlots(firstContext, allSlots,
        DEBUG_HWBP_LAYOUT_SLOTS_MAX, &allCount);
    if (R_FAILED(rc))
        goto cleanup;
    for (u32 i = 0; i < allCount; i++) {
        if (allSlots[i] == secondContext)
            continue;
        out->instructionSlots[out->instructionSlotCount++] = allSlots[i];
    }

    out->contextSlot1Result =
        debugHwBpSetContext(firstContext, debug1);
    if (R_FAILED(out->contextSlot1Result)) {
        rc = out->contextSlot1Result;
        goto cleanup;
    }
    armed1 = true;
    out->contextSlot2Result =
        debugHwBpSetContext(secondContext, debug2);
    if (R_FAILED(out->contextSlot2Result)) {
        rc = out->contextSlot2Result;
        goto cleanup;
    }
    armed2 = true;

cleanup:
    if (armed2) {
        Result clearRc = debugHwBpClearAllCores(out->contextSlot2);
        if (R_FAILED(clearRc) && R_SUCCEEDED(out->cleanupResult))
            out->cleanupResult = clearRc;
    }
    if (armed1) {
        Result clearRc = debugHwBpClearAllCores(out->contextSlot1);
        if (R_FAILED(clearRc) && R_SUCCEEDED(out->cleanupResult))
            out->cleanupResult = clearRc;
    }
    if (debug2 != INVALID_HANDLE)
        debugProcessClose(debug2);
    if (debug1 != INVALID_HANDLE)
        debugProcessClose(debug1);

    (void)dmntClosed;
    return rc;
}
