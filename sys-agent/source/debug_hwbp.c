#include "debug_hwbp.h"

#include <string.h>

#define DEBUG_HWBP_PIN_RETRY_US 100LL
#define DEBUG_HWBP_PIN_ATTEMPTS 20000
#define DEBUG_HWBP_CLEAR_RETRIES 3
#define DEBUG_HWBP_DEFAULT_CORE 3
#define DEBUG_HWBP_BATCH_MAX 4
#define DEBUG_HWBP_DUAL_PROCESS_COUNT 2
#define DEBUG_HWBP_STEP_TIMEOUT_SECONDS 5
/* Atmosphere's svcSetDebugThreadContext extensions; libnx 4.12 does not
 * expose names for these two flags. */
#define DEBUG_THREAD_CONTEXT_FLAG_SET_SINGLE_STEP (1u << 30)
#define DEBUG_THREAD_CONTEXT_FLAG_CLEAR_SINGLE_STEP (1u << 31)

#define DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED (1u << 0)
#define DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT (1u << 1)
#define DEBUG_CONTINUE_FLAG_CONTINUE_ALL (1u << 2)
#define DEBUG_CONTINUE_FLAGS \
    (DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED \
     | DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT \
     | DEBUG_CONTINUE_FLAG_CONTINUE_ALL)

static Result pinToCore(const Handle current, s32 core)
{
    Result rc = svcSetThreadCoreMask(current, core, 1u << core);
    if (R_FAILED(rc))
        return rc;

    s32 attempt = 0;
    while ((s32)svcGetCurrentProcessorNumber() != core) {
        if (++attempt >= DEBUG_HWBP_PIN_ATTEMPTS)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        svcSleepThread(DEBUG_HWBP_PIN_RETRY_US * 1000LL);
    }
    return 0;
}

static Result setRegistersOnAllCores(const u32* registers,
    const u64* controls, const u64* values, u32 count)
{
    if (registers == NULL || controls == NULL || values == NULL
        || count == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const Handle current = threadGetCurHandle();
    s32 originalCore = -1;
    u64 originalMask = 0;
    svcGetThreadCoreMask(&originalCore, &originalMask, current);

    Result rc = 0;
    s32 reachedCore = -1;
    for (s32 core = 0; core < DEBUG_HWBP_CORE_COUNT; core++) {
        rc = pinToCore(current, core);
        if (R_FAILED(rc))
            break;
        reachedCore = core;
        for (u32 i = 0; i < count; i++) {
            rc = svcSetHardwareBreakPoint(registers[i], controls[i],
                values[i]);
            if (R_FAILED(rc))
                break;
        }
        if (R_FAILED(rc))
            break;
    }

    if (R_FAILED(rc) && reachedCore >= 0) {
        for (s32 rollback = 0; rollback <= reachedCore; rollback++) {
            if (R_FAILED(pinToCore(current, rollback)))
                continue;
            for (u32 i = 0; i < count; i++)
                svcSetHardwareBreakPoint(registers[i], 0, 0);
        }
    }

    if (originalCore >= 0 && originalMask != 0)
        svcSetThreadCoreMask(current, originalCore, (u32)originalMask);
    else
        svcSetThreadCoreMask(current, DEBUG_HWBP_DEFAULT_CORE,
            1u << DEBUG_HWBP_DEFAULT_CORE);
    return rc;
}

Result debugHwBpSetAllCores(u32 reg, u64 control, u64 value)
{
    return setRegistersOnAllCores(&reg, &control, &value, 1);
}

Result debugHwBpClearAllCores(u32 reg)
{
    Result rc = debugHwBpSetAllCores(reg, 0, 0);
    for (u32 attempt = 1; R_FAILED(rc)
        && attempt < DEBUG_HWBP_CLEAR_RETRIES; attempt++) {
        svcSleepThread(1000000LL);
        rc = debugHwBpSetAllCores(reg, 0, 0);
    }
    return rc;
}

Result debugHwBpProbeContextSlots(u32* outContextSlot,
    u32* outDataContextSlot)
{
    if (outContextSlot == 0 && outDataContextSlot == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    s32 lastInstruction = -1;
    for (s32 i = DEBUG_HWBP_REGISTER_I0; i <= DEBUG_HWBP_REGISTER_I15; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        lastInstruction = i;
    }
    if (lastInstruction < 0)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    const u64 contextControl =
        (0x3ULL << 20) | (0x0ULL << 16) | (0xFULL << 5) | 1;
    s32 firstContext = -1;
    for (s32 i = lastInstruction; i >= DEBUG_HWBP_REGISTER_I0; i--) {
        Result rc = svcSetHardwareBreakPoint((u32)i, contextControl,
            (u64)CUR_PROCESS_HANDLE);
        svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc) && rc != KERNELRESULT(InvalidHandle))
            break;
        firstContext = i;
    }
    if (firstContext < 0)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    if (outContextSlot != 0)
        *outContextSlot = (u32)firstContext;
    if (outDataContextSlot != 0)
        *outDataContextSlot = (u32)(firstContext + 1);
    return 0;
}

