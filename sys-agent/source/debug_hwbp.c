#include "debug_hwbp.h"

#define DEBUG_HWBP_PIN_RETRY_US 100LL
#define DEBUG_HWBP_PIN_ATTEMPTS 20000
#define DEBUG_HWBP_CLEAR_RETRIES 3
#define DEBUG_HWBP_DEFAULT_CORE 3
#define DEBUG_HWBP_BATCH_MAX 4

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

Result debugHwBpArmDualInstructionTrace(u32 contextSlot1, Handle debugHandle1,
    u32 instructionSlot1, u64 address1, u32 contextSlot2, Handle debugHandle2,
    u32 instructionSlot2, u64 address2)
{
    if (contextSlot1 == contextSlot2
        || instructionSlot1 == instructionSlot2
        || instructionSlot1 == contextSlot1
        || instructionSlot1 == contextSlot2
        || instructionSlot2 == contextSlot1
        || instructionSlot2 == contextSlot2
        || address1 == 0 || address2 == 0
        || (address1 & 3) != 0 || (address2 & 3) != 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const u32 registers[4] = {
        contextSlot1, contextSlot2, instructionSlot1, instructionSlot2,
    };
    const u64 controls[4] = {
        (0x3ULL << 20) | (0xFULL << 5) | 1,
        (0x3ULL << 20) | (0xFULL << 5) | 1,
        (0x1ULL << 20) | ((u64)contextSlot1 << 16) | (0xFULL << 5) | 1,
        (0x1ULL << 20) | ((u64)contextSlot2 << 16) | (0xFULL << 5) | 1,
    };
    const u64 values[4] = {
        (u64)debugHandle1, (u64)debugHandle2, address1, address2,
    };
    return setRegistersOnAllCores(registers, controls, values, 4);
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
