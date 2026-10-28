#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "debug_trace_model.h"

static void testAddressValidation(void)
{
    assert(!debugTraceAddressIsValid(0));
    assert(!debugTraceAddressIsValid(0x1001));
    assert(!debugTraceAddressIsValid(0x1002));
    assert(debugTraceAddressIsValid(0x1000));
    assert(debugTraceAddressIsValid(0x1004));
}

static void testConfigValidation(void)
{
    DebugTraceConfigModel config = {
        .addresses = { 0x1000 },
        .targetCount = 1,
        .maxHits = 1,
        .durationSeconds = 60,
        .ringSize = 0,
        .argsCount = DEBUG_TRACE_ARGS_MAX,
    };
    assert(debugTraceConfigIsValid(&config));
    assert(!debugTraceConfigIsValid(0));

    config.addresses[0] = 0;
    assert(!debugTraceConfigIsValid(&config));
    config.addresses[0] = 0x1002;
    assert(!debugTraceConfigIsValid(&config));
    config.addresses[0] = 0x1000;

    config.maxHits = 0;
    config.ringSize = 0;
    assert(!debugTraceConfigIsValid(&config));
    config.ringSize = 8;
    assert(debugTraceConfigIsValid(&config));
    config.ringSize = 0;
    config.maxHits = 1;

    config.ringSize = DEBUG_TRACE_RING_MAX + 1;
    assert(!debugTraceConfigIsValid(&config));
    config.ringSize = DEBUG_TRACE_RING_MAX;
    assert(debugTraceConfigIsValid(&config));

    config.argsCount = DEBUG_TRACE_ARGS_MAX + 1;
    assert(!debugTraceConfigIsValid(&config));

    config.argsCount = DEBUG_TRACE_ARGS_MAX;
    config.targetCount = 0;
    assert(!debugTraceConfigIsValid(&config));
    config.targetCount = DEBUG_TRACE_TARGET_MAX + 1;
    assert(!debugTraceConfigIsValid(&config));
    config.targetCount = 2;
    config.addresses[0] = 0x1000;
    config.addresses[1] = 0x1000;
    assert(!debugTraceConfigIsValid(&config));
    config.addresses[1] = 0x1004;
    assert(debugTraceConfigIsValid(&config));

    config.snapshotCount = DEBUG_TRACE_SNAPSHOT_MAX + 1;
    assert(!debugTraceConfigIsValid(&config));
    config.snapshotCount = 1;
    config.snapshots[0].base = DebugTraceSnapshotBase_Gpr;
    config.snapshots[0].registerIndex = 0;
    config.snapshots[0].size = 16;
    assert(debugTraceConfigIsValid(&config));
}

static void testSnapshotSpecParsing(void)
{
    DebugTraceSnapshotSpec spec;
    assert(debugTraceParseSnapshotSpec("x0:0x40", &spec));
    assert(spec.base == DebugTraceSnapshotBase_Gpr);
    assert(spec.registerIndex == 0);
    assert(spec.offset == 0);
    assert(spec.size == 0x40);

    assert(debugTraceParseSnapshotSpec("x30+0x18:16", &spec));
    assert(spec.registerIndex == 30);
    assert(spec.offset == 0x18);
    assert(spec.size == 16);

    assert(debugTraceParseSnapshotSpec("sp-0x20:32", &spec));
    assert(spec.base == DebugTraceSnapshotBase_Sp);
    assert(spec.offset == -0x20);
    assert(debugTraceParseSnapshotSpec("pc:4", &spec));
    assert(spec.base == DebugTraceSnapshotBase_Pc);
    assert(debugTraceParseSnapshotSpec("lr:0x100", &spec));
    assert(spec.base == DebugTraceSnapshotBase_Lr);

    assert(!debugTraceParseSnapshotSpec("", &spec));
    assert(!debugTraceParseSnapshotSpec("x0", &spec));
    assert(!debugTraceParseSnapshotSpec("x31:4", &spec));
    assert(!debugTraceParseSnapshotSpec("w0:4", &spec));
    assert(!debugTraceParseSnapshotSpec("x0:0", &spec));
    assert(!debugTraceParseSnapshotSpec("x0:0x101", &spec));
    assert(!debugTraceParseSnapshotSpec("x0+", &spec));
    assert(!debugTraceParseSnapshotSpec("x0+0x10:", &spec));
    assert(!debugTraceParseSnapshotSpec("x0++4:4", &spec));
}

