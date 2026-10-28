#pragma once

#include <switch.h>

#include "debug_trace_model.h"

#define DEBUG_MULTITRACE_PROCESS_COUNT 2
#define DEBUG_MULTITRACE_TARGET_MAX 4

typedef struct {
    u64 processIds[DEBUG_MULTITRACE_PROCESS_COUNT];
    u64 addresses[DEBUG_MULTITRACE_TARGET_MAX];
    u32 targetProcesses[DEBUG_MULTITRACE_TARGET_MAX];
    u32 targetCount;
    u32 maxHits;
    u64 durationSeconds;
    u32 ringSize;
    u32 argsCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 snapshotCount;
} DebugMultiTraceConfig;

typedef struct {
    bool active;
    bool armed;
    bool everArmed;
    bool stale;
    bool memoryBlocked;
    u64 processIds[DEBUG_MULTITRACE_PROCESS_COUNT];
    u64 addresses[DEBUG_MULTITRACE_TARGET_MAX];
    u32 targetProcesses[DEBUG_MULTITRACE_TARGET_MAX];
    u32 targetHits[DEBUG_MULTITRACE_TARGET_MAX];
    u32 contextSlots[DEBUG_MULTITRACE_PROCESS_COUNT];
    u32 instructionSlots[DEBUG_MULTITRACE_TARGET_MAX];
    u32 targetCount;
    u64 hitCount;
    u64 durationSeconds;
    u64 startedTick;
    u32 maxHits;
    u32 ringSize;
    u32 ringCount;
    u32 argsCount;
    u32 snapshotCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 ignoredHits;
    u64 lastSequence;
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastThreadId;
    u32 lastCore;
    Result lastError;
    char stage[32];
} DebugMultiTraceStatus;

void debugMultiTraceInitialize(void);
bool debugMultiTraceStart(const DebugMultiTraceConfig* config);
void debugMultiTraceStop(void);
bool debugMultiTraceGetStatus(DebugMultiTraceStatus* out);
bool debugMultiTraceGetEvent(u32 index, DebugTraceEvent* out);
bool debugMultiTraceIsActive(void);
