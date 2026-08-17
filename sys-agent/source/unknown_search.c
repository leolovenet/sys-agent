#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "process_memory.h"
#include "search_compare.h"
#include "search_range.h"
#include "search_store.h"
#include "search_value.h"
#include "unknown_search.h"

#define UNKNOWN_CHUNK_SIZE 0x40000
#define UNKNOWN_CHUNK_YIELD_NS 20000000L
#define UNKNOWN_STORAGE_DIR "sdmc:/switch/sys-agent/search"
#define UNKNOWN_STORAGE_ROOT "sdmc:/"
#define UNKNOWN_STORAGE_FILE UNKNOWN_STORAGE_DIR "/session.dat"
#define UNKNOWN_STORAGE_TEMP UNKNOWN_STORAGE_DIR "/session.tmp"
#define UNKNOWN_STORAGE_OLD UNKNOWN_STORAGE_DIR "/session.old"
#define UNKNOWN_STORAGE_RESERVE 0x4000000ULL

typedef struct {
    Mutex mutex;
    bool storageAvailable;
    bool exitRequested;
    bool cancelRequested;
    u64 nextSessionId;
    u64 exactValue;
    size_t width;
    ProcessMemoryMetadata metadata;
    SearchStatus status;
} UnknownManager;

static UnknownManager unknownManager;

static bool parseType(const char* text, SearchType* type, size_t* width)
{
    if (!strcmp(text, "u8")) { *type = SearchTypeU8; *width = 1; }
    else if (!strcmp(text, "u16")) { *type = SearchTypeU16; *width = 2; }
    else if (!strcmp(text, "u32")) { *type = SearchTypeU32; *width = 4; }
    else if (!strcmp(text, "u64")) { *type = SearchTypeU64; *width = 8; }
    else return false;
    return true;
}

static bool parseRegion(const char* text, SearchRegion* region)
{
    if (!strcmp(text, "absolute")) *region = SearchRegionAbsolute;
    else if (!strcmp(text, "heap")) *region = SearchRegionHeap;
    else if (!strcmp(text, "main")) *region = SearchRegionMain;
    else if (!strcmp(text, "alias")) *region = SearchRegionAlias;
    else if (!strcmp(text, "addressSpace")) *region = SearchRegionAddressSpace;
    else return false;
    return true;
}

static SearchStartResult resolveRegion(const ProcessMemoryMetadata* metadata, SearchRegion region,
    u64 offset, u64 size, u64* base, u64* start, u64* end)
{
    *base = 0;
    u64 regionSize = UINT64_MAX;
    switch (region) {
    case SearchRegionMain: *base = metadata->mainBase; regionSize = metadata->mainSize; break;
    case SearchRegionHeap: *base = metadata->heapBase; regionSize = metadata->heapSize; break;
    case SearchRegionAlias: *base = metadata->aliasBase; regionSize = metadata->aliasSize; break;
    case SearchRegionAddressSpace:
        *base = metadata->addressSpaceBase; regionSize = metadata->addressSpaceSize; break;
    default: break;
    }
    if (region != SearchRegionAbsolute && (*base == 0 || regionSize == 0))
        return SearchStartBaseUnavailable;
    if (region != SearchRegionAbsolute && (offset > regionSize || size > regionSize - offset))
        return SearchStartInvalidRange;
    if (offset > UINT64_MAX - *base)
        return SearchStartInvalidRange;
    *start = *base + offset;
    if (size > UINT64_MAX - *start)
        return SearchStartInvalidRange;
    *end = *start + size;
    return SearchStartOk;
}

static bool cancelled(void)
{
    mutexLock(&unknownManager.mutex);
    const bool value = unknownManager.cancelRequested || unknownManager.exitRequested;
    mutexUnlock(&unknownManager.mutex);
    return value;
}

static bool metadataMatches(const ProcessMemoryMetadata* metadata)
{
    return metadata->processId == unknownManager.metadata.processId
        && metadata->titleId == unknownManager.metadata.titleId
        && memcmp(metadata->buildId, unknownManager.metadata.buildId,
            sizeof(metadata->buildId)) == 0;
}

