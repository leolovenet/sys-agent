#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "commands.h"
#include "search.h"
#include "search_match.h"
#include "search_range.h"
#include "search_value.h"
#include "process_memory.h"
#include "unknown_search.h"

#define SEARCH_CHUNK_SIZE 0x40000
#define SEARCH_THREAD_STACK_SIZE 0x10000
#define SEARCH_THREAD_PRIORITY 0x2D

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadStarted;
    bool exitRequested;
    bool cancelRequested;
    u64 nextSessionId;
    u64 processId;
    ProcessMemoryBackendKind backend;
    SearchStatus status;
    u8 pattern[SEARCH_MAX_PATTERN_SIZE];
    size_t patternSize;
    size_t alignment;
    u64* results;
} SearchManager;

typedef struct {
    SearchManager* manager;
} MatchContext;

static SearchManager manager;

static int hexNibble(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

static bool parsePattern(const char* text, u8* output, size_t* outputSize)
{
    if (text == NULL)
        return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;

    const size_t length = strlen(text);
    if (length == 0 || (length & 1) != 0 || length / 2 > SEARCH_MAX_PATTERN_SIZE)
        return false;

    for (size_t index = 0; index < length / 2; index++) {
        const int high = hexNibble(text[index * 2]);
        const int low = hexNibble(text[index * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        output[index] = (u8)((high << 4) | low);
    }
    *outputSize = length / 2;
    return true;
}

static bool shouldCancel(void)
{
    bool cancelled;
    mutexLock(&manager.mutex);
    cancelled = manager.cancelRequested || manager.exitRequested;
    mutexUnlock(&manager.mutex);
    return cancelled;
}

static bool processIsCurrent(void)
{
    u64 currentProcessId = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&currentProcessId)))
        return false;
    return currentProcessId == manager.processId;
}

static Result queryMapping(u64 address, MemoryInfo* info)
{
    ProcessMemorySession session;
    Result rc = processMemoryOpenBackend(&session, manager.backend, manager.processId, false);
    if (R_SUCCEEDED(rc)) {
        rc = processMemoryQuery(&session, info, address);
        processMemoryClose(&session);
    }
    return rc;
}

static Result readProcessMemoryQuiet(void* output, u64 address, size_t size)
{
    ProcessMemorySession session;
    Result rc = processMemoryOpenBackend(&session, manager.backend, manager.processId, false);
    if (R_SUCCEEDED(rc)) {
        rc = processMemoryRead(&session, output, address, size);
        processMemoryClose(&session);
    }
    return rc;
}

static void recordMatch(u64 address, void* context)
{
    MatchContext* matchContext = (MatchContext*)context;
    SearchManager* current = matchContext->manager;
    current->status.totalMatches++;
    if (current->status.storedMatches < SEARCH_MAX_RESULTS) {
        current->results[current->status.storedMatches] = address;
        current->status.storedMatches++;
    }
    else {
        current->status.truncated = true;
    }
}

static void finishSearch(SearchState state, Result error)
{
    mutexLock(&manager.mutex);
    manager.status.state = state;
    manager.status.error = error;
    mutexUnlock(&manager.mutex);
}

