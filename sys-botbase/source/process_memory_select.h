#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PmSelectAuto = 0,
    PmSelectDmnt,
    PmSelectDirect
} PmSelectPolicy;

typedef enum {
    PmSelectNone = 0,
    PmSelectDmntBackend,
    PmSelectDirectBackend
} PmSelectBackend;

typedef int32_t (*PmSelectOpenDmnt)(void* context, bool* acquired);
typedef int32_t (*PmSelectOpenDirect)(void* context);

int32_t processMemorySelectBackend(PmSelectPolicy policy, PmSelectOpenDmnt openDmnt,
    PmSelectOpenDirect openDirect, void* context, PmSelectBackend* selected);