static Result openPinned(ProcessMemorySession* session)
{
    Result rc = processMemoryOpenBackend(session, unknownManager.status.backend,
        unknownManager.metadata.processId, false);
    if (R_SUCCEEDED(rc) && !metadataMatches(processMemoryGetMetadata(session))) {
        processMemoryClose(session);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    return rc;
}

static bool processIsCurrent(void)
{
    u64 processId = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&processId))
        && processId == unknownManager.metadata.processId;
}

static Result queryMemory(ProcessMemorySession* pinned, u64 address, MemoryInfo* info)
{
    if (pinned != NULL)
        return processMemoryQuery(pinned, info, address);
    ProcessMemorySession session;
    Result rc = openPinned(&session);
    if (R_SUCCEEDED(rc)) {
        rc = processMemoryQuery(&session, info, address);
        processMemoryClose(&session);
    }
    return rc;
}

static Result readMemory(ProcessMemorySession* pinned, void* output, u64 address, size_t size)
{
    if (pinned != NULL)
        return processMemoryRead(pinned, output, address, size);
    ProcessMemorySession session;
    Result rc = openPinned(&session);
    if (R_SUCCEEDED(rc)) {
        rc = processMemoryRead(&session, output, address, size);
        processMemoryClose(&session);
    }
    return rc;
}

static void updateProgress(u64 scanned, u64 candidates)
{
    mutexLock(&unknownManager.mutex);
    unknownManager.status.scanned += scanned;
    unknownManager.status.totalMatches = candidates;
    unknownManager.status.storedMatches = candidates;
    unknownManager.status.candidates = candidates;
    mutexUnlock(&unknownManager.mutex);
}

static void finish(SearchState state, Result error, SearchFailure failure, bool committed)
{
    struct stat stats;
    mutexLock(&unknownManager.mutex);
    unknownManager.status.state = state;
    unknownManager.status.error = error;
    unknownManager.status.failure = failure;
    unknownManager.status.committed = committed;
    unknownManager.status.resumable = committed
        && failure != SearchFailureProcessChanged
        && failure != SearchFailureCorruptSession;
    if (committed && stat(UNKNOWN_STORAGE_FILE, &stats) == 0)
        unknownManager.status.diskBytes = (u64)stats.st_size;
    mutexUnlock(&unknownManager.mutex);
}

static void restoreCommittedCounts(u64 candidates)
{
    mutexLock(&unknownManager.mutex);
    unknownManager.status.totalMatches = candidates;
    unknownManager.status.storedMatches = candidates;
    unknownManager.status.candidates = candidates;
    mutexUnlock(&unknownManager.mutex);
}

static u64 alignUp(u64 value, u64 alignment)
{
    const u64 mask = alignment - 1;
    if (value > UINT64_MAX - mask)
        return UINT64_MAX;
    return (value + mask) & ~mask;
}

static SearchStoreMetadata makeStoreMetadata(u64 generation)
{
    SearchStoreMetadata metadata = {
        .processId = unknownManager.metadata.processId,
        .titleId = unknownManager.metadata.titleId,
        .start = unknownManager.status.start,
        .end = unknownManager.status.end,
        .regionBase = unknownManager.status.regionBase,
        .regionOffset = unknownManager.status.regionOffset,
        .alignment = unknownManager.status.alignment,
        .generation = generation,
        .type = unknownManager.status.type,
        .region = unknownManager.status.region,
        .backend = unknownManager.status.backend,
        .width = (u32)unknownManager.width,
    };
    memcpy(metadata.buildId, unknownManager.metadata.buildId, sizeof(metadata.buildId));
    return metadata;
}

static bool beginPause(ProcessMemorySession* session, ProcessMemorySession** pinned,
    SearchFailure* failure, Result* error)
{
    *pinned = NULL;
    if (!unknownManager.status.pause)
        return true;
    *error = openPinned(session);
    if (R_FAILED(*error)) {
        *failure = SearchFailureProcessChanged;
        return false;
    }
    *error = processMemoryPause(session);
    if (R_FAILED(*error)) {
        processMemoryClose(session);
        *failure = SearchFailurePauseFailed;
        return false;
    }
    *pinned = session;
    return true;
}

