#include "search_compare.h"
#include <string.h>

uint64_t searchDecodeUnsigned(const uint8_t* value, size_t width)
{
    uint64_t result = 0;
    for (size_t index = 0; index < width; index++)
        result |= (uint64_t)value[index] << (index * 8);
    return result;
}

uint32_t searchFilterUnsigned(SearchCompareMode mode, const uint8_t* previous,
    const uint8_t* current, const uint8_t* inputMask, uint8_t* outputMask,
    uint32_t slotCount, size_t width, uint64_t exactValue)
{
    const size_t maskSize = (slotCount + 7u) / 8u;
    memset(outputMask, 0, maskSize);
    uint32_t matches = 0;
    for (uint32_t slot = 0; slot < slotCount; slot++) {
        if ((inputMask[slot >> 3] & (uint8_t)(1u << (slot & 7))) == 0)
            continue;
        const size_t offset = (size_t)slot * width;
        if (!searchCompareUnsigned(mode, previous + offset, current + offset,
            width, exactValue))
            continue;
        outputMask[slot >> 3] |= (uint8_t)(1u << (slot & 7));
        matches++;
    }
    return matches;
}

bool searchCompareUnsigned(SearchCompareMode mode, const uint8_t* previous,
    const uint8_t* current, size_t width, uint64_t exactValue)
{
    const uint64_t oldValue = searchDecodeUnsigned(previous, width);
    const uint64_t newValue = searchDecodeUnsigned(current, width);
    switch (mode) {
    case SearchCompareExact: return newValue == exactValue;
    case SearchCompareChanged: return newValue != oldValue;
    case SearchCompareUnchanged: return newValue == oldValue;
    case SearchCompareIncreased: return newValue > oldValue;
    case SearchCompareDecreased: return newValue < oldValue;
    default: return false;
    }
}
