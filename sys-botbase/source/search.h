#pragma once

#include <switch.h>
#include "process_memory.h"

#define SEARCH_MAX_PATTERN_SIZE 256
#define SEARCH_MAX_RESULTS 65536
#define SEARCH_MAX_PAGE_RESULTS 256

typedef enum {
    SearchStateIdle = 0,
    SearchStateQueued,
    SearchStateRunning,
    SearchStateDone,
    SearchStateCancelled,
    SearchStateError
} SearchState;

typedef enum {
    SearchTypeBytes = 0,
    SearchTypeU8,
    SearchTypeU16,
    SearchTypeU32,
    SearchTypeU64
} SearchType;

typedef enum {
    SearchRegionAbsolute = 0,
    SearchRegionHeap,
    SearchRegionMain,
    SearchRegionAlias,
    SearchRegionAddressSpace
} SearchRegion;

typedef enum {
    SearchStartOk = 0,
    SearchStartBusy,
    SearchStartInvalidRange,
    SearchStartInvalidPattern,
    SearchStartInvalidType,
    SearchStartInvalidRegion,
    SearchStartInvalidAlignment,
    SearchStartBaseUnavailable,
    SearchStartNoMemory,
    SearchStartNoProcess,
    SearchStartUnavailable,
    SearchStartSdUnavailable,
    SearchStartInsufficientStorage,
    SearchStartCorruptSession,
    SearchStartProcessChanged,
    SearchStartPauseFailed,
    SearchStartIoError,
    SearchStartSessionNotReady
} SearchStartResult;

typedef enum { SearchKindExact = 0, SearchKindUnknown } SearchKind;

typedef enum {
    SearchOperationExactScan = 0,
    SearchOperationBegin,
    SearchOperationRefineExact,
    SearchOperationRefineChanged,
    SearchOperationRefineUnchanged,
    SearchOperationRefineIncreased,
    SearchOperationRefineDecreased
} SearchOperation;

typedef enum {
    SearchFailureNone = 0,
    SearchFailureSdUnavailable,
    SearchFailureInsufficientStorage,
    SearchFailureCorruptSession,
    SearchFailureProcessChanged,
    SearchFailurePauseFailed,
    SearchFailureIoError
} SearchFailure;

typedef enum {
    SearchResultsOk = 0,
    SearchResultsNotFound,
    SearchResultsBusy,
    SearchResultsUnavailable,
    SearchResultsCorrupt
} SearchResultsResult;

typedef struct {
    u64 sessionId;
    SearchState state;
    u64 start;
    u64 end;
    u64 scanned;
    u64 totalMatches;
    u64 storedMatches;
    u64 readErrors;
    u64 regionBase;
    u64 regionOffset;
    u64 alignment;
    SearchType type;
    SearchRegion region;
    ProcessMemoryBackendKind backend;
    SearchKind kind;
    SearchOperation operation;
    SearchFailure failure;
    u64 generation;
    u64 candidates;
    u64 diskBytes;
    bool pause;
    bool committed;
    bool resumable;
    bool truncated;
    Result error;
} SearchStatus;

void searchInitialize(bool storageAvailable);
void searchShutdown(void);
SearchStartResult searchStart(u64 start, u64 end, const char* hexPattern, u64* sessionId);
SearchStartResult searchStartRegion(const char* type, const char* region, u64 offset, u64 size,
    const char* value, u64 alignment, u64* sessionId);
SearchStartResult searchBeginUnknown(const char* type, const char* region, u64 offset, u64 size,
    u64 alignment, bool pause, u64* sessionId);
SearchStartResult searchRefine(u64 sessionId, SearchOperation operation, const char* value,
    bool pause);
bool searchGetStatus(u64 sessionId, SearchStatus* status);
SearchResultsResult searchCopyResults(u64 sessionId, u64 offset, u64 count, u64* addresses,
    u64* copied, u64* totalStored);
bool searchCancel(u64 sessionId);
bool searchClose(u64 sessionId);
bool searchIsActive(void);
bool searchLocksBackend(void);
bool searchMemoryAccessBlocked(void);
const char* searchStateName(SearchState state);
const char* searchTypeName(SearchType type);
const char* searchRegionName(SearchRegion region);
const char* searchKindName(SearchKind kind);
const char* searchOperationName(SearchOperation operation);
const char* searchFailureName(SearchFailure failure);