static void endPause(ProcessMemorySession* session, ProcessMemorySession* pinned,
    SearchFailure* failure, Result* error)
{
    if (pinned == NULL)
        return;
    const Result resume = processMemoryResume(session);
    processMemoryClose(session);
    if (R_FAILED(resume)) {
        *failure = SearchFailurePauseFailed;
        *error = resume;
    }
}

static void runBegin(void)
{
    u8* raw = malloc(UNKNOWN_CHUNK_SIZE + 8);
    u8* values = malloc(UNKNOWN_CHUNK_SIZE + 8);
    SearchStoreWriter writer;
    bool writerOpen = false;
    Result error = 0;
    SearchFailure failure = SearchFailureNone;
    ProcessMemorySession pauseSession;
    ProcessMemorySession* pinned = NULL;
    if (raw == NULL || values == NULL) {
        failure = SearchFailureIoError;
        error = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto done;
    }
    const SearchStoreMetadata metadata = makeStoreMetadata(0);
    if (!searchStoreWriterOpen(&writer, UNKNOWN_STORAGE_TEMP, &metadata)) {
        failure = SearchFailureIoError;
        goto done;
    }
    writerOpen = true;
    if (!beginPause(&pauseSession, &pinned, &failure, &error))
        goto done;

    u64 cursor = unknownManager.status.start;
    const u64 end = unknownManager.status.end;
    while (cursor < end && !cancelled()) {
        if (!processIsCurrent()) {
            failure = SearchFailureProcessChanged;
            error = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            break;
        }
        MemoryInfo info = { 0 };
        error = queryMemory(pinned, cursor, &info);
        if (R_FAILED(error)) {
            failure = SearchFailureProcessChanged;
            break;
        }
        const SearchRangePlan plan = searchPlanMapping(cursor, end, info.addr, info.size,
            (info.perm & Perm_R) != 0);
        if (plan.action == SearchRangeInvalid) {
            failure = SearchFailureCorruptSession;
            break;
        }
        if (plan.action == SearchRangeSkip) {
            updateProgress(plan.end - cursor, writer.header.candidateCount);
            cursor = plan.end;
            continue;
        }
        const u64 mappingEnd = plan.end;
        while (cursor < mappingEnd && !cancelled()) {
            u64 maximumSpan = (UNKNOWN_CHUNK_SIZE / unknownManager.width)
                * unknownManager.status.alignment;
            if (maximumSpan > UNKNOWN_CHUNK_SIZE)
                maximumSpan = UNKNOWN_CHUNK_SIZE;
            size_t amount = (size_t)maximumSpan;
            if ((u64)amount > mappingEnd - cursor)
                amount = (size_t)(mappingEnd - cursor);
            size_t readSize = amount;
            if ((u64)readSize + unknownManager.width - 1 <= mappingEnd - cursor)
                readSize += unknownManager.width - 1;
            error = readMemory(pinned, raw, cursor, readSize);
            if (R_FAILED(error)) {
                failure = SearchFailureIoError;
                mutexLock(&unknownManager.mutex);
                unknownManager.status.readErrors++;
                mutexUnlock(&unknownManager.mutex);
                goto done;
            }
            const u64 first = alignUp(cursor, unknownManager.status.alignment);
            u32 slots = 0;
            if (first != UINT64_MAX && first < cursor + amount
                && first + unknownManager.width <= mappingEnd) {
                slots = (u32)(((cursor + amount - first) + unknownManager.status.alignment - 1)
                    / unknownManager.status.alignment);
                while (slots > 0 && first + (u64)(slots - 1) * unknownManager.status.alignment
                    + unknownManager.width > mappingEnd)
                    slots--;
            }
            for (u32 slot = 0; slot < slots; slot++) {
                const u64 address = first + (u64)slot * unknownManager.status.alignment;
                memcpy(values + (size_t)slot * unknownManager.width,
                    raw + (address - cursor), unknownManager.width);
            }
            if (slots > 0 && !searchStoreWriterAddBlock(&writer, first, slots, values, NULL)) {
                failure = SearchFailureIoError;
                goto done;
            }
            updateProgress(amount, writer.header.candidateCount);
            cursor += amount;
            // All sysmodule threads are restricted to CPU 3. Give the TCP and
            // controller threads a meaningful scheduling window between SD
            // snapshot blocks; a 1 ms yield still starved them on hardware.
            svcSleepThread(UNKNOWN_CHUNK_YIELD_NS);
        }
    }

done:
    endPause(&pauseSession, pinned, &failure, &error);
    free(raw);
    free(values);
    if (cancelled()) {
        if (writerOpen) searchStoreWriterAbort(&writer);
        finish(SearchStateCancelled, 0, SearchFailureNone, false);
    }
    else if (failure != SearchFailureNone || R_FAILED(error)) {
        if (writerOpen) searchStoreWriterAbort(&writer);
        finish(SearchStateError, error, failure, false);
    }
    else if (!searchStoreWriterFinish(&writer)
        || !searchStoreCommit(UNKNOWN_STORAGE_TEMP, UNKNOWN_STORAGE_FILE,
            UNKNOWN_STORAGE_OLD, false)) {
        searchStoreWriterAbort(&writer);
        finish(SearchStateError, 0, SearchFailureIoError, false);
    }
    else {
        mutexLock(&unknownManager.mutex);
        unknownManager.status.generation = 0;
        unknownManager.status.candidates = writer.header.candidateCount;
        unknownManager.status.totalMatches = writer.header.candidateCount;
        unknownManager.status.storedMatches = writer.header.candidateCount;
        mutexUnlock(&unknownManager.mutex);
        finish(SearchStateDone, 0, SearchFailureNone, true);
    }
}

