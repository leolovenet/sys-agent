#pragma once

#include <switch.h>

#define DEBUG_HWBP_CORE_COUNT 4
#define DEBUG_HWBP_REGISTER_I0 0
#define DEBUG_HWBP_REGISTER_I15 15
#define DEBUG_HWBP_REGISTER_D0 16
#define DEBUG_HWBP_REGISTER_D15 31

/*
 * Shared AArch64 hardware-breakpoint primitives.
 *
 * Hardware debug registers are per-core. Every operation here migrates the
 * calling thread through all four cores and verifies the actual core before
 * programming a register. The caller owns the debug process whose handle is
 * used as the context-IDR value.
 */
Result debugHwBpSetAllCores(u32 reg, u64 control, u64 value);
Result debugHwBpClearAllCores(u32 reg);

/* Discover the first usable context-IDR slot and the next slot used by data
 * watchpoints. Both values come from the current kernel, never a constant. */
Result debugHwBpProbeContextSlots(u32* outContextSlot, u32* outDataContextSlot);

/* Pick a valid instruction-breakpoint slot other than `contextSlot`. */
Result debugHwBpProbeInstructionSlot(u32 contextSlot, u32* outInstructionSlot);
Result debugHwBpProbeInstructionSlots(u32 contextSlot, u32* outSlots,
    u32 maxSlots, u32* outCount);
Result debugHwBpProbeDataRegister(u32* outDataRegister);

Result debugHwBpSetContext(u32 contextSlot, Handle debugHandle);
Result debugHwBpSetInstruction(u32 instructionSlot, u32 contextSlot,
    u64 address);
Result debugHwBpArmInstructionTrace(u32 contextSlot, Handle debugHandle,
    const u32* instructionSlots, const u64* addresses, u32 count);
Result debugHwBpArmDualProcessInstructionTrace(
    const u32 contextSlots[2], const Handle debugHandles[2],
    const u32* instructionSlots, const u64* addresses,
    const u32* targetProcesses, u32 targetCount);
Result debugHwBpClearRegistersAllCores(const u32* registers, u32 count);
bool debugHwBpIsValidWatchPoint(u64 address, u64 size);
Result debugHwBpSetData(u32 dataSlot, u32 contextSlot, u64 address, u64 size,
    bool read, bool write);

/* ExceptionHandled | EnableExceptionEvent | ContinueAll, with the valid dummy
 * thread-id list required by the kernel ABI. */
Result debugHwBpContinueAll(Handle debugHandle);
Result debugHwBpStepOverInstruction(Handle debugHandle, u64 threadId,
    u32 instructionSlot, u32 contextSlot, u64 address);

/*
 * Stop the target and drain its debug-event queue before clearing hardware
 * breakpoint registers. Set `exceptionPending` when the caller has already
 * consumed a stop exception and has not resumed. Closing the debug handle
 * resumes the target.
 */
Result debugHwBpQuiesce(Handle debugHandle, bool exceptionPending);
