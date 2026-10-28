#pragma once

#include <switch.h>

#include "debug_trace_model.h"

typedef struct {
    u64 processId;
    u64 addresses[DEBUG_TRACE_TARGET_MAX];
    u32 targetCount;
    u32 maxHits;
    u64 durationSeconds;
    bool durationFromFirstHit;
    u32 ringSize;
    u32 argsCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 snapshotCount;
    DebugTraceSnapshotSpec targetSnapshots[DEBUG_TRACE_TARGET_MAX]
        [DEBUG_TRACE_SNAPSHOT_MAX];
    u32 targetSnapshotCounts[DEBUG_TRACE_TARGET_MAX];
} DebugTraceConfig;

typedef struct {
    bool active;
    bool armed;
    bool everArmed;
    bool stale;
    bool memoryBlocked;
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
    u32 argsCount;
    u32 contextSlot;
    u32 instructionSlot;
    u32 ignoredHits;
    u32 snapshotCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    u32 targetSnapshotCounts[DEBUG_TRACE_TARGET_MAX];
    DebugTraceSnapshotSpec targetSnapshots[DEBUG_TRACE_TARGET_MAX]
        [DEBUG_TRACE_SNAPSHOT_MAX];
    u64 lastSequence;
    u64 lastPc;
    u64 lastLr;
    u64 lastSp;
    u64 lastThreadId;
    u32 lastCore;
    Result lastError;
    char stage[32];
    char hint[160];
} DebugTraceStatus;

void debugTraceInitialize(void);
bool debugTraceStart(const DebugTraceConfig* config);
void debugTraceStop(void);
bool debugTraceGetStatus(DebugTraceStatus* out);
bool debugTraceGetEvent(u32 index, DebugTraceEvent* out);
bool debugTraceIsActive(void);