static SearchCompareMode compareMode(SearchOperation operation)
{
    switch (operation) {
    case SearchOperationRefineChanged: return SearchCompareChanged;
    case SearchOperationRefineUnchanged: return SearchCompareUnchanged;
    case SearchOperationRefineIncreased: return SearchCompareIncreased;
    case SearchOperationRefineDecreased: return SearchCompareDecreased;
    default: return SearchCompareExact;
    }
}

static void runRefine(void)
{
    const u64 previousCandidates = unknownManager.status.candidates;
    SearchStoreReader reader;
    SearchStoreWriter writer;
    bool readerOpen = false;
    bool writerOpen = false;
    Result error = 0;
    SearchFailure failure = SearchFailureNone;
    ProcessMemorySession pauseSession;
    ProcessMemorySession* pinned = NULL;
    u8* previous = NULL;
    u8* currentRaw = NULL;
    u8* currentValues = NULL;
    u8* oldMask = NULL;
    u8* newMask = NULL;

    if (!searchStoreReaderOpen(&reader, UNKNOWN_STORAGE_FILE)) {
        failure = SearchFailureCorruptSession;
        goto done;
    }
    readerOpen = true;
    SearchStoreMetadata expected = makeStoreMetadata(unknownManager.status.generation);
    if (memcmp(&reader.header.metadata, &expected, sizeof(expected)) != 0) {
        failure = SearchFailureProcessChanged;
        goto done;
    }
    SearchStoreMetadata nextMetadata = expected;
    nextMetadata.generation++;
    if (!searchStoreWriterOpen(&writer, UNKNOWN_STORAGE_TEMP, &nextMetadata)) {
        failure = SearchFailureIoError;
        goto done;
    }
    writerOpen = true;
    previous = malloc(UNKNOWN_CHUNK_SIZE + 8);
    currentRaw = malloc(UNKNOWN_CHUNK_SIZE + 8);
    currentValues = malloc(UNKNOWN_CHUNK_SIZE + 8);
    oldMask = malloc(UNKNOWN_CHUNK_SIZE / 8 + 8);
    newMask = malloc(UNKNOWN_CHUNK_SIZE / 8 + 8);
    if (previous == NULL || currentRaw == NULL || currentValues == NULL
        || oldMask == NULL || newMask == NULL) {
        error = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        failure = SearchFailureIoError;
        goto done;
    }
    if (!beginPause(&pauseSession, &pinned, &failure, &error))
        goto done;

    SearchStoreBlockHeader block;
    while (!cancelled() && reader.nextBlock < reader.header.blockCount) {
        if (!processIsCurrent()) {
            failure = SearchFailureProcessChanged;
            error = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            break;
        }
        if (!searchStoreReaderNext(&reader, &block, previous, UNKNOWN_CHUNK_SIZE + 8,
            oldMask, UNKNOWN_CHUNK_SIZE / 8 + 8)) {
            failure = SearchFailureCorruptSession;
            break;
        }
        const u64 span64 = (u64)(block.slotCount - 1) * expected.alignment + expected.width;
        if (span64 > UNKNOWN_CHUNK_SIZE + 8) {
            failure = SearchFailureCorruptSession;
            break;
        }
        const size_t span = (size_t)span64;
        error = readMemory(pinned, currentRaw, block.base, span);
        if (R_FAILED(error)) {
            failure = SearchFailureIoError;
            mutexLock(&unknownManager.mutex);
            unknownManager.status.readErrors++;
            mutexUnlock(&unknownManager.mutex);
            break;
        }
        for (u32 slot = 0; slot < block.slotCount; slot++) {
            const size_t packed = (size_t)slot * expected.width;
            memcpy(currentValues + packed, currentRaw + (u64)slot * expected.alignment,
                expected.width);
        }
        searchFilterUnsigned(compareMode(unknownManager.status.operation), previous,
            currentValues, oldMask, newMask, block.slotCount, expected.width,
            unknownManager.exactValue);
        if (!searchStoreWriterAddBlock(&writer, block.base, block.slotCount,
            currentValues, newMask)) {
            failure = SearchFailureIoError;
            break;
        }
        updateProgress(span, writer.header.candidateCount);
        svcSleepThread(UNKNOWN_CHUNK_YIELD_NS);
    }

done:
    endPause(&pauseSession, pinned, &failure, &error);
    free(previous); free(currentRaw); free(currentValues); free(oldMask); free(newMask);
    if (readerOpen) searchStoreReaderClose(&reader);
    if (cancelled()) {
        if (writerOpen) searchStoreWriterAbort(&writer);
        restoreCommittedCounts(previousCandidates);
        finish(SearchStateCancelled, 0, SearchFailureNone, true);
    }
    else if (failure != SearchFailureNone || R_FAILED(error)) {
        if (writerOpen) searchStoreWriterAbort(&writer);
        restoreCommittedCounts(previousCandidates);
        finish(SearchStateError, error, failure, true);
    }
    else if (!searchStoreWriterFinish(&writer)
        || !searchStoreCommit(UNKNOWN_STORAGE_TEMP, UNKNOWN_STORAGE_FILE,
            UNKNOWN_STORAGE_OLD, true)) {
        searchStoreWriterAbort(&writer);
        restoreCommittedCounts(previousCandidates);
        finish(SearchStateError, 0, SearchFailureIoError, true);
    }
    else {
        mutexLock(&unknownManager.mutex);
        unknownManager.status.generation++;
        unknownManager.status.candidates = writer.header.candidateCount;
        unknownManager.status.totalMatches = writer.header.candidateCount;
        unknownManager.status.storedMatches = writer.header.candidateCount;
        mutexUnlock(&unknownManager.mutex);
        finish(SearchStateDone, 0, SearchFailureNone, true);
    }
}