static void runSearch(void)
{
    u8* buffer = malloc(SEARCH_CHUNK_SIZE + SEARCH_MAX_PATTERN_SIZE - 1);
    if (buffer == NULL) {
        finishSearch(SearchStateError, MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        return;
    }

    mutexLock(&manager.mutex);
    manager.status.state = SearchStateRunning;
    const u64 end = manager.status.end;
    u64 cursor = manager.status.start;
    mutexUnlock(&manager.mutex);

    Result failure = 0;
    while (cursor < end && !shouldCancel()) {
        if (!processIsCurrent()) {
            failure = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            break;
        }

        MemoryInfo info = { 0 };
        failure = queryMapping(cursor, &info);
        if (R_FAILED(failure))
            break;

        SearchRangePlan plan = searchPlanMapping(cursor, end, info.addr, info.size,
            (info.perm & Perm_R) != 0);
        if (plan.action == SearchRangeInvalid) {
            failure = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }

        if (plan.action == SearchRangeSkip) {
            mutexLock(&manager.mutex);
            manager.status.scanned += plan.end - cursor;
            mutexUnlock(&manager.mutex);
            cursor = plan.end;
            continue;
        }

        const u64 mappingEnd = plan.end;

        size_t carry = 0;
        while (cursor < mappingEnd && !shouldCancel()) {
            size_t amount = SEARCH_CHUNK_SIZE;
            if ((u64)amount > mappingEnd - cursor)
                amount = (size_t)(mappingEnd - cursor);

            failure = readProcessMemoryQuiet(buffer + carry, cursor, amount);
            if (R_FAILED(failure)) {
                mutexLock(&manager.mutex);
                manager.status.readErrors++;
                manager.status.scanned += amount;
                mutexUnlock(&manager.mutex);
                carry = 0;
                cursor += amount;
                failure = 0;
                continue;
            }

            const size_t combined = carry + amount;
            const u64 bufferAddress = cursor - carry;
            MatchContext matchContext = { .manager = &manager };
            mutexLock(&manager.mutex);
            searchExactBufferAligned(buffer, combined, manager.pattern, manager.patternSize,
                bufferAddress, manager.alignment, recordMatch, &matchContext);
            manager.status.scanned += amount;
            mutexUnlock(&manager.mutex);

            const size_t overlap = manager.patternSize > 1 ? manager.patternSize - 1 : 0;
            carry = combined < overlap ? combined : overlap;
            if (carry > 0)
                memmove(buffer, buffer + combined - carry, carry);
            cursor += amount;
            svcSleepThread(1000000L);
        }
    }

    free(buffer);
    if (shouldCancel())
        finishSearch(SearchStateCancelled, 0);
    else if (R_FAILED(failure))
        finishSearch(SearchStateError, failure);
    else
        finishSearch(SearchStateDone, 0);
}

static void searchWorker(void* unused)
{
    (void)unused;
    while (true) {
        bool shouldRun = false;
        mutexLock(&manager.mutex);
        if (manager.exitRequested) {
            mutexUnlock(&manager.mutex);
            break;
        }
        shouldRun = manager.status.state == SearchStateQueued;
        mutexUnlock(&manager.mutex);

        if (shouldRun)
            runSearch();
        else if (!unknownSearchRunQueued())
            svcSleepThread(10000000L);
    }
}

void searchInitialize(bool storageAvailable)
{
    memset(&manager, 0, sizeof(manager));
    mutexInit(&manager.mutex);
    manager.nextSessionId = 1;
    manager.status.state = SearchStateIdle;
    unknownSearchInitialize(storageAvailable);
    Result rc = threadCreate(&manager.thread, searchWorker, NULL, NULL,
        SEARCH_THREAD_STACK_SIZE, SEARCH_THREAD_PRIORITY, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&manager.thread);
        manager.threadStarted = R_SUCCEEDED(rc);
    }
    else {
        manager.status.state = SearchStateError;
        manager.status.error = rc;
    }
}

void searchShutdown(void)
{
    unknownSearchShutdown();
    mutexLock(&manager.mutex);
    manager.exitRequested = true;
    manager.cancelRequested = true;
    mutexUnlock(&manager.mutex);
    if (manager.threadStarted) {
        threadWaitForExit(&manager.thread);
        threadClose(&manager.thread);
    }
    free(manager.results);
    manager.results = NULL;
    unknownSearchCleanup();
}

static SearchStartResult queueSearch(u64 processId, u64 start, u64 end, const u8* pattern,
    size_t patternSize, SearchType type, SearchRegion region, u64 regionBase, u64 regionOffset,
    u64 alignment, ProcessMemoryBackendKind backend, u64* sessionId)
{
    mutexLock(&manager.mutex);
    if (!manager.threadStarted) {
        mutexUnlock(&manager.mutex);
        return SearchStartUnavailable;
    }
    if (unknownSearchHasSession()) {
        mutexUnlock(&manager.mutex);
        return SearchStartBusy;
    }
    if (manager.status.state == SearchStateQueued || manager.status.state == SearchStateRunning) {
        mutexUnlock(&manager.mutex);
        return SearchStartBusy;
    }

    u64* results = malloc(sizeof(u64) * SEARCH_MAX_RESULTS);
    if (results == NULL) {
        mutexUnlock(&manager.mutex);
        return SearchStartNoMemory;
    }
    free(manager.results);
    manager.results = results;
    memcpy(manager.pattern, pattern, patternSize);
    manager.patternSize = patternSize;
    manager.alignment = alignment;
    manager.processId = processId;
    manager.backend = backend;
    manager.cancelRequested = false;
    memset(&manager.status, 0, sizeof(manager.status));
    manager.status.sessionId = manager.nextSessionId++;
    if (manager.nextSessionId == 0)
        manager.nextSessionId = 1;
    manager.status.state = SearchStateQueued;
    manager.status.start = start;
    manager.status.end = end;
    manager.status.type = type;
    manager.status.region = region;
    manager.status.regionBase = regionBase;
    manager.status.regionOffset = regionOffset;
    manager.status.alignment = alignment;
    manager.status.backend = backend;
    manager.status.kind = SearchKindExact;
    manager.status.operation = SearchOperationExactScan;
    *sessionId = manager.status.sessionId;
    mutexUnlock(&manager.mutex);
    return SearchStartOk;
}

