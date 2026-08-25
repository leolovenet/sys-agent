#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <math.h>
#include <switch.h>
#include "commands.h"
#include "args.h"
#include "util.h"
#include "freeze.h"
#include "search.h"
#include "process_memory.h"
#include "debug_watch.h"
#include "patch_code.h"
#include "dmnt_client.h"
#include "ftp_server.h"
#include "system_commands.h"
#include <poll.h>
#include <switch/runtime/devices/fs_dev.h>

#define TITLE_ID 0x43000000000000A6
#define HEAP_SIZE 0x00480000
#define THREAD_SIZE 0x1A000
#define VERSION_S "2.7.1"

typedef enum {
    Active = 0,
    Exit = 1,
    Idle = 2,
    Pause = 3
} FreezeThreadState;

Thread freezeThread, touchThread, keyboardThread, clickThread;

// prototype thread functions to give the illusion of cleanliness
void sub_freeze(void* arg);
void sub_touch(void* arg);
void sub_key(void* arg);
void sub_click(void* arg);

// locks for thread
Mutex freezeMutex, touchMutex, keyMutex, clickMutex;

// events for releasing or idling threads
FreezeThreadState freeze_thr_state = Active;
u8 clickThreadState = 0; // 1 = break thread
// key and touch events currently being processed
KeyData currentKeyEvent = { 0 };
TouchData currentTouchEvent = { 0 };
char* currentClick = NULL;

// for cancelling the touch/click thread
u8 touchToken = 0;
u8 clickToken = 0;

// fd counters and max size
int fd_count = 0;
int fd_size = 5;

// we aren't an applet
u32 __nx_applet_type = AppletType_None;
static bool searchSdMounted = false;

// we override libnx internals to do a minimal init
void __libnx_initheap(void)
{
    static u8 inner_heap[HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    // Configure the newlib heap.
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc;
    svcSleepThread(20000000000L);
    rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = fsdevMountSdmc();
        searchSdMounted = R_SUCCEEDED(rc);
        if (!searchSdMounted)
            fsExit();
    }
    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }
    rc = pmdmntInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = ldrDmntInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = pminfoInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = socketInitializeDefault();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = capsscInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);
    rc = viInitialize(ViServiceType_Default);
    if (R_FAILED(rc))
        fatalThrow(rc);
}

void __appExit(void)
{
    if (searchSdMounted) {
        fsdevUnmountDevice("sdmc");
        fsExit();
        searchSdMounted = false;
    }
    smExit();
    nsExit();
    audoutExit();
    socketExit();
    viExit();
}

u64 mainLoopSleepTime = 50;
u64 freezeRate = 3;
bool debugResultCodes = false;

bool echoCommands = false;

void makeTouch(HidTouchState* state, u64 sequentialCount, u64 holdTime, bool hold)
{
    mutexLock(&touchMutex);
    memset(&currentTouchEvent, 0, sizeof currentTouchEvent);
    currentTouchEvent.states = state;
    currentTouchEvent.sequentialCount = sequentialCount;
    currentTouchEvent.holdTime = holdTime;
    currentTouchEvent.hold = hold;
    currentTouchEvent.state = 1;
    mutexUnlock(&touchMutex);
}

void makeKeys(HiddbgKeyboardAutoPilotState* states, u64 sequentialCount)
{
    mutexLock(&keyMutex);
    memset(&currentKeyEvent, 0, sizeof currentKeyEvent);
    currentKeyEvent.states = states;
    currentKeyEvent.sequentialCount = sequentialCount;
    currentKeyEvent.state = 1;
    mutexUnlock(&keyMutex);
}

void makeClickSeq(char* seq)
{
    mutexLock(&clickMutex);
    currentClick = seq;
    mutexUnlock(&clickMutex);
}

static void printSearchStartResponse(SearchStartResult result, u64 sessionId)
{
    switch (result) {
    case SearchStartOk: printf("OK session=%lu state=queued\n", sessionId); break;
    case SearchStartBusy: printf("ERR code=BUSY\n"); break;
    case SearchStartInvalidRange: printf("ERR code=INVALID_RANGE\n"); break;
    case SearchStartInvalidPattern: printf("ERR code=INVALID_VALUE\n"); break;
    case SearchStartInvalidType: printf("ERR code=INVALID_TYPE\n"); break;
    case SearchStartInvalidRegion: printf("ERR code=INVALID_REGION\n"); break;
    case SearchStartInvalidAlignment: printf("ERR code=INVALID_ALIGNMENT\n"); break;
    case SearchStartBaseUnavailable: printf("ERR code=BASE_UNAVAILABLE\n"); break;
    case SearchStartNoMemory: printf("ERR code=NO_MEMORY\n"); break;
    case SearchStartNoProcess: printf("ERR code=NO_PROCESS\n"); break;
    case SearchStartUnavailable: printf("ERR code=UNAVAILABLE\n"); break;
    case SearchStartSdUnavailable: printf("ERR code=SD_UNAVAILABLE\n"); break;
    case SearchStartInsufficientStorage: printf("ERR code=INSUFFICIENT_STORAGE\n"); break;
    case SearchStartCorruptSession: printf("ERR code=CORRUPT_SESSION\n"); break;
    case SearchStartProcessChanged: printf("ERR code=PROCESS_CHANGED\n"); break;
    case SearchStartPauseFailed: printf("ERR code=PAUSE_FAILED\n"); break;
    case SearchStartIoError: printf("ERR code=IO_ERROR\n"); break;
    case SearchStartSessionNotReady: printf("ERR code=SESSION_NOT_READY\n"); break;
    }
}

