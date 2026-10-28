#include "debug_trace_model.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool parseUnsigned(const char* text, uint64_t* out)
{
    if (text == NULL || *text == 0 || *text == '+' || *text == '-'
        || out == NULL)
        return false;
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (*text == 0)
        return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long value = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != 0)
        return false;
    *out = (uint64_t)value;
    return true;
}

static bool parseUnsignedRange(const char* text, size_t length,
    uint64_t* out)
{
    if (text == NULL || out == NULL || length == 0 || length >= 64)
        return false;
    char buffer[64];
    memcpy(buffer, text, length);
    buffer[length] = 0;
    return parseUnsigned(buffer, out);
}

static bool parseRegisterName(const char* text, size_t length,
    DebugTraceSnapshotSpec* spec)
{
    if (length == 2 && text[0] == 's' && text[1] == 'p') {
        spec->base = DebugTraceSnapshotBase_Sp;
        spec->registerIndex = 0;
        return true;
    }
    if (length == 2 && text[0] == 'p' && text[1] == 'c') {
        spec->base = DebugTraceSnapshotBase_Pc;
        spec->registerIndex = 0;
        return true;
    }
    if (length == 2 && text[0] == 'l' && text[1] == 'r') {
        spec->base = DebugTraceSnapshotBase_Lr;
        spec->registerIndex = 0;
        return true;
    }
    if (length < 2 || length > 3 || text[0] != 'x')
        return false;

    uint64_t index = 0;
    const char token[4] = { text[0], text[1],
        length == 3 ? text[2] : 0, 0 };
    if (!parseUnsigned(token + 1, &index) || index > 30)
        return false;
    spec->base = DebugTraceSnapshotBase_Gpr;
    spec->registerIndex = (uint8_t)index;
    return true;
}

bool debugTraceAddressIsValid(uint64_t address)
{
    return address != 0 && (address & 3) == 0;
}

bool debugTraceSnapshotSpecIsValid(const DebugTraceSnapshotSpec* spec)
{
    if (spec == NULL)
        return false;
    if (spec->base > DebugTraceSnapshotBase_Lr)
        return false;
    if (spec->base == DebugTraceSnapshotBase_Gpr
        && spec->registerIndex > 30)
        return false;
    return spec->size > 0
        && spec->size <= DEBUG_TRACE_SNAPSHOT_BYTES_MAX;
}

bool debugTraceParseSnapshotSpec(const char* text,
    DebugTraceSnapshotSpec* out)
{
    if (text == NULL || out == NULL)
        return false;
    const char* colon = strchr(text, ':');
    if (colon == NULL || colon == text)
        return false;

    uint64_t size = 0;
    if (!parseUnsigned(colon + 1, &size)
        || size == 0 || size > DEBUG_TRACE_SNAPSHOT_BYTES_MAX)
        return false;

    const char* baseEnd = colon;
    const char* sign = NULL;
    for (const char* cursor = text; cursor < colon; cursor++) {
        if (*cursor == '+' || *cursor == '-') {
            sign = cursor;
            break;
        }
    }
    if (sign != NULL)
        baseEnd = sign;

    DebugTraceSnapshotSpec parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!parseRegisterName(text, (size_t)(baseEnd - text), &parsed))
        return false;

    if (sign != NULL) {
        const bool negative = *sign == '-';
        uint64_t magnitude = 0;
        if (!parseUnsignedRange(sign + 1,
                (size_t)(colon - sign - 1), &magnitude)
            || magnitude > (uint64_t)INT64_MAX)
            return false;
        parsed.offset = negative ? -(int64_t)magnitude
            : (int64_t)magnitude;
    }
    parsed.size = (uint32_t)size;
    if (!debugTraceSnapshotSpecIsValid(&parsed))
        return false;
    *out = parsed;
    return true;
}

bool debugTraceComputeSnapshotAddress(const DebugTraceSnapshotSpec* spec,
    const uint64_t gprs[31], uint64_t sp, uint64_t pc, uint64_t lr,
    uint64_t* outAddress)
{
    if (!debugTraceSnapshotSpecIsValid(spec) || gprs == NULL
        || outAddress == NULL)
        return false;

    uint64_t base = 0;
    switch (spec->base) {
    case DebugTraceSnapshotBase_Gpr:
        base = gprs[spec->registerIndex];
        break;
    case DebugTraceSnapshotBase_Sp:
        base = sp;
        break;
    case DebugTraceSnapshotBase_Pc:
        base = pc;
        break;
    case DebugTraceSnapshotBase_Lr:
        base = lr;
        break;
    default:
        return false;
    }

    if (spec->offset >= 0) {
        const uint64_t offset = (uint64_t)spec->offset;
        if (base > UINT64_MAX - offset)
            return false;
        base += offset;
    } else {
        const uint64_t magnitude =
            (uint64_t)(-(spec->offset + 1)) + 1;
        if (base < magnitude)
            return false;
        base -= magnitude;
    }
    *outAddress = base;
    return true;
}

bool debugTraceConfigIsValid(const DebugTraceConfigModel* config)
{
    if (config == 0 || config->targetCount == 0
        || config->targetCount > DEBUG_TRACE_TARGET_MAX)
        return false;
    for (uint32_t i = 0; i < config->targetCount; i++) {
        if (!debugTraceAddressIsValid(config->addresses[i]))
            return false;
        for (uint32_t prior = 0; prior < i; prior++) {
            if (config->addresses[prior] == config->addresses[i])
                return false;
        }
    }
    if (config->maxHits == 0 && config->ringSize == 0)
        return false;
    if (config->ringSize > DEBUG_TRACE_RING_MAX)
        return false;
    if (config->argsCount > DEBUG_TRACE_ARGS_MAX)
        return false;
    if (config->snapshotCount > DEBUG_TRACE_SNAPSHOT_MAX)
        return false;
    for (uint32_t i = 0; i < config->snapshotCount; i++) {
        if (!debugTraceSnapshotSpecIsValid(&config->snapshots[i]))
            return false;
    }
    return true;
}

uint32_t debugTraceFindTarget(const uint64_t* addresses, uint32_t count,
    uint64_t address)
{
    if (addresses == NULL || count > DEBUG_TRACE_TARGET_MAX)
        return UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (addresses[i] == address)
            return i;
    }
    return UINT32_MAX;
}

uint32_t debugTraceRingOldestIndex(uint32_t next, uint32_t capacity,
    uint32_t count, uint32_t index)
{
    if (capacity == 0 || count == 0 || count > capacity || index >= count)
        return UINT32_MAX;
    const uint32_t oldest = (next + capacity - count) % capacity;
    return (oldest + index) % capacity;
}

uint64_t debugTraceOverwrittenCount(uint64_t hitCount, uint32_t storedCount)
{
    return hitCount > storedCount ? hitCount - storedCount : 0;
}

bool debugTraceHitLimitReached(uint64_t hitCount, uint32_t maxHits)
{
    return maxHits != 0 && hitCount >= maxHits;
}