Result debugHwBpProbeInstructionSlot(u32 contextSlot, u32* outInstructionSlot)
{
    if (outInstructionSlot == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (s32 i = DEBUG_HWBP_REGISTER_I0; i <= DEBUG_HWBP_REGISTER_I15; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        if ((u32)i != contextSlot) {
            *outInstructionSlot = (u32)i;
            return 0;
        }
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result debugHwBpProbeInstructionSlots(u32 contextSlot, u32* outSlots,
    u32 maxSlots, u32* outCount)
{
    if (outSlots == 0 || outCount == 0 || maxSlots == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    u32 count = 0;
    for (s32 i = DEBUG_HWBP_REGISTER_I0; i <= DEBUG_HWBP_REGISTER_I15
        && count < maxSlots; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        if ((u32)i != contextSlot)
            outSlots[count++] = (u32)i;
    }
    *outCount = count;
    return count > 0 ? 0
        : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result debugHwBpProbeDataRegister(u32* outDataRegister)
{
    if (outDataRegister == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (s32 i = DEBUG_HWBP_REGISTER_D0; i <= DEBUG_HWBP_REGISTER_D15; i++) {
        Result rc = svcSetHardwareBreakPoint((u32)i, 0, 0);
        if (R_FAILED(rc))
            break;
        *outDataRegister = (u32)i;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result debugHwBpSetContext(u32 contextSlot, Handle debugHandle)
{
    const u64 control =
        (0x3ULL << 20) | (0x0ULL << 16) | (0xFULL << 5) | 1;
    return debugHwBpSetAllCores(contextSlot, control, (u64)debugHandle);
}

Result debugHwBpSetInstruction(u32 instructionSlot, u32 contextSlot,
    u64 address)
{
    const u64 control = (0x1ULL << 20) | ((u64)contextSlot << 16)
        | (0xFULL << 5) | ((address != 0) ? 1 : 0);
    return debugHwBpSetAllCores(instructionSlot, control, address);
}

Result debugHwBpArmInstructionTrace(u32 contextSlot, Handle debugHandle,
    const u32* instructionSlots, const u64* addresses, u32 count)
{
    if (instructionSlots == NULL || addresses == NULL || count == 0
        || count > DEBUG_HWBP_BATCH_MAX)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    u32 registers[DEBUG_HWBP_BATCH_MAX + 1];
    u64 controls[DEBUG_HWBP_BATCH_MAX + 1];
    u64 values[DEBUG_HWBP_BATCH_MAX + 1];
    registers[0] = contextSlot;
    controls[0] = (0x3ULL << 20) | (0xFULL << 5) | 1;
    values[0] = (u64)debugHandle;
    for (u32 i = 0; i < count; i++) {
        if (instructionSlots[i] == contextSlot || addresses[i] == 0
            || (addresses[i] & 3) != 0)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        for (u32 prior = 0; prior < i; prior++) {
            if (instructionSlots[prior] == instructionSlots[i])
                return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        registers[i + 1] = instructionSlots[i];
        controls[i + 1] = (0x1ULL << 20)
            | ((u64)contextSlot << 16) | (0xFULL << 5) | 1;
        values[i + 1] = addresses[i];
    }
    return setRegistersOnAllCores(registers, controls, values, count + 1);
}

Result debugHwBpArmDualProcessInstructionTrace(
    const u32 contextSlots[2], const Handle debugHandles[2],
    const u32* instructionSlots, const u64* addresses,
    const u32* targetProcesses, u32 targetCount)
{
    if (contextSlots == NULL || debugHandles == NULL
        || instructionSlots == NULL || addresses == NULL
        || targetProcesses == NULL || targetCount == 0
        || targetCount > DEBUG_HWBP_BATCH_MAX
        || contextSlots[0] == contextSlots[1])
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    u32 registers[DEBUG_HWBP_DUAL_PROCESS_COUNT + DEBUG_HWBP_BATCH_MAX];
    u64 controls[DEBUG_HWBP_DUAL_PROCESS_COUNT + DEBUG_HWBP_BATCH_MAX];
    u64 values[DEBUG_HWBP_DUAL_PROCESS_COUNT + DEBUG_HWBP_BATCH_MAX];
    for (u32 process = 0; process < DEBUG_HWBP_DUAL_PROCESS_COUNT;
        process++) {
        registers[process] = contextSlots[process];
        controls[process] = (0x3ULL << 20) | (0xFULL << 5) | 1;
        values[process] = (u64)debugHandles[process];
    }
    for (u32 target = 0; target < targetCount; target++) {
        const u32 process = targetProcesses[target];
        if (process >= DEBUG_HWBP_DUAL_PROCESS_COUNT
            || addresses[target] == 0
            || (addresses[target] & 3) != 0
            || instructionSlots[target] == contextSlots[0]
            || instructionSlots[target] == contextSlots[1])
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        for (u32 prior = 0; prior < target; prior++) {
            if (instructionSlots[prior] == instructionSlots[target]
                || addresses[prior] == addresses[target])
                return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        const u32 arrayIndex = DEBUG_HWBP_DUAL_PROCESS_COUNT + target;
        registers[arrayIndex] = instructionSlots[target];
        controls[arrayIndex] = (0x1ULL << 20)
            | ((u64)contextSlots[process] << 16) | (0xFULL << 5) | 1;
        values[arrayIndex] = addresses[target];
    }
    return setRegistersOnAllCores(registers, controls, values,
        DEBUG_HWBP_DUAL_PROCESS_COUNT + targetCount);
}

Result debugHwBpClearRegistersAllCores(const u32* registers, u32 count)
{
    if (registers == NULL || count == 0 || count > 16)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    u64 controls[16] = { 0 };
    u64 values[16] = { 0 };
    Result rc = setRegistersOnAllCores(registers, controls, values, count);
    for (u32 attempt = 1; R_FAILED(rc)
        && attempt < DEBUG_HWBP_CLEAR_RETRIES; attempt++) {
        svcSleepThread(1000000LL);
        rc = setRegistersOnAllCores(registers, controls, values, count);
    }
    return rc;
}

bool debugHwBpIsValidWatchPoint(u64 address, u64 size)
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

Result debugHwBpSetData(u32 dataSlot, u32 contextSlot, u64 address, u64 size,
    bool read, bool write)
{
    const u8 lsc = (read ? 1 : 0) | (write ? 2 : 0);
    if (lsc != 0 && !debugHwBpIsValidWatchPoint(address, size))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    u8 bas = 0, mask = 0;
    if (size <= 8) {
        bas = (u8)(((1u << size) - 1) << (address & 7));
        address &= ~7ULL;
    } else {
        bas = 0xFF;
        mask = (u8)__builtin_popcountll(
            (unsigned long long)(size - 1));
    }

    const u64 control = ((u64)mask << 24) | ((u64)contextSlot << 16)
        | ((u64)bas << 5) | ((u64)lsc << 3) | ((lsc != 0) ? 1 : 0);
    return debugHwBpSetAllCores(dataSlot, control, address);
}

Result debugHwBpContinueAll(Handle debugHandle)
{
    u64 threadIds[] = { 0 };
    return svcContinueDebugEvent(debugHandle, DEBUG_CONTINUE_FLAGS,
        threadIds, 1);
}

Result debugHwBpStepOverInstruction(Handle debugHandle, u64 threadId,
    u32 instructionSlot, u32 contextSlot, u64 address)
{
    if (debugHandle == INVALID_HANDLE || threadId == 0 || address == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = debugHwBpClearAllCores(instructionSlot);
    if (R_FAILED(rc))
        return rc;

    ThreadContext context;
    memset(&context, 0, sizeof(context));
    rc = svcSetDebugThreadContext(debugHandle, threadId, &context,
        DEBUG_THREAD_CONTEXT_FLAG_SET_SINGLE_STEP);
    if (R_FAILED(rc))
        goto rearm;

    u64 threadIds[] = { threadId };
    rc = svcContinueDebugEvent(debugHandle,
        DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED
            | DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT,
        threadIds, 1);
    if (R_FAILED(rc))
        goto clearStep;

    const u64 deadline = armGetSystemTick()
        + armGetSystemTickFreq() * DEBUG_HWBP_STEP_TIMEOUT_SECONDS;
    bool stepEventSeen = false;
    while (!stepEventSeen && armGetSystemTick() < deadline) {
        s32 signaled = -1;
        rc = svcWaitSynchronization(&signaled, &debugHandle, 1,
            20000000LL);
        if (rc == KERNELRESULT(TimedOut)) {
            rc = 0;
            continue;
        }
        if (R_FAILED(rc))
            break;
        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
            if (event.type == DebugEventType_ExitProcess) {
                rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
                break;
            }
            if (event.type == DebugEventType_Exception
                && event.thread_id == threadId
                && event.info.exception.type == DebugException_BreakPoint
                && event.info.exception.specific.break_point.type
                    == BreakPointType_HardwareInstruction) {
                stepEventSeen = true;
                break;
            }
        }
        if (R_FAILED(rc))
            break;
    }
    if (R_SUCCEEDED(rc) && !stepEventSeen) {
        Result stopRc = debugHwBpQuiesce(debugHandle, false);
        rc = R_SUCCEEDED(stopRc) ? KERNELRESULT(TimedOut) : stopRc;
    }

clearStep:
    {
        ThreadContext emptyContext;
        memset(&emptyContext, 0, sizeof(emptyContext));
        Result clearRc = svcSetDebugThreadContext(debugHandle, threadId,
            &emptyContext, DEBUG_THREAD_CONTEXT_FLAG_CLEAR_SINGLE_STEP);
        if (R_FAILED(clearRc) && R_SUCCEEDED(rc))
            rc = clearRc;
    }

rearm:
    {
        Result armRc = debugHwBpSetInstruction(instructionSlot,
            contextSlot, address);
        if (R_FAILED(armRc) && R_SUCCEEDED(rc))
            rc = armRc;
    }
    return rc;
}

Result debugHwBpQuiesce(Handle debugHandle, bool exceptionPending)
{
    Result rc = 0;
    if (!exceptionPending) {
        rc = svcBreakDebugProcess(debugHandle);
        if (R_FAILED(rc))
            return rc;

        const u64 deadline = armGetSystemTick() + armGetSystemTickFreq();
        bool stopSeen = false;
        while (armGetSystemTick() < deadline) {
            s32 index = -1;
            rc = svcWaitSynchronization(&index, &debugHandle, 1,
                20000000LL);
            if (rc == KERNELRESULT(TimedOut))
                continue;
            if (R_FAILED(rc))
                return rc;

            while (true) {
                DebugEventInfo event;
                Result eventRc = svcGetDebugEvent(&event, debugHandle);
                if (R_FAILED(eventRc))
                    break;
                stopSeen = true;
            }
            if (stopSeen)
                break;
        }
        if (!stopSeen)
            return KERNELRESULT(TimedOut);
    } else {
        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debugHandle);
            if (R_FAILED(eventRc))
                break;
        }
    }
    return 0;
}
