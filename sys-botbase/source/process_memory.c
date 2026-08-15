#include <stdio.h>
#include <string.h>
#include "dmnt_client.h"
#include "process_memory.h"
#include "process_memory_select.h"

static Mutex backendMutex;
static ProcessMemoryPolicy currentPolicy;
static ProcessMemoryStatus currentStatus;
static bool dmntOwnershipObserved;

static Result getApplicationProcessId(u64* processId)
{
    Result rc = pmdmntGetApplicationProcessId(processId);
    if (R_SUCCEEDED(rc) && *processId == 0)
        rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    return rc;
}

static Result fillDirectMetadata(ProcessMemorySession* session)
{
    ProcessMemoryMetadata* metadata = &session->metadata;
    memset(metadata, 0, sizeof(*metadata));
    metadata->processId = session->processId;

    Result rc = pminfoGetProgramId(&metadata->titleId, session->processId);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->heapBase, InfoType_HeapRegionAddress, session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->heapSize, InfoType_HeapRegionSize, session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->aliasBase, InfoType_AliasRegionAddress, session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->aliasSize, InfoType_AliasRegionSize, session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->addressSpaceBase, InfoType_AslrRegionAddress,
        session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;
    rc = svcGetInfo(&metadata->addressSpaceSize, InfoType_AslrRegionSize,
        session->directHandle, 0);
    if (R_FAILED(rc))
        return rc;

    LoaderModuleInfo modules[2];
    s32 count = 0;
    rc = ldrDmntGetProcessModuleInfo(session->processId, modules, 2, &count);
    if (R_FAILED(rc) || count <= 0)
        return R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    LoaderModuleInfo* mainModule = &modules[count == 2 ? 1 : 0];
    metadata->mainBase = mainModule->base_address;
    metadata->mainSize = mainModule->size;
    memcpy(metadata->buildId, mainModule->build_id, sizeof(metadata->buildId));
    return 0;
}

static Result openDirect(ProcessMemorySession* session, u64 processId)
{
    session->backend = ProcessMemoryBackendDirect;
    session->processId = processId;
    Result rc = svcDebugActiveProcess(&session->directHandle, processId);
    if (R_FAILED(rc))
        return rc;
    rc = fillDirectMetadata(session);
    if (R_FAILED(rc)) {
        svcCloseHandle(session->directHandle);
        session->directHandle = 0;
    }
    return rc;
}

static Result openDmnt(ProcessMemorySession* session, u64 processId, bool* acquired)
{
    *acquired = dmntOwnershipObserved;
    Result rc = dmntClientInitialize();
    currentStatus.dmntAvailable = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        return rc;

    bool attached = false;
    rc = dmntClientHasProcess(&attached);
    if (R_FAILED(rc))
        return rc;
    dmntOwnershipObserved = attached;
    *acquired = attached;
    currentStatus.dmntAttached = attached;
    if (!attached) {
        rc = dmntClientForceOpen();
        if (R_FAILED(rc))
            return rc;
        attached = true;
        dmntOwnershipObserved = true;
        currentStatus.dmntAttached = true;
    }
    *acquired = attached;

    DmntProcessMetadata dmntMetadata;
    rc = dmntClientGetMetadata(&dmntMetadata);
    if (R_FAILED(rc))
        return rc;
    if (dmntMetadata.processId != processId)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    session->backend = ProcessMemoryBackendDmnt;
    session->processId = processId;
    session->metadata.processId = dmntMetadata.processId;
    session->metadata.titleId = dmntMetadata.titleId;
    session->metadata.mainBase = dmntMetadata.mainNso.base;
    session->metadata.mainSize = dmntMetadata.mainNso.size;
    session->metadata.heapBase = dmntMetadata.heap.base;
    session->metadata.heapSize = dmntMetadata.heap.size;
    session->metadata.aliasBase = dmntMetadata.alias.base;
    session->metadata.aliasSize = dmntMetadata.alias.size;
    session->metadata.addressSpaceBase = dmntMetadata.addressSpace.base;
    session->metadata.addressSpaceSize = dmntMetadata.addressSpace.size;
    memcpy(session->metadata.buildId, dmntMetadata.buildId, sizeof(session->metadata.buildId));
    return 0;
}

static void recordOpenResult(const ProcessMemorySession* session, Result rc)
{
    currentStatus.lastError = rc;
    if (R_SUCCEEDED(rc)) {
        currentStatus.active = session->backend;
        currentStatus.processId = session->metadata.processId;
        currentStatus.titleId = session->metadata.titleId;
    }
    else {
        currentStatus.active = ProcessMemoryBackendNone;
        currentStatus.processId = 0;
        currentStatus.titleId = 0;
    }
}

typedef struct {
    ProcessMemorySession* session;
    u64 processId;
} OpenContext;

static int32_t selectOpenDmnt(void* context, bool* acquired)
{
    OpenContext* openContext = context;
    return (int32_t)openDmnt(openContext->session, openContext->processId, acquired);
}

static int32_t selectOpenDirect(void* context)
{
    OpenContext* openContext = context;
    return (int32_t)openDirect(openContext->session, openContext->processId);
}