void unknownSearchInitialize(bool storageAvailable)
{
    memset(&unknownManager, 0, sizeof(unknownManager));
    mutexInit(&unknownManager.mutex);
    unknownManager.nextSessionId = 0x8000000000000001ULL;
    unknownManager.status.state = SearchStateIdle;
    unknownManager.storageAvailable = storageAvailable
        && searchStorePrepareDirectory(UNKNOWN_STORAGE_DIR)
        && searchStoreClearDirectory(UNKNOWN_STORAGE_DIR);
}

void unknownSearchShutdown(void)
{
    mutexLock(&unknownManager.mutex);
    unknownManager.exitRequested = true;
    unknownManager.cancelRequested = true;
    mutexUnlock(&unknownManager.mutex);
}

void unknownSearchCleanup(void)
{
    remove(UNKNOWN_STORAGE_TEMP);
    remove(UNKNOWN_STORAGE_OLD);
    remove(UNKNOWN_STORAGE_FILE);
}

bool unknownSearchRunQueued(void)
{
    mutexLock(&unknownManager.mutex);
    const bool queued = unknownManager.status.state == SearchStateQueued;
    const SearchOperation operation = unknownManager.status.operation;
    if (queued)
        unknownManager.status.state = SearchStateRunning;
    mutexUnlock(&unknownManager.mutex);
    if (!queued)
        return false;
    if (operation == SearchOperationBegin)
        runBegin();
    else
        runRefine();
    return true;
}

