#include "debug_process.h"

#include <stdio.h>
#include <string.h>

#include "debug_trace.h"
#include "debug_multitrace.h"
#include "debug_watch.h"
#include "dmnt_client.h"

#define DEBUG_PROCESS_EVENT_TIMEOUT_NS 20000000LL
#define DEBUG_PROCESS_ERR_EXITED MAKERESULT(Module_Libnx, 0x2A1)

/* Same continue flags as debug_watch (dmnt.gen2 semantics). */
#define DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED (1u << 0)
#define DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT (1u << 1)
#define DEBUG_CONTINUE_FLAG_CONTINUE_ALL (1u << 2)
#define DEBUG_CONTINUE_FLAGS \
    (DEBUG_CONTINUE_FLAG_EXCEPTION_HANDLED \
     | DEBUG_CONTINUE_FLAG_ENABLE_EXCEPTION_EVENT \
     | DEBUG_CONTINUE_FLAG_CONTINUE_ALL)

Result debugProcessOpen(u64 pid, Handle* out, bool* dmntClosed)
{
    *out = INVALID_HANDLE;
    *dmntClosed = false;
    if (pid == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (debugWatchIsActive() || debugTraceIsActive()
        || debugMultiTraceIsActive())
        return KERNELRESULT(OwnedByAnotherProcess);

    /* dmnt:cht owns the single debug handle to the foreground application.
     * Only close it when the target actually is that application; system
     * processes are unattached and must not disturb the game's cheats. */
    u64 appPid = 0;
    if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&appPid)) && appPid == pid) {
        if (R_SUCCEEDED(dmntClientInitialize())) {
            bool attached = false;
            if (R_SUCCEEDED(dmntClientHasProcess(&attached)) && attached) {
                dmntClientForceClose();
                *dmntClosed = true;
            }
        }
    }
    return svcDebugActiveProcess(out, pid);
}

Result debugProcessDrain(Handle debug)
{
    const u64 deadline = armGetSystemTick() + armGetSystemTickFreq(); /* 1 s */
    bool attachedSeen = false;

    while (true) {
        s32 index = -1;
        Result rc = svcWaitSynchronization(&index, &debug, 1,
            DEBUG_PROCESS_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut)) {
            if (attachedSeen || armGetSystemTick() >= deadline)
                return 0;
            continue;
        }
        if (R_FAILED(rc))
            return rc;

        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debug);
            if (R_FAILED(eventRc))
                break;
            if (event.type == DebugEventType_ExitProcess)
                return DEBUG_PROCESS_ERR_EXITED;
            if (event.type == DebugEventType_CreateProcess)
                attachedSeen = true;
        }
    }
}

Result debugProcessPause(Handle debug)
{
    Result rc = svcBreakDebugProcess(debug);
    if (R_FAILED(rc))
        return rc;

    for (s32 attempt = 0; attempt < 50; attempt++) {
        s32 index = -1;
        rc = svcWaitSynchronization(&index, &debug, 1,
            DEBUG_PROCESS_EVENT_TIMEOUT_NS);
        if (rc == KERNELRESULT(TimedOut))
            continue;
        if (R_FAILED(rc))
            return rc;

        while (true) {
            DebugEventInfo event;
            Result eventRc = svcGetDebugEvent(&event, debug);
            if (R_FAILED(eventRc))
                break;
            if (event.type == DebugEventType_ExitProcess)
                return DEBUG_PROCESS_ERR_EXITED;
        }
        /* The break event has been consumed; the process is now stopped. */
        return 0;
    }
    return KERNELRESULT(TimedOut);
}

Result debugProcessResume(Handle debug)
{
    u64 threadIds[] = { 0 };
    return svcContinueDebugEvent(debug, DEBUG_CONTINUE_FLAGS,
        threadIds, 1);
}

void debugProcessClose(Handle debug)
{
    if (debug != INVALID_HANDLE)
        svcCloseHandle(debug);
}

