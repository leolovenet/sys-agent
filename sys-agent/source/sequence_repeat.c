#include "sequence_repeat.h"

#include <string.h>

typedef struct {
    const char* input;
    size_t length;
    char* out;
    size_t outSize; /* bytes available including the terminating NUL */
    size_t outLen;
    size_t tokens;
    uint64_t clickCostMs;
    uint64_t estimatedMs;
    bool stickValueNext; /* the token after `%x`/`&x` is that stick's y value */
    SequenceExpandResult result;
    size_t errorIndex;
} ExpandState;

static bool fail(ExpandState* state, SequenceExpandResult result, size_t errorIndex)
{
    state->result = result;
    state->errorIndex = errorIndex;
    return false;
}

static bool isDigit(char value)
{
    return value >= '0' && value <= '9';
}

/* The command line cannot carry these, and a sequence has no use for them. */
static bool isControlByte(char value)
{
    unsigned char byte = (unsigned char)value;
    return byte < 0x20 || byte == 0x7F;
}

/* Value of a `W` token, following the same convention as parseStringToInt():
 * "0x"/"0X" prefix means hexadecimal, anything unparseable is 0 ms. */
static uint64_t parseWaitMs(const ExpandState* state, size_t begin, size_t end)
{
    uint64_t base = 10;
    uint64_t value = 0;

    if (begin + 1 < end && state->input[begin] == '0' &&
        (state->input[begin + 1] == 'x' || state->input[begin + 1] == 'X')) {
        base = 16;
        begin += 2;
    }

    for (size_t i = begin; i < end; i++) {
        char byte = state->input[i];
        uint64_t digit;
        if (byte >= '0' && byte <= '9')
            digit = (uint64_t)(byte - '0');
        else if (base == 16 && byte >= 'a' && byte <= 'f')
            digit = (uint64_t)(byte - 'a' + 10);
        else if (base == 16 && byte >= 'A' && byte <= 'F')
            digit = (uint64_t)(byte - 'A' + 10);
        else
            return 0;
        if (value > (UINT64_MAX - digit) / base)
            return UINT64_MAX;
        value = value * base + digit;
    }
    return value;
}

static uint64_t tokenCostMs(ExpandState* state, size_t begin, size_t end)
{
    /* A stick token and its y value are consumed by one branch, so the value
     * token is not a click of its own. */
    if (state->stickValueNext) {
        state->stickValueNext = false;
        return 0;
    }

    char first = state->input[begin];
    if (first == 'W')
        return parseWaitMs(state, begin + 1, end);
    if (first == '+' || first == '-')
        return 0;
    if (first == '%' || first == '&') {
        state->stickValueNext = true;
        return 0;
    }
    return state->clickCostMs;
}

/* Appends one token: comma-joined, so a sequence without repeats is emitted
 * byte for byte and the output never holds an empty element. */
static bool emitToken(ExpandState* state, size_t begin, size_t end)
{
    size_t length = end - begin;
    if (state->tokens + 1 > SEQUENCE_REPEAT_MAX_TOKENS)
        return fail(state, SequenceExpandTooManyTokens, begin);

    size_t needed = length + (state->tokens != 0 ? 1u : 0u) + 1u;
    if (state->outLen + needed > state->outSize)
        return fail(state, SequenceExpandTooLong, begin);

    if (state->tokens != 0)
        state->out[state->outLen++] = ',';
    memcpy(state->out + state->outLen, state->input + begin, length);
    state->outLen += length;
    state->out[state->outLen] = '\0';
    state->estimatedMs += tokenCostMs(state, begin, end);
    state->tokens++;
    return true;
}

/* Reads the count of a '*N' that starts at starIndex. The accumulator cannot
 * overflow: it stops at the first value above the limit. */
static bool parseCount(ExpandState* state, size_t starIndex, size_t* cursor,
                       size_t end, size_t* count)
{
    size_t value = 0;
    size_t digits = 0;
    size_t pos = starIndex + 1;

    while (pos < end && isDigit(state->input[pos])) {
        value = value * 10u + (size_t)(state->input[pos] - '0');
        pos++;
        digits++;
        if (value > SEQUENCE_REPEAT_MAX_COUNT)
            return fail(state, SequenceExpandCountRange, starIndex);
    }
    if (digits == 0)
        return fail(state, SequenceExpandBadSyntax, starIndex);
    if (value == 0)
        return fail(state, SequenceExpandCountRange, starIndex);

    *count = value;
    *cursor = pos;
    return true;
}

/* Walks the items of input[begin,end) and emits their tokens. `depth` is the
 * group nesting level of the range (0 at the top level). */