SearchStartResult unknownSearchBegin(const char* typeText, const char* regionText, u64 offset,
    u64 size, u64 alignment, bool pause, u64* sessionId)
{
    SearchType type;
    SearchRegion region;
    size_t width;
    if (!parseType(typeText, &type, &width))
        return SearchStartInvalidType;
    if (!parseRegion(regionText, &region))
        return SearchStartInvalidRegion;
    if (alignment == 0)
        alignment = width;
    if (!searchAlignmentIsValid(alignment))
        return SearchStartInvalidAlignment;
    if (size < width)
        return SearchStartInvalidRange;
    mutexLock(&unknownManager.mutex);
    const bool available = unknownManager.storageAvailable;
    const bool busy = unknownManager.status.sessionId != 0;
    mutexUnlock(&unknownManager.mutex);
    if (!available)
        return SearchStartSdUnavailable;
    if (busy)
        return SearchStartBusy;
    ProcessMemorySession session;
    if (R_FAILED(processMemoryOpen(&session, false)))
        return SearchStartNoProcess;
    const ProcessMemoryMetadata metadata = *processMemoryGetMetadata(&session);
    const ProcessMemoryBackendKind backend = session.backend;
    processMemoryClose(&session);
    u64 base, start, end;
    const SearchStartResult resolved = resolveRegion(&metadata, region, offset, size,
        &base, &start, &end);
    if (resolved != SearchStartOk)
        return resolved;
    const u64 estimate = searchStoreEstimateMaximum(size, alignment, (u32)width);
    if (estimate == UINT64_MAX
        || !searchStoreHasFreeSpace(UNKNOWN_STORAGE_ROOT, estimate,
            UNKNOWN_STORAGE_RESERVE, NULL))
        return SearchStartInsufficientStorage;

    mutexLock(&unknownManager.mutex);
    if (unknownManager.status.sessionId != 0) {
        mutexUnlock(&unknownManager.mutex);
        return SearchStartBusy;
    }
    unknownManager.metadata = metadata;
    unknownManager.width = width;
    unknownManager.cancelRequested = false;
    memset(&unknownManager.status, 0, sizeof(unknownManager.status));
    unknownManager.status.sessionId = unknownManager.nextSessionId++;
    unknownManager.status.state = SearchStateQueued;
    unknownManager.status.start = start;
    unknownManager.status.end = end;
    unknownManager.status.type = type;
    unknownManager.status.region = region;
    unknownManager.status.regionBase = base;
    unknownManager.status.regionOffset = offset;
    unknownManager.status.alignment = alignment;
    unknownManager.status.backend = backend;
    unknownManager.status.kind = SearchKindUnknown;
    unknownManager.status.operation = SearchOperationBegin;
    unknownManager.status.pause = pause;
    *sessionId = unknownManager.status.sessionId;
    mutexUnlock(&unknownManager.mutex);
    return SearchStartOk;
}

