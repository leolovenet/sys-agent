#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "search_range.h"

static void testReadableMappingIsClippedToSearchEnd(void)
{
    SearchRangePlan plan = searchPlanMapping(0x1100, 0x1800, 0x1000, 0x1000, true);
    assert(plan.action == SearchRangeRead);
    assert(plan.start == 0x1100);
    assert(plan.end == 0x1800);
}

static void testUnreadableMappingIsSkipped(void)
{
    SearchRangePlan plan = searchPlanMapping(0x2000, 0x4000, 0x2000, 0x1000, false);
    assert(plan.action == SearchRangeSkip);
    assert(plan.start == 0x2000);
    assert(plan.end == 0x3000);
}

static void testGapBeforeMappingIsSkippedSeparately(void)
{
    SearchRangePlan plan = searchPlanMapping(0x4000, 0x8000, 0x5000, 0x1000, true);
    assert(plan.action == SearchRangeSkip);
    assert(plan.start == 0x4000);
    assert(plan.end == 0x5000);
}

static void testInvalidAndOverflowMappings(void)
{
    assert(searchPlanMapping(0x1000, 0x2000, 0x1000, 0, true).action == SearchRangeInvalid);
    assert(searchPlanMapping(0x1000, 0x2000, UINT64_MAX - 1, 4, true).action == SearchRangeInvalid);
    assert(searchPlanMapping(0x2000, 0x2000, 0x2000, 0x1000, true).action == SearchRangeInvalid);
    assert(searchPlanMapping(0x3000, 0x4000, 0x1000, 0x1000, true).action == SearchRangeInvalid);
}

int main(void)
{
    testReadableMappingIsClippedToSearchEnd();
    testUnreadableMappingIsSkipped();
    testGapBeforeMappingIsSkippedSeparately();
    testInvalidAndOverflowMappings();
    puts("search_range_test: ok");
    return 0;
}