static bool expandItems(ExpandState* state, size_t begin, size_t end, size_t depth)
{
    size_t pos = begin;

    while (pos < end) {
        size_t itemStart = pos;
        size_t bodyBegin = 0;
        size_t bodyEnd = 0;
        size_t tokenBegin = 0;
        size_t tokenEnd = 0;
        bool group = false;

        if (state->input[pos] == '(') {
            group = true;
            if (depth + 1 > SEQUENCE_REPEAT_MAX_DEPTH)
                return fail(state, SequenceExpandTooDeep, pos);

            /* Find the ')' that closes this group, skipping nested groups. */
            size_t cursor = pos + 1;
            size_t nesting = 1;
            while (cursor < end && nesting != 0) {
                char value = state->input[cursor];
                if (isControlByte(value))
                    return fail(state, SequenceExpandBadSyntax, cursor);
                if (value == '(')
                    nesting++;
                else if (value == ')')
                    nesting--;
                if (nesting != 0)
                    cursor++;
            }
            if (nesting != 0)
                return fail(state, SequenceExpandBadSyntax, pos);

            bodyBegin = pos + 1;
            bodyEnd = cursor;   /* the matching ')' */
            pos = cursor + 1;
        }
        else if (state->input[pos] == ')') {
            return fail(state, SequenceExpandBadSyntax, pos);
        }
        else {
            tokenBegin = pos;
            while (pos < end && state->input[pos] != ',' && state->input[pos] != '*' &&
                   state->input[pos] != ')') {
                if (isControlByte(state->input[pos]))
                    return fail(state, SequenceExpandBadSyntax, pos);
                pos++;
            }
            tokenEnd = pos;
        }

        size_t count = 1;
        if (pos < end && state->input[pos] == '*') {
            if (!parseCount(state, pos, &pos, end, &count))
                return false;
            if (!group && tokenEnd == tokenBegin)
                return fail(state, SequenceExpandBadSyntax, itemStart);
        }

        if (group) {
            size_t emitted = state->tokens;
            for (size_t i = 0; i < count; i++) {
                if (!expandItems(state, bodyBegin, bodyEnd, depth + 1))
                    return false;
                if (i == 0 && state->tokens == emitted)
                    return fail(state, SequenceExpandBadSyntax, itemStart);
            }
        }
        else if (tokenEnd > tokenBegin) {
            for (size_t i = 0; i < count; i++) {
                if (!emitToken(state, tokenBegin, tokenEnd))
                    return false;
            }
        }
        /* An empty element (leading, trailing or doubled comma) is skipped, the
         * same way the sequence walker collapses comma runs. */

        if (pos < end) {
            if (state->input[pos] != ',')
                return fail(state, SequenceExpandBadSyntax, pos);
            pos++;
        }
    }
    return true;
}

SequenceExpandResult sequenceExpand(const char* sequence, char* out, size_t outSize,
                                    uint64_t clickCostMs, SequenceExpandInfo* info)
{
    SequenceExpandInfo scratch = { 0, 0, 0 };
    if (info == NULL)
        info = &scratch;
    info->errorIndex = 0;
    info->tokenCount = 0;
    info->estimatedMs = 0;

    if (out == NULL || outSize == 0)
        return SequenceExpandTooLong;
    out[0] = '\0';
    if (sequence == NULL)
        return SequenceExpandOk;

    ExpandState state;
    state.input = sequence;
    state.length = strlen(sequence);
    state.out = out;
    state.outSize = outSize;
    state.outLen = 0;
    state.tokens = 0;
    state.clickCostMs = clickCostMs;
    state.estimatedMs = 0;
    state.stickValueNext = false;
    state.result = SequenceExpandOk;
    state.errorIndex = 0;

    if (!expandItems(&state, 0, state.length, 0)) {
        out[0] = '\0';
        info->errorIndex = state.errorIndex;
        info->tokenCount = state.tokens;
        return state.result;
    }

    info->tokenCount = state.tokens;
    info->estimatedMs = state.estimatedMs;
    return SequenceExpandOk;
}

const char* sequenceExpandResultName(SequenceExpandResult result)
{
    switch (result) {
    case SequenceExpandOk: return "ok";
    case SequenceExpandBadSyntax: return "bad-syntax";
    case SequenceExpandCountRange: return "count-range";
    case SequenceExpandTooDeep: return "too-deep";
    case SequenceExpandTooManyTokens: return "too-many-tokens";
    case SequenceExpandTooLong: return "too-long";
    }
    return "unknown";
}
