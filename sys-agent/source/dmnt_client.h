#pragma once

#include <switch.h>

typedef struct {
    u64 base;
    u64 size;
} DmntMemoryRegionExtents;

typedef struct {
    u64 processId;
    u64 titleId;
    DmntMemoryRegionExtents mainNso;
    DmntMemoryRegionExtents heap;
    DmntMemoryRegionExtents alias;
    DmntMemoryRegionExtents addressSpace;
    u8 buildId[0x20];
} DmntProcessMetadata;

_Static_assert(sizeof(DmntProcessMetadata) == 0x70, "dmnt metadata ABI size changed");

Result dmntClientInitialize(void);
void dmntClientExit(void);
bool dmntClientIsInitialized(void);
Result dmntClientHasProcess(bool* out);
Result dmntClientGetMetadata(DmntProcessMetadata* out);
Result dmntClientForceOpen(void);
Result dmntClientRead(u64 address, void* buffer, size_t size);
Result dmntClientWrite(u64 address, const void* buffer, size_t size);
Result dmntClientQuery(MemoryInfo* info, u64 address);
Result dmntClientPause(void);
Result dmntClientResume(void);