SearchStartResult searchStart(u64 start, u64 end, const char* hexPattern, u64* sessionId)
{
    u8 pattern[SEARCH_MAX_PATTERN_SIZE];
    size_t patternSize = 0;
    if (end <= start)
        return SearchStartInvalidRange;
    if (!parsePattern(hexPattern, pattern, &patternSize) || patternSize > end - start)
        return SearchStartInvalidPattern;

    ProcessMemorySession memorySession;
    if (R_FAILED(processMemoryOpen(&memorySession, false)))
        return SearchStartNoProcess;
    const u64 processId = memorySession.processId;
    const ProcessMemoryBackendKind backend = memorySession.backend;
    processMemoryClose(&memorySession);
    return queueSearch(processId, start, end, pattern, patternSize, SearchTypeBytes,
        SearchRegionAbsolute, 0, start, 1, backend, sessionId);
}

static bool parseSearchType(const char* text, SearchType* type, size_t* width)
{
    if (!strcmp(text, "bytes")) { *type = SearchTypeBytes; *width = 0; }
    else if (!strcmp(text, "u8")) { *type = SearchTypeU8; *width = 1; }
    else if (!strcmp(text, "u16")) { *type = SearchTypeU16; *width = 2; }
    else if (!strcmp(text, "u32")) { *type = SearchTypeU32; *width = 4; }
    else if (!strcmp(text, "u64")) { *type = SearchTypeU64; *width = 8; }
    else return false;
    return true;
}

static bool parseSearchRegion(const char* text, SearchRegion* region)
{
    if (!strcmp(text, "absolute")) *region = SearchRegionAbsolute;
    else if (!strcmp(text, "heap")) *region = SearchRegionHeap;
    else if (!strcmp(text, "main")) *region = SearchRegionMain;
    else return false;
    return true;
}

SearchStartResult searchStartRegion(const char* typeText, const char* regionText, u64 offset,
    u64 size, const char* value, u64 alignment, u64* sessionId)
{
    SearchType type;
    SearchRegion region;
    size_t width = 0;
    if (!parseSearchType(typeText, &type, &width))
        return SearchStartInvalidType;
    if (!parseSearchRegion(regionText, &region))
        return SearchStartInvalidRegion;

    u8 pattern[SEARCH_MAX_PATTERN_SIZE];
    size_t patternSize = width;
    if (type == SearchTypeBytes) {
        if (!parsePattern(value, pattern, &patternSize))
            return SearchStartInvalidPattern;
    }
    else if (!searchEncodeUnsigned(value, width, pattern)) {
        return SearchStartInvalidPattern;
    }

    if (alignment == 0)
        alignment = type == SearchTypeBytes ? 1 : width;
    if (!searchAlignmentIsValid(alignment))
        return SearchStartInvalidAlignment;
    if (size < patternSize)
        return SearchStartInvalidRange;

    ProcessMemorySession memorySession;
    if (R_FAILED(processMemoryOpen(&memorySession, false)))
        return SearchStartNoProcess;
    const ProcessMemoryMetadata* metadata = processMemoryGetMetadata(&memorySession);
    const u64 processId = metadata->processId;
    const ProcessMemoryBackendKind backend = memorySession.backend;

    u64 base = 0;
    if (region == SearchRegionMain) {
        base = metadata->mainBase;
        if (base == 0)
            goto baseUnavailable;
    }
    else if (region == SearchRegionHeap) {
        base = metadata->heapBase;
        if (base == 0)
            goto baseUnavailable;
    }

    if (offset > UINT64_MAX - base) {
        processMemoryClose(&memorySession);
        return SearchStartInvalidRange;
    }
    const u64 start = base + offset;
    if (size > UINT64_MAX - start) {
        processMemoryClose(&memorySession);
        return SearchStartInvalidRange;
    }
    processMemoryClose(&memorySession);
    return queueSearch(processId, start, start + size, pattern, patternSize, type, region,
        base, offset, alignment, backend, sessionId);

baseUnavailable:
    processMemoryClose(&memorySession);
    return SearchStartBaseUnavailable;
}

bool searchGetStatus(u64 sessionId, SearchStatus* status)
{
    mutexLock(&manager.mutex);
    const bool found = sessionId != 0 && manager.status.sessionId == sessionId;
    if (found)
        *status = manager.status;
    mutexUnlock(&manager.mutex);
    return found || unknownSearchGetStatus(sessionId, status);
}

SearchResultsResult searchCopyResults(u64 sessionId, u64 offset, u64 count, u64* addresses,
    u64* copied, u64* totalStored)
{
    if ((sessionId & 0x8000000000000000ULL) != 0)
        return unknownSearchCopyResults(sessionId, offset, count, addresses, copied, totalStored);
    if (count > SEARCH_MAX_PAGE_RESULTS)
        count = SEARCH_MAX_PAGE_RESULTS;
    mutexLock(&manager.mutex);
    const bool found = sessionId != 0 && manager.status.sessionId == sessionId;
    if (found) {
        *totalStored = manager.status.storedMatches;
        if (offset >= manager.status.storedMatches)
            count = 0;
        else if (count > manager.status.storedMatches - offset)
            count = manager.status.storedMatches - offset;
        if (count > 0)
            memcpy(addresses, manager.results + offset, sizeof(u64) * count);
        *copied = count;
    }
    mutexUnlock(&manager.mutex);
    return found ? SearchResultsOk : SearchResultsNotFound;
}

