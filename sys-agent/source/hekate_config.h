#pragma once

#include <stddef.h>

typedef enum {
    HekateConfigOk = 0,
    HekateConfigOpenFailed,
    HekateConfigNotFound,
    HekateConfigDuplicateId,
    HekateConfigIndexOutOfRange,
} HekateConfigResult;

HekateConfigResult hekateFindMainConfigId(const char* path, const char* wantedId,
    unsigned* entryIndex);
const char* hekateConfigResultName(HekateConfigResult result);

