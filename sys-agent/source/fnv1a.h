#pragma once

#include <stddef.h>
#include <stdint.h>

/* FNV-1a 32-bit constants. Kept here so the sysmodule and the host tests (and
 * any host-side tooling that reproduces a `memoryHash`) agree by construction. */
#define FNV1A32_BASIS 0x811C9DC5u
#define FNV1A32_PRIME 0x01000193u

/* Incremental FNV-1a 32-bit over a byte range; feed the previous result back in
 * to hash a region in chunks. */
uint32_t fnv1a32Update(uint32_t hash, const void* data, size_t size);
