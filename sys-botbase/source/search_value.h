#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool searchEncodeUnsigned(const char* text, size_t width, uint8_t* output);
bool searchAlignmentIsValid(uint64_t alignment);
