#include <errno.h>
#include <stdlib.h>
#include "search_value.h"

bool searchEncodeUnsigned(const char* text, size_t width, uint8_t* output)
{
    if (text == NULL || output == NULL || text[0] == 0 || text[0] == '-'
        || (width != 1 && width != 2 && width != 4 && width != 8))
        return false;

    int base = 10;
    const char* digits = text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        digits += 2;
        if (digits[0] == 0)
            return false;
    }

    char* end = NULL;
    errno = 0;
    unsigned long long value = strtoull(digits, &end, base);
    if (errno != 0 || end == digits || *end != 0)
        return false;
    if (width < 8 && value >= (1ULL << (width * 8)))
        return false;

    for (size_t index = 0; index < width; index++)
        output[index] = (uint8_t)(value >> (index * 8));
    return true;
}

bool searchAlignmentIsValid(uint64_t alignment)
{
    return alignment != 0 && alignment <= 256 && (alignment & (alignment - 1)) == 0;
}