SearchStartResult unknownSearchRefine(u64 sessionId, SearchOperation operation,
    const char* value, bool pause)
{
    if (operation < SearchOperationRefineExact || operation > SearchOperationRefineDecreased)
        return SearchStartInvalidType;
    mutexLock(&unknownManager.mutex);
    if (unknownManager.status.sessionId != sessionId || !unknownManager.status.committed
        || !unknownManager.status.resumable) {
        mutexUnlock(&unknownManager.mutex);
        return SearchStartSessionNotReady;
    }
    if (unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning) {
        mutexUnlock(&unknownManager.mutex);
        return SearchStartBusy;
    }
    const size_t width = unknownManager.width;
    mutexUnlock(&unknownManager.mutex);
    u8 encoded[8];
    u64 exactValue = 0;
    if (operation == SearchOperationRefineExact) {
        if (value == NULL || !searchEncodeUnsigned(value, width, encoded))
            return SearchStartInvalidPattern;
        exactValue = searchDecodeUnsigned(encoded, width);
    }
    struct stat stats;
    if (stat(UNKNOWN_STORAGE_FILE, &stats) != 0)
        return SearchStartCorruptSession;
    const u64 estimate = (u64)stats.st_size + ((u64)stats.st_size >> 3) + 0x100000;
    if (!searchStoreHasFreeSpace(UNKNOWN_STORAGE_ROOT, estimate,
        UNKNOWN_STORAGE_RESERVE, NULL))
        return SearchStartInsufficientStorage;
    mutexLock(&unknownManager.mutex);
    if (unknownManager.status.sessionId != sessionId
        || unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning) {
        mutexUnlock(&unknownManager.mutex);
        return SearchStartBusy;
    }
    unknownManager.exactValue = exactValue;
    unknownManager.status.operation = operation;
    unknownManager.status.pause = pause;
    unknownManager.status.state = SearchStateQueued;
    unknownManager.status.failure = SearchFailureNone;
    unknownManager.status.scanned = 0;
    unknownManager.status.totalMatches = 0;
    unknownManager.status.storedMatches = 0;
    unknownManager.cancelRequested = false;
    mutexUnlock(&unknownManager.mutex);
    return SearchStartOk;
}

bool unknownSearchGetStatus(u64 sessionId, SearchStatus* status)
{
    mutexLock(&unknownManager.mutex);
    const bool found = sessionId != 0 && unknownManager.status.sessionId == sessionId;
    if (found)
        *status = unknownManager.status;
    mutexUnlock(&unknownManager.mutex);
    return found;
}

SearchResultsResult unknownSearchCopyResults(u64 sessionId, u64 offset, u64 count,
    u64* addresses, u64* copied, u64* total)
{
    mutexLock(&unknownManager.mutex);
    if (unknownManager.status.sessionId != sessionId) {
        mutexUnlock(&unknownManager.mutex);
        return SearchResultsNotFound;
    }
    if (unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning) {
        mutexUnlock(&unknownManager.mutex);
        return SearchResultsBusy;
    }
    const bool committed = unknownManager.status.committed;
    mutexUnlock(&unknownManager.mutex);
    if (!committed)
        return SearchResultsUnavailable;
    if (count > SEARCH_MAX_PAGE_RESULTS)
        count = SEARCH_MAX_PAGE_RESULTS;
    return searchStoreReadAddresses(UNKNOWN_STORAGE_FILE, offset, count, addresses, copied, total)
        ? SearchResultsOk : SearchResultsCorrupt;
}

bool unknownSearchCancel(u64 sessionId)
{
    mutexLock(&unknownManager.mutex);
    const bool found = sessionId != 0 && unknownManager.status.sessionId == sessionId;
    if (found && (unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning))
        unknownManager.cancelRequested = true;
    mutexUnlock(&unknownManager.mutex);
    return found;
}

bool unknownSearchClose(u64 sessionId)
{
    mutexLock(&unknownManager.mutex);
    const bool found = sessionId != 0 && unknownManager.status.sessionId == sessionId;
    if (!found || unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning) {
        mutexUnlock(&unknownManager.mutex);
        return false;
    }
    remove(UNKNOWN_STORAGE_TEMP);
    remove(UNKNOWN_STORAGE_OLD);
    remove(UNKNOWN_STORAGE_FILE);
    memset(&unknownManager.status, 0, sizeof(unknownManager.status));
    unknownManager.status.state = SearchStateIdle;
    mutexUnlock(&unknownManager.mutex);
    return true;
}

bool unknownSearchIsActive(void)
{
    mutexLock(&unknownManager.mutex);
    const bool active = unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning;
    mutexUnlock(&unknownManager.mutex);
    return active;
}

bool unknownSearchHasSession(void)
{
    mutexLock(&unknownManager.mutex);
    const bool present = unknownManager.status.sessionId != 0;
    mutexUnlock(&unknownManager.mutex);
    return present;
}

bool unknownSearchBlocksMemoryAccess(void)
{
    mutexLock(&unknownManager.mutex);
    const bool blocked = unknownManager.status.state == SearchStateQueued
        || unknownManager.status.state == SearchStateRunning;
    mutexUnlock(&unknownManager.mutex);
    return blocked;
}
