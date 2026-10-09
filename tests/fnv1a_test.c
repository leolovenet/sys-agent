#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fnv1a.h"

static uint32_t fnv(const char* text)
{
    return fnv1a32Update(FNV1A32_BASIS, text, strlen(text));
}

int main(void)
{
    /* Published FNV-1a 32-bit vectors. */
    assert(fnv("") == 0x811C9DC5u);
    assert(fnv("a") == 0xE40C292Cu);
    assert(fnv("foobar") == 0xBF9CF968u);

    /* Feeding the hash in chunks must match the one-shot result, because the
     * sysmodule hashes a region in 4 KiB blocks. */
    uint32_t chunked = fnv1a32Update(FNV1A32_BASIS, "foo", 3);
    chunked = fnv1a32Update(chunked, "bar", 3);
    assert(chunked == 0xBF9CF968u);

    /* The empty range yields the basis, which is what an empty memoryHash over
     * a zero-length region would return. */
    assert(fnv1a32Update(FNV1A32_BASIS, "", 0) == 0x811C9DC5u);

    printf("fnv1a tests passed\n");
    return 0;
}
