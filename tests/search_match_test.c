#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "search_match.h"

typedef struct {
    uint64_t values[16];
    size_t count;
} Matches;

static void collect(uint64_t address, void* context)
{
    Matches* matches = context;
    matches->values[matches->count++] = address;
}

static void testExactAndOverlappingMatches(void)
{
    const uint8_t buffer[] = { 0xAA, 0xAA, 0xAA };
    const uint8_t pattern[] = { 0xAA, 0xAA };
    Matches matches = { 0 };
    assert(searchExactBuffer(buffer, sizeof(buffer), pattern, sizeof(pattern), 0x1000, collect, &matches) == 2);
    assert(matches.count == 2);
    assert(matches.values[0] == 0x1000);
    assert(matches.values[1] == 0x1001);
}

static void testNoMatchAndInvalidInputs(void)
{
    const uint8_t buffer[] = { 1, 2, 3 };
    const uint8_t pattern[] = { 4, 5 };
    assert(searchExactBuffer(buffer, sizeof(buffer), pattern, sizeof(pattern), 0, NULL, NULL) == 0);
    assert(searchExactBuffer(buffer, 1, pattern, sizeof(pattern), 0, NULL, NULL) == 0);
    assert(searchExactBuffer(buffer, sizeof(buffer), pattern, 0, 0, NULL, NULL) == 0);
}

static void testChunkOverlapConvention(void)
{
    const uint8_t first[] = { 0x10, 0x20, 0x30 };
    const uint8_t secondWithCarry[] = { 0x20, 0x30, 0x40, 0x50 };
    const uint8_t pattern[] = { 0x20, 0x30, 0x40 };
    assert(searchExactBuffer(first, sizeof(first), pattern, sizeof(pattern), 0x2000, NULL, NULL) == 0);
    Matches matches = { 0 };
    assert(searchExactBuffer(secondWithCarry, sizeof(secondWithCarry), pattern, sizeof(pattern),
        0x2001, collect, &matches) == 1);
    assert(matches.values[0] == 0x2001);
}

static void testAbsoluteAddressAlignment(void)
{
    const uint8_t buffer[] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    const uint8_t pattern[] = { 0xAA };
    Matches matches = { 0 };
    assert(searchExactBufferAligned(buffer, sizeof(buffer), pattern, sizeof(pattern),
        0x1001, 4, collect, &matches) == 1);
    assert(matches.count == 1);
    assert(matches.values[0] == 0x1004);
    assert(searchExactBufferAligned(buffer, sizeof(buffer), pattern, sizeof(pattern),
        0x1000, 0, NULL, NULL) == 0);
}

int main(void)
{
    testExactAndOverlappingMatches();
    testNoMatchAndInvalidInputs();
    testChunkOverlapConvention();
    testAbsoluteAddressAlignment();
    puts("search_match_test: ok");
    return 0;
}
