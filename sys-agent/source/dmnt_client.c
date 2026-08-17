#define NX_SERVICE_ASSUME_NON_DOMAIN

#include "dmnt_client.h"

static Service dmntService;
static bool initialized;

Result dmntClientInitialize(void)
{
    if (initialized)
        return 0;
    Result rc = smGetService(&dmntService, "dmnt:cht");
    if (R_SUCCEEDED(rc))
        initialized = true;
    return rc;
}

void dmntClientExit(void)
{
    if (initialized)
        serviceClose(&dmntService);
    initialized = false;
}

bool dmntClientIsInitialized(void)
{
    return initialized;
}

Result dmntClientHasProcess(bool* out)
{
    u8 value = 0;
    Result rc = serviceDispatchOut(&dmntService, 65000, value);
    if (R_SUCCEEDED(rc) && out != NULL)
        *out = (value & 1) != 0;
    return rc;
}

Result dmntClientGetMetadata(DmntProcessMetadata* out)
{
    return serviceDispatchOut(&dmntService, 65002, *out);
}

Result dmntClientForceOpen(void)
{
    return serviceDispatch(&dmntService, 65003);
}

Result dmntClientRead(u64 address, void* buffer, size_t size)
{
    const struct { u64 address; u64 size; } in = { address, size };
    return serviceDispatchIn(&dmntService, 65102, in,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
        .buffers = { { buffer, size } });
}

Result dmntClientWrite(u64 address, const void* buffer, size_t size)
{
    const struct { u64 address; u64 size; } in = { address, size };
    return serviceDispatchIn(&dmntService, 65103, in,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { buffer, size } });
}

Result dmntClientQuery(MemoryInfo* info, u64 address)
{
    return serviceDispatchInOut(&dmntService, 65104, address, *info);
}

Result dmntClientPause(void)
{
    return serviceDispatch(&dmntService, 65004);
}

Result dmntClientResume(void)
{
    return serviceDispatch(&dmntService, 65005);
}
