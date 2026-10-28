#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DEBUG_TRACE_RING_MAX 64
#define DEBUG_TRACE_ARGS_MAX 8
#define DEBUG_TRACE_INSN_BYTES 12
#define DEBUG_TRACE_DEFAULT_RING_HITS 64
#define DEBUG_TRACE_TARGET_MAX 4
#define DEBUG_TRACE_SNAPSHOT_MAX 2
#define DEBUG_TRACE_SNAPSHOT_BYTES_MAX 0x100

typedef enum {
    DebugTraceSnapshotBase_Gpr = 0,
    DebugTraceSnapshotBase_Sp = 1,
    DebugTraceSnapshotBase_Pc = 2,
    DebugTraceSnapshotBase_Lr = 3,
} DebugTraceSnapshotBase;

typedef struct {
    uint8_t base;
    uint8_t registerIndex;
    int64_t offset;
    uint32_t size;
} DebugTraceSnapshotSpec;

typedef struct {
    uint64_t addresses[DEBUG_TRACE_TARGET_MAX];
    uint32_t targetCount;
    uint32_t maxHits;
    uint64_t durationSeconds;
    uint32_t ringSize;
    uint32_t argsCount;
    DebugTraceSnapshotSpec snapshots[DEBUG_TRACE_SNAPSHOT_MAX];
    uint32_t snapshotCount;
} DebugTraceConfigModel;

typedef struct {
    uint64_t sequence;
    uint64_t targetAddress;
    uint64_t threadId;
    uint64_t pc;
    uint64_t lr;
    uint64_t sp;
    uint64_t x[DEBUG_TRACE_ARGS_MAX];
    uint64_t tick;
    uint32_t core;
    uint32_t argsCount;
    uint32_t insnBytes;
    uint32_t contextResult;
    uint32_t coreResult;
    uint32_t targetIndex;
    uint32_t snapshotCount;
    uint64_t snapshotAddresses[DEBUG_TRACE_SNAPSHOT_MAX];
    uint32_t snapshotBytes[DEBUG_TRACE_SNAPSHOT_MAX];
    uint32_t snapshotResults[DEBUG_TRACE_SNAPSHOT_MAX];
    uint8_t snapshots[DEBUG_TRACE_SNAPSHOT_MAX][DEBUG_TRACE_SNAPSHOT_BYTES_MAX];
    uint8_t insn[DEBUG_TRACE_INSN_BYTES];
} DebugTraceEvent;

bool debugTraceAddressIsValid(uint64_t address);
bool debugTraceParseSnapshotSpec(const char* text,
    DebugTraceSnapshotSpec* out);
bool debugTraceSnapshotSpecIsValid(const DebugTraceSnapshotSpec* spec);
bool debugTraceComputeSnapshotAddress(const DebugTraceSnapshotSpec* spec,
    const uint64_t gprs[31], uint64_t sp, uint64_t pc, uint64_t lr,
    uint64_t* outAddress);
bool debugTraceConfigIsValid(const DebugTraceConfigModel* config);
uint32_t debugTraceFindTarget(const uint64_t* addresses, uint32_t count,
    uint64_t address);
uint32_t debugTraceRingOldestIndex(uint32_t next, uint32_t capacity,
    uint32_t count, uint32_t index);
uint64_t debugTraceOverwrittenCount(uint64_t hitCount, uint32_t storedCount);
bool debugTraceHitLimitReached(uint64_t hitCount, uint32_t maxHits);
