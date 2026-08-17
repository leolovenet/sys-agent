#pragma once

#include "search.h"

void unknownSearchInitialize(bool storageAvailable);
void unknownSearchShutdown(void);
void unknownSearchCleanup(void);
bool unknownSearchRunQueued(void);
SearchStartResult unknownSearchBegin(const char* type, const char* region, u64 offset, u64 size,
    u64 alignment, bool pause, u64* sessionId);
SearchStartResult unknownSearchRefine(u64 sessionId, SearchOperation operation,
    const char* value, bool pause);
bool unknownSearchGetStatus(u64 sessionId, SearchStatus* status);
SearchResultsResult unknownSearchCopyResults(u64 sessionId, u64 offset, u64 count,
    u64* addresses, u64* copied, u64* total);
bool unknownSearchCancel(u64 sessionId);
bool unknownSearchClose(u64 sessionId);
bool unknownSearchIsActive(void);
bool unknownSearchHasSession(void);
bool unknownSearchBlocksMemoryAccess(void);