static Result openLocked(ProcessMemorySession* session, ProcessMemoryPolicy policy,
    u64 expectedProcessId, bool reportErrors)
{
    memset(session, 0, sizeof(*session));
    u64 processId = 0;
    Result rc = getApplicationProcessId(&processId);
    if (R_FAILED(rc))
        goto done;
    if (expectedProcessId != 0 && expectedProcessId != processId) {
        rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        goto done;
    }

    OpenContext context = { .session = session, .processId = processId };
    PmSelectBackend selected;
    rc = (Result)processMemorySelectBackend((PmSelectPolicy)policy, selectOpenDmnt,
        selectOpenDirect, &context, &selected);

done:
    recordOpenResult(session, rc);
    if (R_FAILED(rc) && reportErrors)
        printf("processMemoryOpen: 0x%X\n", rc);
    if (R_SUCCEEDED(rc)) {
        session->open = true;
        return rc;
    }
    mutexUnlock(&backendMutex);
    return rc;
}

void processMemoryInitialize(void)
{
    mutexInit(&backendMutex);
    currentPolicy = ProcessMemoryPolicyAuto;
    memset(&currentStatus, 0, sizeof(currentStatus));
    dmntOwnershipObserved = false;
    currentStatus.policy = currentPolicy;
}

void processMemoryExit(void)
{
    mutexLock(&backendMutex);
    dmntClientExit();
    mutexUnlock(&backendMutex);
}

Result processMemoryOpen(ProcessMemorySession* session, bool reportErrors)
{
    mutexLock(&backendMutex);
    return openLocked(session, currentPolicy, 0, reportErrors);
}

Result processMemoryOpenBackend(ProcessMemorySession* session, ProcessMemoryBackendKind backend,
    u64 expectedProcessId, bool reportErrors)
{
    if (backend == ProcessMemoryBackendNone)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    mutexLock(&backendMutex);
    return openLocked(session, backend == ProcessMemoryBackendDmnt
        ? ProcessMemoryPolicyDmnt : ProcessMemoryPolicyDirect, expectedProcessId, reportErrors);
}

void processMemoryClose(ProcessMemorySession* session)
{
    if (session == NULL || !session->open)
        return;
    if (session->backend == ProcessMemoryBackendDirect && session->directHandle != 0)
        svcCloseHandle(session->directHandle);
    session->directHandle = 0;
    session->open = false;
    mutexUnlock(&backendMutex);
}

Result processMemoryRead(ProcessMemorySession* session, void* output, u64 address, size_t size)
{
    if (session->backend == ProcessMemoryBackendDmnt)
        return dmntClientRead(address, output, size);
    return svcReadDebugProcessMemory(output, session->directHandle, address, size);
}

Result processMemoryWrite(ProcessMemorySession* session, const void* input, u64 address, size_t size)
{
    if (session->backend == ProcessMemoryBackendDmnt)
        return dmntClientWrite(address, input, size);
    return svcWriteDebugProcessMemory(session->directHandle, input, address, size);
}

Result processMemoryQuery(ProcessMemorySession* session, MemoryInfo* info, u64 address)
{
    if (session->backend == ProcessMemoryBackendDmnt)
        return dmntClientQuery(info, address);
    u32 pageInfo = 0;
    return svcQueryDebugProcessMemory(info, &pageInfo, session->directHandle, address);
}

Result processMemoryPause(ProcessMemorySession* session)
{
    if (session->backend == ProcessMemoryBackendDmnt)
        return dmntClientPause();
    return svcBreakDebugProcess(session->directHandle);
}

Result processMemoryResume(ProcessMemorySession* session)
{
    if (session->backend == ProcessMemoryBackendDmnt)
        return dmntClientResume();
    return svcContinueDebugEvent(session->directHandle, 4 | 2 | 1, 0, 0);
}

const ProcessMemoryMetadata* processMemoryGetMetadata(const ProcessMemorySession* session)
{
    return &session->metadata;
}

ProcessMemoryPolicy processMemoryGetPolicy(void)
{
    mutexLock(&backendMutex);
    ProcessMemoryPolicy policy = currentPolicy;
    mutexUnlock(&backendMutex);
    return policy;
}

bool processMemorySetPolicy(ProcessMemoryPolicy policy)
{
    if (policy < ProcessMemoryPolicyAuto || policy > ProcessMemoryPolicyDirect)
        return false;
    mutexLock(&backendMutex);
    currentPolicy = policy;
    currentStatus.policy = policy;
    currentStatus.active = ProcessMemoryBackendNone;
    currentStatus.processId = 0;
    currentStatus.titleId = 0;
    currentStatus.lastError = 0;
    mutexUnlock(&backendMutex);
    return true;
}

void processMemoryGetStatus(ProcessMemoryStatus* status)
{
    mutexLock(&backendMutex);
    *status = currentStatus;
    status->policy = currentPolicy;
    status->dmntAvailable = dmntClientIsInitialized();
    mutexUnlock(&backendMutex);
}

bool processMemoryParsePolicy(const char* text, ProcessMemoryPolicy* policy)
{
    if (!strcmp(text, "auto")) *policy = ProcessMemoryPolicyAuto;
    else if (!strcmp(text, "dmnt")) *policy = ProcessMemoryPolicyDmnt;
    else if (!strcmp(text, "direct")) *policy = ProcessMemoryPolicyDirect;
    else return false;
    return true;
}

const char* processMemoryPolicyName(ProcessMemoryPolicy policy)
{
    switch (policy) {
    case ProcessMemoryPolicyAuto: return "auto";
    case ProcessMemoryPolicyDmnt: return "dmnt";
    case ProcessMemoryPolicyDirect: return "direct";
    default: return "unknown";
    }
}

const char* processMemoryBackendName(ProcessMemoryBackendKind backend)
{
    switch (backend) {
    case ProcessMemoryBackendNone: return "none";
    case ProcessMemoryBackendDmnt: return "dmnt";
    case ProcessMemoryBackendDirect: return "direct";
    default: return "unknown";
    }
}
