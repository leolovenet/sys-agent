#pragma once

#include <switch.h>

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
    SearchRegionMain
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
    SearchStartUnavailable
} SearchStartResult;

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
    bool truncated;
    Result error;
} SearchStatus;

void searchInitialize(void);
void searchShutdown(void);
SearchStartResult searchStart(u64 start, u64 end, const char* hexPattern, u64* sessionId);
SearchStartResult searchStartRegion(const char* type, const char* region, u64 offset, u64 size,
    const char* value, u64 alignment, u64* sessionId);
bool searchGetStatus(u64 sessionId, SearchStatus* status);
bool searchCopyResults(u64 sessionId, u64 offset, u64 count, u64* addresses, u64* copied, u64* totalStored);
bool searchCancel(u64 sessionId);
bool searchClose(u64 sessionId);
const char* searchStateName(SearchState state);
const char* searchTypeName(SearchType type);
const char* searchRegionName(SearchRegion region);