void debugProcessPrintMaps(u64 pid, u32 limit)
{
    if (limit == 0)
        limit = 1;
    if (limit > DEBUG_PROCESS_MAX_MAPS)
        limit = DEBUG_PROCESS_MAX_MAPS;

    Handle debug = INVALID_HANDLE;
    bool dmntClosed = false;
    Result rc = debugProcessOpen(pid, &debug, &dmntClosed);
    if (R_FAILED(rc)) {
        printf("ERR code=DEBUG_ATTACH_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }

    typedef struct {
        u64 addr;
        u64 size;
        u32 type;
        u32 perm;
    } MapEntry;
    static MapEntry entries[DEBUG_PROCESS_MAX_MAPS];
    u32 count = 0;
    u64 addr = 0;
    Result scanRc = 0;
    bool truncated = false;

    while (count < limit) {
        MemoryInfo info;
        u32 pageInfo = 0;
        rc = svcQueryDebugProcessMemory(&info, &pageInfo, debug, addr);
        if (R_FAILED(rc)) {
            scanRc = rc;
            break;
        }
        if (info.size == 0)
            break;
        if ((info.type & 0xFF) != MemType_Unmapped) {
            entries[count].addr = info.addr;
            entries[count].size = info.size;
            entries[count].type = info.type & 0xFF;
            entries[count].perm = info.perm;
            count++;
        }
        const u64 next = info.addr + info.size;
        if (next <= addr)
            break;
        addr = next;
    }

    if (count == limit) {
        MemoryInfo info;
        u32 pageInfo = 0;
        if (R_SUCCEEDED(svcQueryDebugProcessMemory(&info, &pageInfo, debug,
                addr)) && info.size != 0)
            truncated = true;
    }

    if (count == 0 && R_FAILED(scanRc)) {
        debugProcessClose(debug);
        printf("ERR code=DEBUG_MAPS_FAILED pid=0x%lX result=0x%X\n",
            pid, scanRc);
        return;
    }

    debugProcessClose(debug);

    printf("OK pid=0x%lX count=%u limit=%u truncated=%d dmntClosed=%d maps=",
        pid, count, limit, truncated ? 1 : 0, dmntClosed ? 1 : 0);
    for (u32 i = 0; i < count; i++) {
        if (i != 0)
            printf(",");
        printf("%u=0x%lX+0x%lX:t%u/p%u", i, entries[i].addr,
            entries[i].size, entries[i].type, entries[i].perm);
    }
    if (R_FAILED(scanRc))
        printf(" scanErr=0x%X", scanRc);
    printf("\n");
}

void debugProcessPrintThreads(u64 pid)
{
    typedef struct {
        u64 threadId;
        u64 pc;
        u64 lr;
        u64 sp;
        u32 coreMask;
        u32 state;
        u32 currentCore;
        Result contextRc;
        Result stateRc;
        Result coreRc;
        Result maskRc;
    } ThreadRecord;

    ThreadRecord records[DEBUG_PROCESS_MAX_THREADS];

    Handle debug = INVALID_HANDLE;
    bool dmntClosed = false;
    Result rc = debugProcessOpen(pid, &debug, &dmntClosed);
    if (R_FAILED(rc)) {
        printf("ERR code=DEBUG_ATTACH_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }

    rc = debugProcessDrain(debug);
    if (R_FAILED(rc)) {
        debugProcessClose(debug);
        printf("ERR code=DEBUG_DRAIN_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }
    rc = debugProcessPause(debug);
    if (R_FAILED(rc)) {
        debugProcessClose(debug);
        printf("ERR code=DEBUG_PAUSE_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }

    u64 threadIds[DEBUG_PROCESS_MAX_THREADS];
    s32 threadCount = 0;
    rc = svcGetThreadList(&threadCount, threadIds,
        DEBUG_PROCESS_MAX_THREADS, debug);
    if (R_FAILED(rc)) {
        debugProcessResume(debug);
        debugProcessClose(debug);
        printf("ERR code=DEBUG_THREADS_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }

    /* GetThreadList returns the total count even when it is larger than the
     * supplied array.  Never index past the IDs the syscall could store. */
    const s32 reportedCount = threadCount;
    const s32 storedCount = threadCount < DEBUG_PROCESS_MAX_THREADS
        ? threadCount : DEBUG_PROCESS_MAX_THREADS;
    for (s32 i = 0; i < storedCount; i++) {
        ThreadRecord* record = &records[i];
        memset(record, 0, sizeof(*record));
        record->threadId = threadIds[i];

        ThreadContext ctx;
        record->contextRc = svcGetDebugThreadContext(&ctx, debug,
            record->threadId,
            RegisterGroup_CpuAll);
        if (R_SUCCEEDED(record->contextRc)) {
            record->pc = ctx.pc.x;
            record->lr = ctx.lr;
            record->sp = ctx.sp;
        }

        u64 value64 = 0;
        u32 value32 = 0;
        record->stateRc = svcGetDebugThreadParam(&value64, &value32, debug,
            record->threadId, DebugThreadParam_State);
        if (R_SUCCEEDED(record->stateRc))
            record->state = (u32)value64;

        value64 = 0;
        value32 = 0;
        record->coreRc = svcGetDebugThreadParam(&value64, &value32, debug,
            record->threadId, DebugThreadParam_CurrentCore);
        if (R_SUCCEEDED(record->coreRc))
            record->currentCore = value32;

        value64 = 0;
        value32 = 0;
        record->maskRc = svcGetDebugThreadParam(&value64, &value32, debug,
            record->threadId, DebugThreadParam_CoreMask);
        if (R_SUCCEEDED(record->maskRc))
            record->coreMask = value32;
    }

    rc = debugProcessResume(debug);
    if (R_FAILED(rc)) {
        debugProcessClose(debug);
        printf("ERR code=DEBUG_RESUME_FAILED pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }
    debugProcessClose(debug);

    printf("OK pid=0x%lX count=%d reported=%d truncated=%d dmntClosed=%d"
        " threads=", pid, storedCount, reportedCount,
        reportedCount > storedCount ? 1 : 0, dmntClosed ? 1 : 0);
    for (s32 i = 0; i < storedCount; i++) {
        const ThreadRecord* record = &records[i];
        if (i != 0)
            printf(",");
        printf("%d=0x%lX", i, record->threadId);
        if (R_SUCCEEDED(record->contextRc))
            printf(":pc=0x%lX,lr=0x%lX,sp=0x%lX", record->pc,
                record->lr, record->sp);
        else
            printf(":contextErr=0x%X", record->contextRc);
        if (R_SUCCEEDED(record->stateRc))
            printf(",state=%u", record->state);
        else
            printf(",stateErr=0x%X", record->stateRc);
        if (R_SUCCEEDED(record->coreRc))
            printf(",core=%u", record->currentCore);
        else
            printf(",coreErr=0x%X", record->coreRc);
        if (R_SUCCEEDED(record->maskRc))
            printf(",mask=0x%X", record->coreMask);
        else
            printf(",maskErr=0x%X", record->maskRc);
    }
    printf("\n");
}

void debugProcessPrintModules(u64 pid, u32 limit)
{
    if (limit == 0 || limit > DEBUG_PROCESS_MAX_MODULES)
        limit = DEBUG_PROCESS_MAX_MODULES;

    LoaderModuleInfo modules[DEBUG_PROCESS_MAX_MODULES];
    memset(modules, 0, sizeof(modules));
    s32 count = 0;
    Result rc = ldrDmntGetProcessModuleInfo(pid, modules, limit, &count);
    if (R_FAILED(rc) || count <= 0) {
        printf("ERR code=MODULES_UNAVAILABLE pid=0x%lX result=0x%X\n",
            pid, rc);
        return;
    }

    const u32 reportedCount = (u32)count;
    const u32 storedCount = reportedCount < limit ? reportedCount : limit;
    printf("OK pid=0x%lX count=%u reported=%u truncated=%d modules=",
        pid, storedCount, reportedCount, reportedCount > storedCount ? 1 : 0);
    for (u32 i = 0; i < storedCount; i++) {
        if (i != 0)
            printf(",");
        printf("%u=0x%lX+0x%lX:build=", i, modules[i].base_address,
            modules[i].size);
        for (u32 byte = 0; byte < sizeof(modules[i].build_id); byte++)
            printf("%02X", modules[i].build_id[byte]);
    }
    printf("\n");
}

Result debugProcessResolveMainBase(u64 pid, u64* outMainBase)
{
    if (pid == 0 || outMainBase == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    LoaderModuleInfo modules[2];
    s32 count = 0;
    Result rc = ldrDmntGetProcessModuleInfo(pid, modules, 2, &count);
    if (R_FAILED(rc))
        return rc;
    if (count <= 0)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    /* ldr:dmnt returns rtld first and the process's main module second. */
    *outMainBase = modules[count > 1 ? 1 : 0].base_address;
    return 0;
}

Result debugProcessReadMemory(u64 pid, u64 address, void* out, u32 size)
{
    if (pid == 0 || address == 0 || out == NULL || size == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Handle debug = INVALID_HANDLE;
    bool dmntClosed = false;
    Result rc = debugProcessOpen(pid, &debug, &dmntClosed);
    if (R_FAILED(rc))
        return rc;
    rc = debugProcessDrain(debug);
    if (R_SUCCEEDED(rc))
        rc = svcReadDebugProcessMemory(out, debug, address, size);
    debugProcessClose(debug);
    return rc;
}
