#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*SearchMatchCallback)(uint64_t address, void* context);

size_t searchExactBuffer(
    const uint8_t* buffer,
    size_t bufferSize,
    const uint8_t* pattern,
    size_t patternSize,
    uint64_t bufferAddress,
    SearchMatchCallback callback,
    void* context);

size_t searchExactBufferAligned(
    const uint8_t* buffer,
    size_t bufferSize,
    const uint8_t* pattern,
    size_t patternSize,
    uint64_t bufferAddress,
    size_t alignment,
    SearchMatchCallback callback,
    void* context);
