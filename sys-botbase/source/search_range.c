#include "search_range.h"

SearchRangePlan searchPlanMapping(
    uint64_t cursor,
    uint64_t searchEnd,
    uint64_t mappingAddress,
    uint64_t mappingSize,
    bool readable)
{
    SearchRangePlan invalid = { .action = SearchRangeInvalid, .start = cursor, .end = cursor };
    if (cursor >= searchEnd || mappingSize == 0 || mappingAddress + mappingSize <= mappingAddress)
        return invalid;

    if (mappingAddress > cursor) {
        SearchRangePlan gap = {
            .action = SearchRangeSkip,
            .start = cursor,
            .end = mappingAddress < searchEnd ? mappingAddress : searchEnd,
        };
        return gap.end > gap.start ? gap : invalid;
    }

    uint64_t mappingEnd = mappingAddress + mappingSize;
    if (mappingEnd > searchEnd)
        mappingEnd = searchEnd;
    if (mappingEnd <= cursor)
        return invalid;

    SearchRangePlan plan = {
        .action = readable ? SearchRangeRead : SearchRangeSkip,
        .start = cursor,
        .end = mappingEnd,
    };
    return plan;
}
