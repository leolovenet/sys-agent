#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "search_compare.h"

int main(void)
{
    const uint8_t old32[] = { 0x78, 0x56, 0x34, 0x12 };
    const uint8_t same32[] = { 0x78, 0x56, 0x34, 0x12 };
    const uint8_t up32[] = { 0x79, 0x56, 0x34, 0x12 };
    const uint8_t down32[] = { 0x77, 0x56, 0x34, 0x12 };
    assert(searchDecodeUnsigned(old32, 4) == UINT64_C(0x12345678));
    assert(searchCompareUnsigned(SearchCompareExact, old32, same32, 4, 0x12345678));
    assert(searchCompareUnsigned(SearchCompareUnchanged, old32, same32, 4, 0));
    assert(searchCompareUnsigned(SearchCompareChanged, old32, up32, 4, 0));
    assert(searchCompareUnsigned(SearchCompareIncreased, old32, up32, 4, 0));
    assert(searchCompareUnsigned(SearchCompareDecreased, old32, down32, 4, 0));
    assert(!searchCompareUnsigned(SearchCompareIncreased, old32, down32, 4, 0));

    const uint8_t max64[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(searchDecodeUnsigned(max64, 8) == UINT64_MAX);
    const uint8_t old8[] = { 0x7F }, new8[] = { 0x80 };
    assert(searchCompareUnsigned(SearchCompareIncreased, old8, new8, 1, 0));
    const uint8_t old16[] = { 0xFF, 0x00 }, new16[] = { 0x00, 0x01 };
    assert(searchCompareUnsigned(SearchCompareIncreased, old16, new16, 2, 0));
    const uint8_t lower64[] = { 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(searchCompareUnsigned(SearchCompareIncreased, lower64, max64, 8, 0));

    const uint8_t generation0[] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t generation1[] = { 1, 9, 3, 8, 5, 7 };
    const uint8_t allMask[] = { 0x3F };
    uint8_t changedMask[1], increasedMask[1], exactMask[1];
    assert(searchFilterUnsigned(SearchCompareChanged, generation0, generation1,
        allMask, changedMask, 6, 1, 0) == 3);
    assert(changedMask[0] == 0x2A);
    const uint8_t generation2[] = { 1, 8, 3, 10, 5, 7 };
    assert(searchFilterUnsigned(SearchCompareIncreased, generation1, generation2,
        changedMask, increasedMask, 6, 1, 0) == 1);
    assert(increasedMask[0] == 0x08);
    assert(searchFilterUnsigned(SearchCompareExact, generation1, generation2,
        increasedMask, exactMask, 6, 1, 10) == 1);
    assert(exactMask[0] == 0x08);
    puts("search_compare_test: ok");
    return 0;
}