bool searchCancel(u64 sessionId)
{
    if ((sessionId & 0x8000000000000000ULL) != 0)
        return unknownSearchCancel(sessionId);
    mutexLock(&manager.mutex);
    const bool found = sessionId != 0 && manager.status.sessionId == sessionId;
    if (found && (manager.status.state == SearchStateQueued || manager.status.state == SearchStateRunning))
        manager.cancelRequested = true;
    mutexUnlock(&manager.mutex);
    return found;
}

bool searchClose(u64 sessionId)
{
    if ((sessionId & 0x8000000000000000ULL) != 0)
        return unknownSearchClose(sessionId);
    mutexLock(&manager.mutex);
    const bool found = sessionId != 0 && manager.status.sessionId == sessionId;
    bool closed = false;
    if (found && manager.status.state != SearchStateQueued && manager.status.state != SearchStateRunning) {
        free(manager.results);
        manager.results = NULL;
        memset(&manager.status, 0, sizeof(manager.status));
        manager.status.state = SearchStateIdle;
        closed = true;
    }
    mutexUnlock(&manager.mutex);
    return closed;
}

bool searchIsActive(void)
{
    mutexLock(&manager.mutex);
    const bool active = manager.status.state == SearchStateQueued
        || manager.status.state == SearchStateRunning;
    mutexUnlock(&manager.mutex);
    return active || unknownSearchIsActive();
}

bool searchLocksBackend(void)
{
    return searchIsActive() || unknownSearchHasSession();
}

bool searchMemoryAccessBlocked(void)
{
    return unknownSearchBlocksMemoryAccess();
}

SearchStartResult searchBeginUnknown(const char* type, const char* region, u64 offset, u64 size,
    u64 alignment, bool pause, u64* sessionId)
{
    if (searchIsActive())
        return SearchStartBusy;
    return unknownSearchBegin(type, region, offset, size, alignment, pause, sessionId);
}

SearchStartResult searchRefine(u64 sessionId, SearchOperation operation, const char* value,
    bool pause)
{
    return unknownSearchRefine(sessionId, operation, value, pause);
}

const char* searchStateName(SearchState state)
{
    switch (state) {
    case SearchStateIdle: return "idle";
    case SearchStateQueued: return "queued";
    case SearchStateRunning: return "running";
    case SearchStateDone: return "done";
    case SearchStateCancelled: return "cancelled";
    case SearchStateError: return "error";
    default: return "unknown";
    }
}

const char* searchTypeName(SearchType type)
{
    switch (type) {
    case SearchTypeBytes: return "bytes";
    case SearchTypeU8: return "u8";
    case SearchTypeU16: return "u16";
    case SearchTypeU32: return "u32";
    case SearchTypeU64: return "u64";
    default: return "unknown";
    }
}

const char* searchRegionName(SearchRegion region)
{
    switch (region) {
    case SearchRegionAbsolute: return "absolute";
    case SearchRegionHeap: return "heap";
    case SearchRegionMain: return "main";
    case SearchRegionAlias: return "alias";
    case SearchRegionAddressSpace: return "addressSpace";
    default: return "unknown";
    }
}

const char* searchKindName(SearchKind kind)
{
    return kind == SearchKindUnknown ? "unknown" : "exact";
}

const char* searchOperationName(SearchOperation operation)
{
    switch (operation) {
    case SearchOperationExactScan: return "scan";
    case SearchOperationBegin: return "begin";
    case SearchOperationRefineExact: return "exact";
    case SearchOperationRefineChanged: return "changed";
    case SearchOperationRefineUnchanged: return "unchanged";
    case SearchOperationRefineIncreased: return "increased";
    case SearchOperationRefineDecreased: return "decreased";
    default: return "unknown";
    }
}

const char* searchFailureName(SearchFailure failure)
{
    switch (failure) {
    case SearchFailureNone: return "NONE";
    case SearchFailureSdUnavailable: return "SD_UNAVAILABLE";
    case SearchFailureInsufficientStorage: return "INSUFFICIENT_STORAGE";
    case SearchFailureCorruptSession: return "CORRUPT_SESSION";
    case SearchFailureProcessChanged: return "PROCESS_CHANGED";
    case SearchFailurePauseFailed: return "PAUSE_FAILED";
    case SearchFailureIoError: return "IO_ERROR";
    default: return "UNKNOWN";
    }
}
