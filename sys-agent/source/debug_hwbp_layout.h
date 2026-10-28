#pragma once

#include <switch.h>

#include "debug_hwbp.h"

#define DEBUG_HWBP_LAYOUT_SLOTS_MAX 16

typedef struct {
    u64 processId1;
    u64 processId2;
    u32 contextSlot1;
    u32 contextSlot2;
    Result contextSlot1Result;
    Result contextSlot2Result;
    u32 instructionSlotCount;
    u32 instructionSlots[DEBUG_HWBP_LAYOUT_SLOTS_MAX];
    Result drainResult;
    Result resumeResult;
    Result cleanupResult;
} DebugHwBpLayout;

Result debugHwBpProbeLayout(u64 pid1, u64 pid2, DebugHwBpLayout* out);
