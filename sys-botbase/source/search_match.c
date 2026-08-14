#include <string.h>
#include "search_match.h"

size_t searchExactBuffer(
    const uint8_t* buffer,
    size_t bufferSize,
    const uint8_t* pattern,
    size_t patternSize,
    uint64_t bufferAddress,
    SearchMatchCallback callback,
    void* context)
{
    return searchExactBufferAligned(buffer, bufferSize, pattern, patternSize,
        bufferAddress, 1, callback, context);
}

size_t searchExactBufferAligned(
    const uint8_t* buffer,
    size_t bufferSize,
    const uint8_t* pattern,
    size_t patternSize,
    uint64_t bufferAddress,
    size_t alignment,
    SearchMatchCallback callback,
    void* context)
{
    if (buffer == NULL || pattern == NULL || patternSize == 0 || bufferSize < patternSize
        || alignment == 0)
        return 0;

    size_t count = 0;
    const size_t last = bufferSize - patternSize;
    for (size_t offset = 0; offset <= last; offset++) {
        if ((bufferAddress + offset) % alignment == 0
            && memcmp(buffer + offset, pattern, patternSize) == 0) {
            if (callback != NULL)
                callback(bufferAddress + offset, context);
            count++;
        }
    }
    return count;
}
