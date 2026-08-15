#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SearchCompareExact = 0,
    SearchCompareChanged,
    SearchCompareUnchanged,
    SearchCompareIncreased,
    SearchCompareDecreased
} SearchCompareMode;

uint64_t searchDecodeUnsigned(const uint8_t* value, size_t width);
bool searchCompareUnsigned(SearchCompareMode mode, const uint8_t* previous,
    const uint8_t* current, size_t width, uint64_t exactValue);
uint32_t searchFilterUnsigned(SearchCompareMode mode, const uint8_t* previous,
    const uint8_t* current, const uint8_t* inputMask, uint8_t* outputMask,
    uint32_t slotCount, size_t width, uint64_t exactValue);
