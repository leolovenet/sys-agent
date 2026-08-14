#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "search_value.h"

static void testIntegerEncodingIsLittleEndian(void)
{
    uint8_t output[8] = { 0 };
    const uint8_t expected32[] = { 0x78, 0x56, 0x34, 0x12 };
    assert(searchEncodeUnsigned("0x12345678", 4, output));
    assert(memcmp(output, expected32, sizeof(expected32)) == 0);
    assert(searchEncodeUnsigned("18446744073709551615", 8, output));
    for (size_t index = 0; index < 8; index++)
        assert(output[index] == 0xFF);
    assert(searchEncodeUnsigned("08", 1, output));
    assert(output[0] == 8);
}

static void testIntegerValidation(void)
{
    uint8_t output[8];
    assert(!searchEncodeUnsigned("256", 1, output));
    assert(!searchEncodeUnsigned("-1", 8, output));
    assert(!searchEncodeUnsigned("12x", 4, output));
    assert(!searchEncodeUnsigned("0x", 4, output));
    assert(!searchEncodeUnsigned("1", 3, output));
}

static void testAlignmentValidation(void)
{
    assert(searchAlignmentIsValid(1));
    assert(searchAlignmentIsValid(8));
    assert(searchAlignmentIsValid(256));
    assert(!searchAlignmentIsValid(0));
    assert(!searchAlignmentIsValid(3));
    assert(!searchAlignmentIsValid(512));
}

int main(void)
{
    testIntegerEncodingIsLittleEndian();
    testIntegerValidation();
    testAlignmentValidation();
    puts("search_value_test: ok");
    return 0;
}