int argmain(int argc, char** argv)
{
    if (argc == 0)
        return 0;

    if (systemCommandsDispatch(argc, argv))
        return 0;

    if (!strcmp(argv[0], "ftpStatus"))
    {
        if (argc != 1) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        FtpServerStatus status;
        ftpServerGetStatus(&status);
        printf("OK state=%s enabled=%d port=%u anonymous=%d transfers=%u bytesSent=%lu bytesReceived=%lu config=%s lastError=%d lastFsResult=0x%X\n",
            ftpServerStateName(status.state), status.config.enabled, status.config.port,
            status.config.anonymous, status.activeTransfers, status.bytesSent,
            status.bytesReceived, ftpConfigResultName(status.configResult), status.lastError,
            status.lastFsResult);
        return 0;
    }

    if (!strcmp(argv[0], "ftpStart") || !strcmp(argv[0], "ftpStop")
        || !strcmp(argv[0], "ftpRestart") || !strcmp(argv[0], "ftpReload"))
    {
        if (argc != 1) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        bool accepted;
        if (!strcmp(argv[0], "ftpStart"))
            accepted = ftpServerStart();
        else if (!strcmp(argv[0], "ftpStop"))
            accepted = ftpServerStop();
        else
            accepted = ftpServerRestart(!strcmp(argv[0], "ftpReload"));
        if (!accepted) {
            FtpServerStatus status;
            ftpServerGetStatus(&status);
            if (status.configResult != FtpConfigOk)
                printf("ERR code=FTP_CONFIG_ERROR detail=%s\n",
                    ftpConfigResultName(status.configResult));
            else
                printf("ERR code=FTP_UNAVAILABLE\n");
            return 0;
        }
        FtpServerStatus status;
        ftpServerGetStatus(&status);
        printf("OK state=%s\n", ftpServerStateName(status.state));
        return 0;
    }

    if (!strcmp(argv[0], "memoryBackend"))
    {
        if (argc == 2) {
            ProcessMemoryPolicy policy;
            if (!processMemoryParsePolicy(argv[1], &policy)) {
                printf("ERR code=INVALID_POLICY\n");
                return 0;
            }
            if (searchLocksBackend()) {
                printf("ERR code=BUSY\n");
                return 0;
            }
            processMemorySetPolicy(policy);
        }
        else if (argc != 1) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        ProcessMemoryStatus status;
        processMemoryGetStatus(&status);
        printf("OK policy=%s active=%s dmntAvailable=%d dmntAttached=%d pid=%016lX titleId=%016lX lastError=0x%X\n",
            processMemoryPolicyName(status.policy), processMemoryBackendName(status.active),
            status.dmntAvailable, status.dmntAttached, status.processId, status.titleId,
            status.lastError);
        return 0;
    }

    if (!strcmp(argv[0], "memoryBackendProbe"))
    {
        if (argc != 1) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        ProcessMemorySession session;
        Result rc = processMemoryOpen(&session, false);
        if (R_FAILED(rc)) {
            printf("ERR code=BACKEND_UNAVAILABLE result=0x%X\n", rc);
            return 0;
        }
        processMemoryClose(&session);
        ProcessMemoryStatus status;
        processMemoryGetStatus(&status);
        printf("OK policy=%s active=%s dmntAvailable=%d dmntAttached=%d pid=%016lX titleId=%016lX lastError=0x%X\n",
            processMemoryPolicyName(status.policy), processMemoryBackendName(status.active),
            status.dmntAvailable, status.dmntAttached, status.processId, status.titleId,
            status.lastError);
        return 0;
    }

    if (!strcmp(argv[0], "debug"))
    {
        if (argc < 2) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        if (!strcmp(argv[1], "watch"))
        {
            /* debug watch <address|main+offset> [size] [hits N] [duration N] */
            u64 address = 0;
            u64 size = 4;
            u32 maxHits = 1;
            u64 duration = 60;
            int index = 2;
            if (index >= argc) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            if (strncmp(argv[index], "main+", 5) == 0) {
                u64 mainBase = 0;
                Result rc = debugWatchResolveMainBase(&mainBase);
                if (R_FAILED(rc)) {
                    printf("ERR code=MAIN_BASE_UNAVAILABLE result=0x%X\n", rc);
                    return 0;
                }
                u64 offset = 0;
                if (!tryParseStringToInt(argv[index] + 5, &offset)) {
                    printf("ERR code=INVALID_ADDRESS\n");
                    return 0;
                }
                address = mainBase + offset;
            } else if (!tryParseStringToInt(argv[index], &address)) {
                printf("ERR code=INVALID_ADDRESS\n");
                return 0;
            }
            index++;
            if (index < argc) {
                u64 parsed = 0;
                if (!tryParseStringToInt(argv[index], &parsed)
                    || parsed == 0 || parsed > 8) {
                    printf("ERR code=INVALID_SIZE\n");
                    return 0;
                }
                size = parsed;
                index++;
            }
            while (index < argc) {
                if (!strcmp(argv[index], "hits") && index + 1 < argc) {
                    u64 parsed = 0;
                    if (!tryParseStringToInt(argv[index + 1], &parsed)
                        || parsed == 0 || parsed > 0xFFFF) {
                        printf("ERR code=INVALID_HITS\n");
                        return 0;
                    }
                    maxHits = (u32)parsed;
                    index += 2;
                } else if (!strcmp(argv[index], "duration")
                    && index + 1 < argc) {
                    u64 parsed = 0;
                    if (!tryParseStringToInt(argv[index + 1], &parsed)
                        || parsed > 0x7FFFFFFF) {
                        printf("ERR code=INVALID_DURATION\n");
                        return 0;
                    }
                    duration = parsed;
                    index += 2;
                } else {
                    break;
                }
            }
            if (index != argc) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            /* If gen1 (dmnt:cht) already owns the debug handle, close it so
             * the watch can attach directly without a game restart.  This is
             * a live query (no cached state) and harmless when not attached. */
            bool dmntClosed = false;
            if (R_SUCCEEDED(dmntClientInitialize())) {
                bool attached = false;
                if (R_SUCCEEDED(dmntClientHasProcess(&attached)) && attached) {
                    dmntClientForceClose();
                    dmntClosed = true;
                }
            }
            if (!debugWatchStart(address, size, maxHits, duration)) {
                DebugWatchStatus status;
                debugWatchGetStatus(&status);
                printf("ERR code=WATCH_BUSY lastError=0x%X\n", status.lastError);
                return 0;
            }
            /* Wait briefly for attach/arm so failures (e.g. the debug handle
             * already owned by dmnt:cht or gdbstub) are reported here instead
             * of requiring a separate watch-status poll. */
            DebugWatchStatus status;
            u32 attempts = 0;
            do {
                svcSleepThread(50 * 1000 * 1000LL);
                debugWatchGetStatus(&status);
            } while (!status.armed && status.active && ++attempts < 20);
            if (!status.active && status.lastError != 0) {
                printf("ERR code=DEBUG_HANDLE_IN_USE result=0x%X stage=%s"
                    " hint=%s\n", status.lastError, status.stage, status.hint);
                return 0;
            }
            printf("OK state=started address=%016lX size=%lu hits=%u"
                " duration=%lu dmntClosed=%d\n", address, size, maxHits,
                duration, dmntClosed);
            return 0;
        }
        if (!strcmp(argv[1], "force-close"))
        {
            if (argc != 2) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            Result rc = dmntClientInitialize();
            if (R_SUCCEEDED(rc))
                rc = dmntClientForceClose();
            printf("OK dmntForceClose=0x%X\n", rc);
            return 0;
        }
        if (!strcmp(argv[1], "watch-stop"))
        {
            if (argc != 2) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            debugWatchStop();
            printf("OK state=stopped\n");
            return 0;
        }
        if (!strcmp(argv[1], "watch-status"))
        {
            if (argc != 2) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            DebugWatchStatus status;
            debugWatchGetStatus(&status);
            printf("OK active=%d armed=%d pid=%016lX address=%016lX size=%lu"
                " maxHits=%u hitCount=%u ctxSlot=%u wpSlot=%u duration=%lu"
                " lastPc=%016lX lastLr=%016lX lastSp=%016lX lastData=%016lX"
                " lastThread=%016lX stage=%s lastError=0x%X hint=%s\n",
                status.active, status.armed, status.processId,
                status.watchAddress, status.watchSize, status.maxHits,
                status.hitCount, status.ctxSlot, status.wpSlot,
                status.durationSeconds, status.lastPc, status.lastLr,
                status.lastSp, status.lastDataAddress, status.lastThreadId,
                status.stage, status.lastError, status.hint);
            return 0;
        }
        if (!strcmp(argv[1], "modules"))
        {
            if (argc != 2) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            u64 pid = 0;
            Result rc = pmdmntGetApplicationProcessId(&pid);
            if (R_FAILED(rc) || pid == 0) {
                printf("ERR code=NO_APP result=0x%X\n", rc);
                return 0;
            }
            /* Requesting more than 2 modules over ldr:dmnt closes the session
             * on this stack (ConnectionClosed 0xF601); the full module map is
             * derived from the main base by host-side tooling instead. */
            LoaderModuleInfo modules[2];
            s32 count = 0;
            rc = ldrDmntGetProcessModuleInfo(pid, modules, 2, &count);
            if (R_FAILED(rc) || count <= 0) {
                printf("ERR code=MODULES_UNAVAILABLE result=0x%X\n", rc);
                return 0;
            }
            printf("OK pid=%016lX count=%d modules=", pid, count);
            s32 i = 0;
            for (i = 0; i < count; i++) {
                printf("%s%d=%016lX+%lX", i ? "," : "", i,
                    modules[i].base_address, modules[i].size);
            }
            printf("\n");
            return 0;
        }
        if (!strcmp(argv[1], "watch-last"))
        {
            if (argc != 2) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            DebugWatchHit hit;
            if (!debugWatchGetLastHit(&hit)) {
                printf("ERR code=NO_HIT\n");
                return 0;
            }
            printf("OK pc=%016lX sp=%016lX lr=%016lX data=%016lX thread=%016lX"
                " insnBytes=%u insn=", hit.pc, hit.sp, hit.x[30],
                hit.dataAddress, hit.threadId, hit.insnBytes);
            u32 i = 0;
            for (i = 0; i < hit.insnBytes; i++)
                printf("%02X", hit.insn[i]);
            printf(" fpStackBytes=%u fpStack=", hit.fpStackBytes);
            for (i = 0; i < hit.fpStackBytes; i++)
                printf("%02X", hit.fpStack[i]);
            printf(" spStackBytes=%u spStack=", hit.spStackBytes);
            for (i = 0; i < hit.spStackBytes; i++)
                printf("%02X", hit.spStack[i]);
            for (i = 0; i < 31; i++)
                printf(" x%u=%016lX", i, hit.x[i]);
            printf("\n");
            return 0;
        }
        if (!strcmp(argv[1], "patch-code"))
        {
            if (argc < 4) {
                printf("ERR code=INVALID_ARGUMENTS\n");
                return 0;
            }
            u64 address = 0;
            if (!tryParseStringToInt(argv[2], &address)) {
                printf("ERR code=INVALID_ADDRESS arg=%s\n", argv[2]);
                return 0;
            }
            bool skipVerify = false;
            u64 expectedSize = 0;
            u8* expected = NULL;
            if (strcmp(argv[3], "-") == 0) {
                skipVerify = true;
            } else {
                expected = parseStringToByteBuffer(argv[3], &expectedSize);
                if (expected == NULL) {
                    printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[3]);
                    return 0;
                }
            }
            u64 patchSize = 0;
            u8* patch = parseStringToByteBuffer(argv[4], &patchSize);
            if (patch == NULL || patchSize == 0
                || patchSize > PATCH_CODE_MAX_SIZE) {
                printf("ERR code=INVALID_PATCH_SIZE size=%lu max=%u\n",
                    patchSize, PATCH_CODE_MAX_SIZE);
                free(expected);
                free(patch);
                return 0;
            }
            if (!skipVerify && expectedSize != patchSize) {
                printf("ERR code=INVALID_PATCH_SIZE\n");
                free(expected);
                free(patch);
                return 0;
            }
            bool checkPc = true;
            u64 pid = 0;
            int a = 0;
            for (a = 5; a < argc; a++) {
                if (strncmp(argv[a], "pid=", 4) == 0) {
                    if (!tryParseStringToInt(argv[a] + 4, &pid)) {
                        printf("ERR code=INVALID_PID arg=%s\n", argv[a]);
                        free(expected);
                        free(patch);
                        return 0;
                    }
                } else if (strcmp(argv[a], "no-pc-check") == 0) {
                    checkPc = false;
                } else {
                    printf("ERR code=INVALID_OPTION arg=%s\n", argv[a]);
                    free(expected);
                    free(patch);
                    return 0;
                }
            }
            PatchCodeResult result;
            Result rc = patchCodeRun(address, expected, expectedSize,
                skipVerify, patch, patchSize, checkPc, pid, &result);
            if (R_FAILED(rc)) {
                printf("ERR code=PATCH_FAILED result=0x%X dmntClosed=%d"
                    " pausedThreads=%u nearestPc=%016lX\n", rc,
                    result.dmntClosed, result.pausedThreads,
                    result.nearestPc);
            } else if (result.status == PatchCodeBusyPcInRange) {
                printf("ERR code=PATCH_BUSY_PC_IN_RANGE thread=%lu"
                    " pc=%016lX dmntClosed=%d\n", result.pcHitThread,
                    result.pcHitValue, result.dmntClosed);
            } else if (result.status == PatchCodeExpectedMismatch) {
                printf("ERR code=PATCH_EXPECTED_MISMATCH old=");
                u32 i = 0;
                for (i = 0; i < result.patchSize; i++)
                    printf("%02X", result.oldBytes[i]);
                printf(" dmntClosed=%d\n", result.dmntClosed);
            } else if (result.status == PatchCodeReadbackFailed) {
                printf("ERR code=PATCH_READBACK_FAILED got=");
                u32 i = 0;
                for (i = 0; i < result.patchSize; i++)
                    printf("%02X", result.newBytes[i]);
                printf(" dmntClosed=%d\n", result.dmntClosed);
            } else {
                printf("OK state=patched size=%lu dmntClosed=%d"
                    " pausedThreads=%u nearestPc=%016lX old=",
                    result.patchSize, result.dmntClosed,
                    result.pausedThreads, result.nearestPc);
                u32 i = 0;
                for (i = 0; i < result.patchSize; i++)
                    printf("%02X", result.oldBytes[i]);
                printf(" new=");
                for (i = 0; i < result.patchSize; i++)
                    printf("%02X", result.newBytes[i]);
                printf(" readbackOk=1\n");
            }
            free(expected);
            free(patch);
            return 0;
        }
        printf("ERR code=UNKNOWN_DEBUG_COMMAND\n");
        return 0;
    }

    if (!strcmp(argv[0], "searchCapabilities"))
    {
        printf("OK version=3 modes=bytes,u8,u16,u32,u64 regions=absolute,heap,main alignment=powerOfTwo,max256 endian=little maxPattern=%d chunk=0x40000 maxResults=%d maxPage=%d refine=exact,changed,unchanged,increased,decreased persistent=runtime storage=sd cRegions=absolute,heap,main,alias,addressSpace\n",
            SEARCH_MAX_PATTERN_SIZE, SEARCH_MAX_RESULTS, SEARCH_MAX_PAGE_RESULTS);
        return 0;
    }

    if (!strcmp(argv[0], "searchStart") || !strcmp(argv[0], "searchExact"))
    {
        if (argc != 4) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        u64 start = 0;
        u64 end = 0;
        if (!tryParseStringToInt(argv[1], &start) || !tryParseStringToInt(argv[2], &end)) {
            printf("ERR code=INVALID_RANGE\n");
            return 0;
        }
        u64 sessionId = 0;
        SearchStartResult result = searchStart(start, end, argv[3], &sessionId);
        if (result == SearchStartInvalidPattern)
            printf("ERR code=INVALID_PATTERN\n");
        else
            printSearchStartResponse(result, sessionId);
        return 0;
    }

    if (!strcmp(argv[0], "searchStartRegion"))
    {
        u64 offset = 0;
        u64 size = 0;
        u64 alignment = 0;
        if ((argc != 6 && argc != 7) || !tryParseStringToInt(argv[3], &offset)
            || !tryParseStringToInt(argv[4], &size)
            || (argc == 7 && !tryParseStringToInt(argv[6], &alignment))) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        u64 sessionId = 0;
        SearchStartResult result = searchStartRegion(argv[1], argv[2], offset, size,
            argv[5], alignment, &sessionId);
        printSearchStartResponse(result, sessionId);
        return 0;
    }

    if (!strcmp(argv[0], "searchBegin"))
    {
        u64 offset = 0, size = 0, alignment = 0, pauseValue = 0;
        if ((argc < 5 || argc > 7) || !tryParseStringToInt(argv[3], &offset)
            || !tryParseStringToInt(argv[4], &size)
            || (argc >= 6 && !tryParseStringToInt(argv[5], &alignment))
            || (argc == 7 && (!tryParseStringToInt(argv[6], &pauseValue) || pauseValue > 1))) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        u64 sessionId = 0;
        SearchStartResult result = searchBeginUnknown(argv[1], argv[2], offset, size,
            alignment, pauseValue == 1, &sessionId);
        printSearchStartResponse(result, sessionId);
        return 0;
    }

    SearchOperation refineOperation = SearchOperationExactScan;
    bool isRefine = true;
    if (!strcmp(argv[0], "searchRefineExact")) refineOperation = SearchOperationRefineExact;
    else if (!strcmp(argv[0], "searchRefineChanged")) refineOperation = SearchOperationRefineChanged;
    else if (!strcmp(argv[0], "searchRefineUnchanged")) refineOperation = SearchOperationRefineUnchanged;
    else if (!strcmp(argv[0], "searchRefineIncreased")) refineOperation = SearchOperationRefineIncreased;
    else if (!strcmp(argv[0], "searchRefineDecreased")) refineOperation = SearchOperationRefineDecreased;
    else isRefine = false;
    if (isRefine)
    {
        const bool exact = refineOperation == SearchOperationRefineExact;
        const int minArgs = exact ? 3 : 2;
        const int maxArgs = exact ? 4 : 3;
        u64 sessionId = 0, pauseValue = 0;
        if (argc < minArgs || argc > maxArgs || !tryParseStringToInt(argv[1], &sessionId)
            || (argc == maxArgs && (!tryParseStringToInt(argv[maxArgs - 1], &pauseValue)
                || pauseValue > 1))) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        const char* value = exact ? argv[2] : NULL;
        SearchStartResult result = searchRefine(sessionId, refineOperation, value,
            pauseValue == 1);
        printSearchStartResponse(result, sessionId);
        return 0;
    }

    if (!strcmp(argv[0], "searchStatus"))
    {
        u64 sessionId = 0;
        SearchStatus status;
        if (argc != 2 || !tryParseStringToInt(argv[1], &sessionId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        if (!searchGetStatus(sessionId, &status)) {
            printf("ERR code=SESSION_NOT_FOUND\n");
            return 0;
        }
        printf("OK session=%lu state=%s start=%016lX end=%016lX scanned=%lu total=%lu matches=%lu stored=%lu truncated=%d readErrors=%lu error=0x%X type=%s region=%s base=%016lX regionOffset=%016lX alignment=%lu backend=%s kind=%s generation=%lu candidates=%lu operation=%s diskBytes=%lu pause=%d committed=%d resumable=%d failure=%s\n",
            status.sessionId, searchStateName(status.state), status.start, status.end,
            status.scanned, status.end - status.start, status.totalMatches,
            status.storedMatches, status.truncated, status.readErrors, status.error,
            searchTypeName(status.type), searchRegionName(status.region), status.regionBase,
            status.regionOffset, status.alignment, processMemoryBackendName(status.backend),
            searchKindName(status.kind), status.generation, status.candidates,
            searchOperationName(status.operation), status.diskBytes, status.pause,
            status.committed, status.resumable, searchFailureName(status.failure));
        return 0;
    }

    if (!strcmp(argv[0], "searchResults"))
    {
        u64 sessionId = 0;
        u64 offset = 0;
        u64 count = 0;
        if (argc != 4 || !tryParseStringToInt(argv[1], &sessionId)
            || !tryParseStringToInt(argv[2], &offset) || !tryParseStringToInt(argv[3], &count)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        u64 addresses[SEARCH_MAX_PAGE_RESULTS];
        u64 copied = 0;
        u64 totalStored = 0;
        SearchResultsResult result = searchCopyResults(sessionId, offset, count, addresses,
            &copied, &totalStored);
        if (result != SearchResultsOk) {
            if (result == SearchResultsNotFound) printf("ERR code=SESSION_NOT_FOUND\n");
            else if (result == SearchResultsBusy) printf("ERR code=BUSY\n");
            else if (result == SearchResultsUnavailable) printf("ERR code=RESULTS_UNAVAILABLE\n");
            else printf("ERR code=CORRUPT_SESSION\n");
            return 0;
        }
        printf("OK session=%lu offset=%lu count=%lu stored=%lu addresses=", sessionId, offset, copied, totalStored);
        for (u64 index = 0; index < copied; index++)
            printf("%s%016lX", index == 0 ? "" : ",", addresses[index]);
        printf("\n");
        return 0;
    }

    if (!strcmp(argv[0], "searchCancel"))
    {
        u64 sessionId = 0;
        if (argc != 2 || !tryParseStringToInt(argv[1], &sessionId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        if (!searchCancel(sessionId))
            printf("ERR code=SESSION_NOT_FOUND\n");
        else
            printf("OK session=%lu cancel=requested\n", sessionId);
        return 0;
    }

    if (!strcmp(argv[0], "searchClose"))
    {
        u64 sessionId = 0;
        if (argc != 2 || !tryParseStringToInt(argv[1], &sessionId)) {
            printf("ERR code=INVALID_ARGUMENTS\n");
            return 0;
        }
        if (!searchClose(sessionId))
            printf("ERR code=SESSION_ACTIVE_OR_NOT_FOUND\n");
        else
            printf("OK session=%lu state=closed\n", sessionId);
        return 0;
    }


    //peek <address in hex or dec> <amount of bytes in hex or dec>
    if (!strcmp(argv[0], "peek"))
    {
        if (argc != 3)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 offset = parseStringToInt(argv[1]);
        u64 size = parseStringToInt(argv[2]);
        peekInfinite(meta.heap_base + offset, size);
    }

    if (!strcmp(argv[0], "peekMulti"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 itemCount = (argc - 1) / 2;
        u64 offsets[itemCount];
        u64 sizes[itemCount];

        for (int i = 0; i < itemCount; ++i)
        {
            offsets[i] = meta.heap_base + parseStringToInt(argv[(i * 2) + 1]);
            sizes[i] = parseStringToInt(argv[(i * 2) + 2]);
        }
        peekMulti(offsets, sizes, itemCount);
    }

    if (!strcmp(argv[0], "peekAbsolute"))
    {
        if (argc != 3)
            return 0;

        u64 offset = parseStringToInt(argv[1]);
        u64 size = parseStringToInt(argv[2]);
        peekInfinite(offset, size);
    }

    if (!strcmp(argv[0], "peekAbsoluteMulti"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        u64 itemCount = (argc - 1) / 2;
        u64 offsets[itemCount];
        u64 sizes[itemCount];

        for (int i = 0; i < itemCount; ++i)
        {
            offsets[i] = parseStringToInt(argv[(i * 2) + 1]);
            sizes[i] = parseStringToInt(argv[(i * 2) + 2]);
        }
        peekMulti(offsets, sizes, itemCount);
    }

    if (!strcmp(argv[0], "peekMain"))
    {
        if (argc != 3)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 offset = parseStringToInt(argv[1]);
        u64 size = parseStringToInt(argv[2]);
        peekInfinite(meta.main_nso_base + offset, size);
    }

    if (!strcmp(argv[0], "peekMainMulti"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 itemCount = (argc - 1) / 2;
        u64 offsets[itemCount];
        u64 sizes[itemCount];

        for (int i = 0; i < itemCount; ++i)
        {
            offsets[i] = meta.main_nso_base + parseStringToInt(argv[(i * 2) + 1]);
            sizes[i] = parseStringToInt(argv[(i * 2) + 2]);
        }
        peekMulti(offsets, sizes, itemCount);
    }

    //poke <address in hex or dec> <data in hex or dec>
    if (!strcmp(argv[0], "poke"))
    {
        if (argc != 3)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 offset = 0;
        if (!tryParseStringToInt(argv[1], &offset)) {
            printf("ERR code=INVALID_ADDRESS arg=%s\n", argv[1]);
            return 0;
        }
        u64 size = 0;
        u8* data = parseStringToByteBuffer(argv[2], &size);
        if (data == NULL) {
            printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[2]);
            return 0;
        }
        poke(meta.heap_base + offset, size, data);
        free(data);
    }

    if (!strcmp(argv[0], "pokeAbsolute"))
    {
        if (argc != 3)
            return 0;

        u64 offset = 0;
        if (!tryParseStringToInt(argv[1], &offset)) {
            printf("ERR code=INVALID_ADDRESS arg=%s\n", argv[1]);
            return 0;
        }
        u64 size = 0;
        u8* data = parseStringToByteBuffer(argv[2], &size);
        if (data == NULL) {
            printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[2]);
            return 0;
        }
        poke(offset, size, data);
        free(data);
    }

    if (!strcmp(argv[0], "pokeMain"))
    {
        if (argc != 3)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 offset = 0;
        if (!tryParseStringToInt(argv[1], &offset)) {
            printf("ERR code=INVALID_ADDRESS arg=%s\n", argv[1]);
            return 0;
        }
        u64 size = 0;
        u8* data = parseStringToByteBuffer(argv[2], &size);
        if (data == NULL) {
            printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[2]);
            return 0;
        }
        poke(meta.main_nso_base + offset, size, data);
        free(data);
    }

    //click <buttontype>
    if (!strcmp(argv[0], "click"))
    {
        if (argc != 2)
            return 0;
        HidNpadButton key = parseStringToButton(argv[1]);
        click(key);
    }

    //clickSeq <sequence> eg clickSeq A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0 (some params don't parse correctly, such as DDOWN so use the alt)
    //syntax: <button>=click, 'W<number>'=wait/sleep thread, '+<button>'=press button, '-<button>'=release button, '%<x axis>,<y axis>'=move L stick <x axis, y axis>, '&<x axis>,<y axis>'=move R stick <x axis, y axis> 
    if (!strcmp(argv[0], "clickSeq"))
    {
        if (argc != 2)
            return 0;

        u64 sizeArg = strlen(argv[1]) + 1;
        char* seqNew = malloc(sizeArg);
        if (seqNew == NULL) {
            printf("\n");
            return 0;
        }
        strcpy(seqNew, argv[1]);
        makeClickSeq(seqNew);
    }

    if (!strcmp(argv[0], "clickCancel"))
        clickToken = 1;

    //hold <buttontype>
    if (!strcmp(argv[0], "press"))
    {
        if (argc != 2)
            return 0;
        HidNpadButton key = parseStringToButton(argv[1]);
        press(key);
    }

    //release <buttontype>
    if (!strcmp(argv[0], "release"))
    {
        if (argc != 2)
            return 0;
        HidNpadButton key = parseStringToButton(argv[1]);
        release(key);
    }

    //setStick <left or right stick> <x value> <y value>
    if (!strcmp(argv[0], "setStick"))
    {
        if (argc != 4)
            return 0;

        int side = 0;
        if (!strcmp(argv[1], "LEFT")) {
            side = JOYSTICK_LEFT;
        }
        else if (!strcmp(argv[1], "RIGHT")) {
            side = JOYSTICK_RIGHT;
        }
        else {
            return 0;
        }

        int dxVal = strtol(argv[2], NULL, 0);
        if (dxVal > JOYSTICK_MAX) dxVal = JOYSTICK_MAX; //0x7FFF
        if (dxVal < JOYSTICK_MIN) dxVal = JOYSTICK_MIN; //-0x8000
        int dyVal = strtol(argv[3], NULL, 0);
        if (dyVal > JOYSTICK_MAX) dyVal = JOYSTICK_MAX;
        if (dyVal < JOYSTICK_MIN) dyVal = JOYSTICK_MIN;

        setStickState(side, dxVal, dyVal);
    }

    //detachController
    if (!strcmp(argv[0], "detachController"))
    {
        detachController();
    }
    if (!strcmp(argv[0], "game"))
    {
        if (argc != 2)
            return 0;
        NsApplicationControlData* buf = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
        u64 outsize = getoutsize(buf);
        NacpLanguageEntry* langentry = NULL;
        if (outsize != 0) {
            if (!strcmp(argv[1], "icon")) {
                u64 i;
                for (i = 0; i < outsize - sizeof(buf->nacp); i++)
                {
                    printf("%02X", buf->icon[i]);
                }
                printf("\n");
            }
            if (!strcmp(argv[1], "version"))
            {
                char version[0x11];
                memset(version, 0, sizeof(version));
                strncpy(version, buf->nacp.display_version, sizeof(version));
                printf("%s\n", version);
            }
            if (!strcmp(argv[1], "rating"))
            {
                printf("%d\n", buf->nacp.rating_age[0]);
            }
            if (!strcmp(argv[1], "author"))
            {
                char author[0x101];
                nacpGetLanguageEntry(&buf->nacp, &langentry);
                memset(author, 0, sizeof(author));
                strncpy(author, langentry->author, sizeof(author));
                printf("%s\n", author);
            }
            if (!strcmp(argv[1], "name"))
            {
                char name[0x201];
                nacpGetLanguageEntry(&buf->nacp, &langentry);
                memset(name, 0, sizeof(name));
                strncpy(name, langentry->name, sizeof(name));
                printf("%s\n", name);
            }
        }
        free(buf);
    }
    //configure <mainLoopSleepTime or buttonClickSleepTime> <time in ms>
    if (!strcmp(argv[0], "configure")) {
        if (argc != 3)
            return 0;

        if (!strcmp(argv[1], "mainLoopSleepTime")) {
            u64 time = parseStringToInt(argv[2]);
            mainLoopSleepTime = time;
        }

        if (!strcmp(argv[1], "buttonClickSleepTime")) {
            u64 time = parseStringToInt(argv[2]);
            buttonClickSleepTime = time;
        }

        if (!strcmp(argv[1], "echoCommands")) {
            u64 shouldActivate = parseStringToInt(argv[2]);
            echoCommands = shouldActivate != 0;
        }

        if (!strcmp(argv[1], "printDebugResultCodes")) {
            u64 shouldActivate = parseStringToInt(argv[2]);
            debugResultCodes = shouldActivate != 0;
        }

        if (!strcmp(argv[1], "keySleepTime")) {
            u64 keyTime = parseStringToInt(argv[2]);
            keyPressSleepTime = keyTime;
        }

        if (!strcmp(argv[1], "fingerDiameter")) {
            u32 fDiameter = (u32)parseStringToInt(argv[2]);
            fingerDiameter = fDiameter;
        }

        if (!strcmp(argv[1], "pollRate")) {
            u64 fPollRate = parseStringToInt(argv[2]);
            pollRate = fPollRate;
        }

        if (!strcmp(argv[1], "freezeRate")) {
            u64 fFreezeRate = parseStringToInt(argv[2]);
            freezeRate = fFreezeRate;
        }

        if (!strcmp(argv[1], "controllerType")) {
            detachController();
            u8 fControllerType = (u8)parseStringToInt(argv[2]);
            mutexLock(&controllerMutex);
            controllerInitializedType = fControllerType;
            mutexUnlock(&controllerMutex);
        }
    }

    if (!strcmp(argv[0], "getTitleID")) {
        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }
        printf("%016lX\n", meta.titleID);
    }

    if (!strcmp(argv[0], "getTitleVersion")) {
        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }
        printf("%016lX\n", meta.titleVersion);
    }

    if (!strcmp(argv[0], "getSystemLanguage")) {
        //thanks zaksa
        setInitialize();
        u64 languageCode = 0;
        SetLanguage language = SetLanguage_ENUS;
        setGetSystemLanguage(&languageCode);
        setMakeLanguage(languageCode, &language);
        printf("%d\n", language);
    }

    if (!strcmp(argv[0], "getMainNsoBase")) {
        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }
        printf("%016lX\n", meta.main_nso_base);
    }

    if (!strcmp(argv[0], "getBuildID")) {
        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }
        printf("%02x%02x%02x%02x%02x%02x%02x%02x\n", meta.buildID[0], meta.buildID[1], meta.buildID[2], meta.buildID[3], meta.buildID[4], meta.buildID[5], meta.buildID[6], meta.buildID[7]);

    }

    if (!strcmp(argv[0], "getHeapBase")) {
        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }
        printf("%016lX\n", meta.heap_base);
    }

    if (!strcmp(argv[0], "isProgramRunning")) {
        if (argc != 2)
            return 0;
        u64 programId = parseStringToInt(argv[1]);
        bool isRunning = getIsProgramOpen(programId);
        printf("%d\n", isRunning);
    }

    if (!strcmp(argv[0], "screenCapture") || !strcmp(argv[0], "pixelPeek")) {
        //errors with 0x668CE, unless debugunit flag is patched
        u64 bSize = 0x7D000;
        char* buf = malloc(bSize);
        u64 outSize = 0;

        Result rc = capsscCaptureForDebug(buf, bSize, &outSize);

        if (R_FAILED(rc) && debugResultCodes)
            printf("capssc, 1204: %d\n", rc);

        u64 i;
        for (i = 0; i < outSize; i++)
        {
            printf("%02X", buf[i]);
        }
        printf("\n");

        free(buf);
    }

    if (!strcmp(argv[0], "getVersion")) {
        printf("%s\n", VERSION_S);
    }

    // follow pointers and print absolute offset (little endian, flip it yourself if required)
    // pointer <first (main) jump> <additional jumps> !!do not add the last jump in pointerexpr here, add it yourself!!
    if (!strcmp(argv[0], "pointer"))
    {
        if (argc < 2)
            return 0;
        s64 jumps[argc - 1];
        for (int i = 1; i < argc; i++)
            jumps[i - 1] = parseStringToSignedLong(argv[i]);
        u64 solved = followMainPointer(jumps, argc - 1);
        printf("%016lX\n", solved);
    }

    // pointerAll <first (main) jump> <additional jumps> <final jump in pointerexpr> 
    // possibly redundant between the one above, one needs to go eventually. (little endian, flip it yourself if required)
    if (!strcmp(argv[0], "pointerAll"))
    {
        if (argc < 3)
            return 0;
        s64 finalJump = parseStringToSignedLong(argv[argc - 1]);
        u64 count = argc - 2;
        s64 jumps[count];
        for (int i = 1; i < argc - 1; i++)
            jumps[i - 1] = parseStringToSignedLong(argv[i]);
        u64 solved = followMainPointer(jumps, count);
        if (solved != 0)
            solved += finalJump;
        printf("%016lX\n", solved);
    }

    // pointerRelative <first (main) jump> <additional jumps> <final jump in pointerexpr> 
    // returns offset relative to heap
    if (!strcmp(argv[0], "pointerRelative"))
    {
        if (argc < 3)
            return 0;
        s64 finalJump = parseStringToSignedLong(argv[argc - 1]);
        u64 count = argc - 2;
        s64 jumps[count];
        for (int i = 1; i < argc - 1; i++)
            jumps[i - 1] = parseStringToSignedLong(argv[i]);
        u64 solved = followMainPointer(jumps, count);
        if (solved != 0)
        {
            solved += finalJump;
            MetaData meta = getMetaData();
            if (meta.main_nso_base == 0)
            {
                printf("\n");
                return 0;
            }
            solved -= meta.heap_base;
        }
        printf("%016lX\n", solved);
    }

    // pointerPeek <amount of bytes in hex or dec> <first (main) jump> <additional jumps> <final jump in pointerexpr>
    // warning: no validation
    if (!strcmp(argv[0], "pointerPeek"))
    {
        if (argc < 4)
            return 0;

        s64 finalJump = parseStringToSignedLong(argv[argc - 1]);
        u64 size = parseStringToInt(argv[1]);
        u64 count = argc - 3;
        s64 jumps[count];
        for (int i = 2; i < argc - 1; i++)
            jumps[i - 2] = parseStringToSignedLong(argv[i]);
        u64 solved = followMainPointer(jumps, count);
        solved += finalJump;
        peek(solved, size);
    }

    // pointerPeekMulti <amount of bytes in hex or dec> <first (main) jump> <additional jumps> <final jump in pointerexpr> split by asterisks (*)
    // warning: no validation
    if (!strcmp(argv[0], "pointerPeekMulti"))
    {
        if (argc < 4)
            return 0;

        // we guess a max of 40 for now
        u64 offsets[40];
        u64 sizes[40];
        u64 itemCount = 0;

        u64 currIndex = 1;
        u64 lastIndex = 1;

        while (currIndex < argc)
        {
            // count first
            char* thisArg = argv[currIndex];
            while (strcmp(thisArg, "*"))
            {
                currIndex++;
                if (currIndex < argc)
                    thisArg = argv[currIndex];
                else
                    break;
            }

            u64 thisCount = currIndex - lastIndex;

            s64 finalJump = parseStringToSignedLong(argv[currIndex - 1]);
            u64 size = parseStringToSignedLong(argv[lastIndex]);
            u64 count = thisCount - 2;
            s64 jumps[count];
            for (int i = 1; i < count + 1; i++)
                jumps[i - 1] = parseStringToSignedLong(argv[i + lastIndex]);
            u64 solved = followMainPointer(jumps, count);
            solved += finalJump;

            offsets[itemCount] = solved;
            sizes[itemCount] = size;
            itemCount++;
            currIndex++;
            lastIndex = currIndex;
        }

        peekMulti(offsets, sizes, itemCount);
    }

    // pointerPoke <data to be sent> <first (main) jump> <additional jumps> <final jump in pointerexpr>
    // warning: no validation
    if (!strcmp(argv[0], "pointerPoke"))
    {
        if (argc < 4)
            return 0;

        s64 finalJump = parseStringToSignedLong(argv[argc - 1]);
        u64 count = argc - 3;
        s64 jumps[count];
        for (int i = 2; i < argc - 1; i++)
            jumps[i - 2] = parseStringToSignedLong(argv[i]);
        u64 solved = followMainPointer(jumps, count);
        solved += finalJump;

        u64 size;
        u8* data = parseStringToByteBuffer(argv[1], &size);
        if (data == NULL) {
            printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[1]);
            return 0;
        }
        poke(solved, size, data);
        free(data);
    }

    // add to freeze map
    if (!strcmp(argv[0], "freeze"))
    {
        if (argc != 3)
            return 0;

        MetaData meta = getMetaData();
        if (meta.main_nso_base == 0)
        {
            printf("\n");
            return 0;
        }

        u64 offset = 0;
        if (!tryParseStringToInt(argv[1], &offset)) {
            printf("ERR code=INVALID_ADDRESS arg=%s\n", argv[1]);
            return 0;
        }
        u64 size = 0;
        u8* data = parseStringToByteBuffer(argv[2], &size);
        if (data == NULL) {
            printf("ERR code=INVALID_HEX_PAYLOAD arg=%s\n", argv[2]);
            return 0;
        }
        addToFreezeMap(offset, data, size, meta.titleID);
    }

    // remove from freeze map
    if (!strcmp(argv[0], "unFreeze"))
    {
        if (argc != 2)
            return 0;

        u64 offset = parseStringToInt(argv[1]);
        removeFromFreezeMap(offset);
    }

    // get count of offsets being frozen
    if (!strcmp(argv[0], "freezeCount"))
    {
        getFreezeCount(true);
    }

    // clear all freezes
    if (!strcmp(argv[0], "freezeClear"))
    {
        clearFreezes();
        freeze_thr_state = Idle;
    }

    if (!strcmp(argv[0], "freezePause"))
        freeze_thr_state = Pause;

    if (!strcmp(argv[0], "freezeUnpause"))
        freeze_thr_state = Active;

    //touch followed by arrayof: <x in the range 0-1280> <y in the range 0-720>. Array is sequential taps, not different fingers. Functions in its own thread, but will not allow the call again while running. tapcount * pollRate * 2
    if (!strcmp(argv[0], "touch"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        u32 count = (argc - 1) / 2;
        HidTouchState* state = calloc(count, sizeof(HidTouchState));
        u32 i, j = 0;
        for (i = 0; i < count; ++i)
        {
            state[i].diameter_x = state[i].diameter_y = fingerDiameter;
            state[i].x = (u32)parseStringToInt(argv[++j]);
            state[i].y = (u32)parseStringToInt(argv[++j]);
        }

        makeTouch(state, count, pollRate * 1e+6L, false);
    }

    //touchHold <x in the range 0-1280> <y in the range 0-720> <time in milliseconds (must be at least 15ms)>. Functions in its own thread, but will not allow the call again while running. pollRate + holdtime
    if (!strcmp(argv[0], "touchHold")) {
        if (argc != 4)
            return 0;

        HidTouchState* state = calloc(1, sizeof(HidTouchState));
        state->diameter_x = state->diameter_y = fingerDiameter;
        state->x = (u32)parseStringToInt(argv[1]);
        state->y = (u32)parseStringToInt(argv[2]);
        u64 time = parseStringToInt(argv[3]);
        makeTouch(state, 1, time * 1e+6L, false);
    }

    //touchDraw followed by arrayof: <x in the range 0-1280> <y in the range 0-720>. Array is vectors of where finger moves to, then removes the finger. Functions in its own thread, but will not allow the call again while running. (vectorcount * pollRate * 2) + pollRate
    if (!strcmp(argv[0], "touchDraw"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        u32 count = (argc - 1) / 2;
        HidTouchState* state = calloc(count, sizeof(HidTouchState));
        u32 i, j = 0;
        for (i = 0; i < count; ++i)
        {
            state[i].diameter_x = state[i].diameter_y = fingerDiameter;
            state[i].x = (u32)parseStringToInt(argv[++j]);
            state[i].y = (u32)parseStringToInt(argv[++j]);
        }

        makeTouch(state, count, pollRate * 1e+6L * 2, true);
    }

    if (!strcmp(argv[0], "touchCancel"))
        touchToken = 1;

    //key followed by arrayof: <HidKeyboardKey> to be pressed in sequential order
    //thank you Red (hp3721) for this functionality
    if (!strcmp(argv[0], "key"))
    {
        if (argc < 2)
            return 0;

        u64 count = argc - 1;
        HiddbgKeyboardAutoPilotState* keystates = calloc(count, sizeof(HiddbgKeyboardAutoPilotState));
        u64 i;
        for (i = 0; i < count; i++)
        {
            u8 key = (u8)parseStringToInt(argv[i + 1]);
            if (key < 4 || key > 231)
                continue;
            keystates[i].keys[key / 64] = 1UL << key;
            keystates[i].modifiers = 1024UL; //numlock
        }

        makeKeys(keystates, count);
    }

    //keyMod followed by arrayof: <HidKeyboardKey> <HidKeyboardModifier>(without the bitfield shift) to be pressed in sequential order
    if (!strcmp(argv[0], "keyMod"))
    {
        if (argc < 3 || argc % 2 == 0)
            return 0;

        u32 count = (argc - 1) / 2;
        HiddbgKeyboardAutoPilotState* keystates = calloc(count, sizeof(HiddbgKeyboardAutoPilotState));
        u64 i, j = 0;
        for (i = 0; i < count; i++)
        {
            u8 key = (u8)parseStringToInt(argv[++j]);
            if (key < 4 || key > 231)
                continue;
            keystates[i].keys[key / 64] = 1UL << key;
            keystates[i].modifiers = BIT((u8)parseStringToInt(argv[++j]));
        }

        makeKeys(keystates, count);
    }

    //keyMulti followed by arrayof: <HidKeyboardKey> to be pressed at the same time.
    if (!strcmp(argv[0], "keyMulti"))
    {
        if (argc < 2)
            return 0;

        u64 count = argc - 1;
        HiddbgKeyboardAutoPilotState* keystate = calloc(1, sizeof(HiddbgKeyboardAutoPilotState));
        u64 i;
        for (i = 0; i < count; i++)
        {
            u8 key = (u8)parseStringToInt(argv[i + 1]);
            if (key < 4 || key > 231)
                continue;
            keystate[0].keys[key / 64] |= 1UL << key;
        }

        makeKeys(keystate, 1);
    }

    //turns off the screen (display)
    if (!strcmp(argv[0], "screenOff"))
    {
        ViDisplay temp_display;
        Result rc = viOpenDisplay("Internal", &temp_display);
        if (R_FAILED(rc))
            rc = viOpenDefaultDisplay(&temp_display);
        if (R_SUCCEEDED(rc))
        {
            rc = viSetDisplayPowerState(&temp_display, ViPowerState_NotScanning); // not scanning keeps the screen on but does not push new pixels to the display. Battery save is non-negligible and should be used where possible
            svcSleepThread(1e+6l);
            viCloseDisplay(&temp_display);

            rc = lblInitialize();
            if (R_FAILED(rc))
                fatalThrow(rc);
            lblSwitchBacklightOff(1ul);
            lblExit();
        }
    }

    //turns on the screen (display)
    if (!strcmp(argv[0], "screenOn"))
    {
        ViDisplay temp_display;
        Result rc = viOpenDisplay("Internal", &temp_display);
        if (R_FAILED(rc))
            rc = viOpenDefaultDisplay(&temp_display);
        if (R_SUCCEEDED(rc))
        {
            rc = viSetDisplayPowerState(&temp_display, ViPowerState_On);
            svcSleepThread(1e+6l);
            viCloseDisplay(&temp_display);

            rc = lblInitialize();
            if (R_FAILED(rc))
                fatalThrow(rc);
            lblSwitchBacklightOn(1ul);
            lblExit();
        }
    }

    if (!strcmp(argv[0], "charge"))
    {
        u32 charge;
        Result rc = psmInitialize();
        if (R_FAILED(rc))
            fatalThrow(rc);
        psmGetBatteryChargePercentage(&charge);
        printf("%d\n", charge);
        psmExit();
    }

    if (!strcmp(argv[0], "fdCount"))
    {
        printf("%d\n", fd_count);
    }

    return 0;
}

void add_to_pfds(struct pollfd* pfds[], int newfd, int* fd_count, int* fd_size)
{
    if (*fd_count == *fd_size) {
        *fd_size *= 2;

        *pfds = realloc(*pfds, sizeof(**pfds) * (*fd_size));
    }

    (*pfds)[*fd_count].fd = newfd;
    (*pfds)[*fd_count].events = POLLIN;

    (*fd_count)++;
}

void del_from_pfds(struct pollfd pfds[], int i, int* fd_count)
{
    pfds[i] = pfds[*fd_count - 1];

    (*fd_count)--;
}

int main()
{
    char* linebuf = malloc(sizeof(char) * MAX_LINE_LENGTH);

    int c = sizeof(struct sockaddr_in);
    struct sockaddr_in client;

    struct pollfd* pfds = malloc(sizeof * pfds * fd_size);

    int listenfd = setupServerSocket();
    pfds[0].fd = listenfd;
    pfds[0].events = POLLIN;
    fd_count = 1;

    int newfd;

    Result rc;
    int fr_count = 0;

    // Hackfix found by Red: an unused key press (KBD_MEDIA_CALC) is required to
    // allow sequential same-key presses. Set once before threads start so the
    // keyboard worker never races with controller (re)initialization.
    dummyKeyboardState.keys[3] = 0x800000000000000UL; // bitfield[3]

    initFreezes();
    processMemoryInitialize();
    debugWatchInitialize();
    searchInitialize(searchSdMounted);
    ftpServerInitialize(searchSdMounted);

    // freeze thread
    mutexInit(&freezeMutex);
    rc = threadCreate(&freezeThread, sub_freeze, (void*)&freeze_thr_state, NULL, THREAD_SIZE, 0x2C, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&freezeThread);

    // touch thread
    mutexInit(&touchMutex);
    rc = threadCreate(&touchThread, sub_touch, (void*)&currentTouchEvent, NULL, THREAD_SIZE, 0x2C, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&touchThread);

    // key thread
    mutexInit(&keyMutex);
    rc = threadCreate(&keyboardThread, sub_key, (void*)&currentKeyEvent, NULL, THREAD_SIZE, 0x2C, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&keyboardThread);

    // click sequence thread
    mutexInit(&clickMutex);
    mutexInit(&controllerMutex);
    rc = threadCreate(&clickThread, sub_click, (void*)currentClick, NULL, THREAD_SIZE, 0x2C, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&clickThread);

    flashLed();

    while (true)
    {
        poll(pfds, fd_count, -1);
        mutexLock(&freezeMutex);
        for (int i = 0; i < fd_count; i++)
        {
            if (pfds[i].revents & POLLIN)
            {
                if (pfds[i].fd == listenfd)
                {
                    newfd = accept(listenfd, (struct sockaddr*)&client, (socklen_t*)&c);
                    if (newfd != -1)
                    {
                        add_to_pfds(&pfds, newfd, &fd_count, &fd_size);
                    }
                    else {
                        svcSleepThread(1e+9L);
                        close(listenfd);
                        listenfd = setupServerSocket();
                        pfds[0].fd = listenfd;
                        pfds[0].events = POLLIN;
                        break;
                    }
                }
                else
                {
                    bool readEnd = false;
                    int readBytesSoFar = 0;
                    while (!readEnd) {
                        int len = recv(pfds[i].fd, &linebuf[readBytesSoFar], 1, 0);
                        if (len <= 0)
                        {
                            close(pfds[i].fd);
                            del_from_pfds(pfds, i, &fd_count);
                            readEnd = true;
                        }
                        else
                        {
                            readBytesSoFar += len;
                            if (readBytesSoFar >= MAX_LINE_LENGTH) {
                                close(pfds[i].fd);
                                del_from_pfds(pfds, i, &fd_count);
                                readEnd = true;
                                continue;
                            }
                            if (linebuf[readBytesSoFar - 1] == '\n') {
                                readEnd = true;
                                linebuf[readBytesSoFar - 1] = 0;

                                fflush(stdout);
                                dup2(pfds[i].fd, STDOUT_FILENO);

                                parseArgs(linebuf, &argmain);

                                if (echoCommands) {
                                    printf("%s\n", linebuf);
                                }
                            }
                        }
                    }
                }
            }
        }
        fr_count = getFreezeCount(false);
        if (fr_count == 0)
            freeze_thr_state = Idle;
        mutexUnlock(&freezeMutex);
        svcSleepThread(mainLoopSleepTime * 1e+6L);
    }

    if (R_SUCCEEDED(rc))
    {
        freeze_thr_state = Exit;
        threadWaitForExit(&freezeThread);
        threadClose(&freezeThread);
        currentTouchEvent.state = 3;
        threadWaitForExit(&touchThread);
        threadClose(&touchThread);
        currentKeyEvent.state = 3;
        threadWaitForExit(&keyboardThread);
        threadClose(&keyboardThread);
        clickThreadState = 1;
        threadWaitForExit(&clickThread);
    }

    clearFreezes();
    freeFreezes();
    ftpServerShutdown();
    searchShutdown();
    processMemoryExit();

    return 0;
}

void sub_freeze(void* arg)
{
    u64 heap_base;
    u64 tid_now = 0;
    bool wait_su = false;
    int freezecount = 0;

IDLE:while (freezecount == 0)
{
    if (*(FreezeThreadState*)arg == Exit)
        break;

    // do nothing
    svcSleepThread(1e+9L);
    freezecount = getFreezeCount(false);
}

while (1)
{
    if (*(FreezeThreadState*)arg == Exit)
        break;

    if (searchMemoryAccessBlocked()) {
        svcSleepThread(1e+8L);
        continue;
    }

    if (*(FreezeThreadState*)arg == Idle) // no freeze
    {
        mutexLock(&freezeMutex);
        freeze_thr_state = Active;
        mutexUnlock(&freezeMutex); // stupid but it works so is it really stupid? (yes)
        freezecount = 0;
        wait_su = false;
        goto IDLE;
    }
    else if (*(FreezeThreadState*)arg == Pause)
    {
        svcSleepThread(1e+8L); //1s
        continue;
    }

    mutexLock(&freezeMutex);
    // The search can be queued after the lock-free check above.  Recheck while
    // holding freezeMutex: search commands are dispatched under this same lock,
    // so a running C-level scan can no longer race us into waiting for the
    // process-memory backend while blocking the TCP command loop.
    if (searchMemoryAccessBlocked()) {
        mutexUnlock(&freezeMutex);
        svcSleepThread(1e+8L);
        continue;
    }
    ProcessMemorySession memorySession;
    Result memoryRc = processMemoryOpen(&memorySession, false);
    if (R_FAILED(memoryRc)) {
        mutexUnlock(&freezeMutex);
        svcSleepThread(1e+9L);
        continue;
    }
    const ProcessMemoryMetadata* metadata = processMemoryGetMetadata(&memorySession);
    heap_base = metadata->heapBase;
    tid_now = metadata->titleId;

    // don't freeze on startup of new tid to remove any chance of save corruption
    if (tid_now == 0)
    {
        processMemoryClose(&memorySession);
        mutexUnlock(&freezeMutex);
        svcSleepThread(1e+10L);
        wait_su = true;
        continue;
    }

    if (wait_su)
    {
        processMemoryClose(&memorySession);
        mutexUnlock(&freezeMutex);
        svcSleepThread(3e+10L);
        wait_su = false;
        continue;
    }

    if (heap_base > 0)
    {
        for (int j = 0; j < FREEZE_DIC_LENGTH; j++)
        {
            if (freezes[j].state == 1 && freezes[j].titleId == tid_now)
            {
                processMemoryWrite(&memorySession, freezes[j].vData,
                    heap_base + freezes[j].address, freezes[j].size);
            }
        }
    }
    processMemoryClose(&memorySession);

    mutexUnlock(&freezeMutex);
    svcSleepThread(freezeRate * 1e+6L);
    tid_now = 0;
}
}

void sub_touch(void* arg)
{
    while (1)
    {
        TouchData* touchPtr = (TouchData*)arg;
        if (touchPtr->state == 1)
        {
            mutexLock(&touchMutex); // don't allow any more assignments to the touch var (will lock the main thread)
            touch(touchPtr->states, touchPtr->sequentialCount, touchPtr->holdTime, touchPtr->hold, &touchToken);
            free(touchPtr->states);
            touchPtr->state = 0;
            mutexUnlock(&touchMutex);
        }

        svcSleepThread(1e+6L);

        touchToken = 0;

        if (touchPtr->state == 3)
            break;
    }
}

void sub_key(void* arg)
{
    while (1)
    {
        KeyData* keyPtr = (KeyData*)arg;
        if (keyPtr->state == 1)
        {
            mutexLock(&keyMutex);
            key(keyPtr->states, keyPtr->sequentialCount);
            free(keyPtr->states);
            keyPtr->state = 0;
            mutexUnlock(&keyMutex);
        }

        svcSleepThread(1e+6L);

        if (keyPtr->state == 3)
            break;
    }
}

void sub_click(void* arg)
{
    while (1)
    {
        if (clickThreadState == 1)
            break;

        if (currentClick != NULL)
        {
            mutexLock(&clickMutex);
            clickSequence(currentClick, &clickToken);
            free(currentClick); currentClick = NULL;
            mutexUnlock(&clickMutex);
            printf("done\n");
        }

        clickToken = 0;

        svcSleepThread(1e+6L);
    }
}