static void testSnapshotAddress(void)
{
    uint64_t gprs[31] = { 0 };
    gprs[3] = 0x1000;
    DebugTraceSnapshotSpec spec = {
        .base = DebugTraceSnapshotBase_Gpr,
        .registerIndex = 3,
        .offset = 0x20,
        .size = 8,
    };
    uint64_t address = 0;
    assert(debugTraceComputeSnapshotAddress(&spec, gprs, 0x2000, 0x3000,
        0x4000, &address));
    assert(address == 0x1020);

    spec.offset = -0x10;
    assert(debugTraceComputeSnapshotAddress(&spec, gprs, 0x2000, 0x3000,
        0x4000, &address));
    assert(address == 0xff0);

    spec.offset = -0x1001;
    assert(!debugTraceComputeSnapshotAddress(&spec, gprs, 0x2000, 0x3000,
        0x4000, &address));

    spec.base = DebugTraceSnapshotBase_Sp;
    spec.offset = 8;
    assert(debugTraceComputeSnapshotAddress(&spec, gprs, 0x2000, 0x3000,
        0x4000, &address));
    assert(address == 0x2008);
}

static void testTargetLookup(void)
{
    const uint64_t targets[] = { 0x1000, 0x2000, 0x3000 };
    assert(debugTraceFindTarget(targets, 3, 0x1000) == 0);
    assert(debugTraceFindTarget(targets, 3, 0x3000) == 2);
    assert(debugTraceFindTarget(targets, 3, 0x4000) == UINT32_MAX);
}

static void testRingOrder(void)
{
    assert(debugTraceRingOldestIndex(0, 8, 0, 0) == UINT32_MAX);
    assert(debugTraceRingOldestIndex(0, 0, 0, 0) == UINT32_MAX);
    assert(debugTraceRingOldestIndex(0, 8, 9, 0) == UINT32_MAX);
    assert(debugTraceRingOldestIndex(2, 8, 2, 2) == UINT32_MAX);

    /* Writes 0,1,2,3,4 into an 8-entry ring: oldest is 0. */
    assert(debugTraceRingOldestIndex(5, 8, 5, 0) == 0);
    assert(debugTraceRingOldestIndex(5, 8, 5, 4) == 4);

    /* Writes 0..10: the retained values are 3..10, oldest first. */
    assert(debugTraceRingOldestIndex(11 % 8, 8, 8, 0) == 3);
    assert(debugTraceRingOldestIndex(11 % 8, 8, 8, 7) == 2);
}

static void testOverwriteAndLimit(void)
{
    assert(debugTraceOverwrittenCount(0, 0) == 0);
    assert(debugTraceOverwrittenCount(3, 3) == 0);
    assert(debugTraceOverwrittenCount(10, 8) == 2);

    assert(debugTraceHitLimitReached(1, 1));
    assert(debugTraceHitLimitReached(2, 1));
    assert(!debugTraceHitLimitReached(0, 1));
    assert(!debugTraceHitLimitReached(63,
        DEBUG_TRACE_DEFAULT_RING_HITS));
    assert(debugTraceHitLimitReached(64,
        DEBUG_TRACE_DEFAULT_RING_HITS));
}

int main(void)
{
    testAddressValidation();
    testConfigValidation();
    testSnapshotSpecParsing();
    testSnapshotAddress();
    testTargetLookup();
    testRingOrder();
    testOverwriteAndLimit();
    printf("debug_trace_model tests passed\n");
    return 0;
}
