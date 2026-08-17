#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SearchRangeInvalid = 0,
    SearchRangeSkip,
    SearchRangeRead
} SearchRangeAction;

typedef struct {
    SearchRangeAction action;
    uint64_t start;
    uint64_t end;
} SearchRangePlan;

SearchRangePlan searchPlanMapping(
    uint64_t cursor,
    uint64_t searchEnd,
    uint64_t mappingAddress,
    uint64_t mappingSize,
    bool readable);
