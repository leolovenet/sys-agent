#include "fnv1a.h"

uint32_t fnv1a32Update(uint32_t hash, const void* data, size_t size)
{
    const unsigned char* bytes = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= FNV1A32_PRIME;
    }
    return hash;
}
