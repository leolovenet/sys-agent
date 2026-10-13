#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Repeat syntax for clickSeq sequences: `(A,W100)*10` and `DDOWN*3`.
 * Kept free of libnx headers so tests/ can compile it off-target.
 *
 * A sequence is expanded on the command thread *before* it is queued, so a
 * malformed sequence is answered with an ERR line instead of quietly sending no
 * input at all (the failure mode that once made clickSeq look like "the command
 * worked but nothing happened"). Everything a sequence without repeats is
 * passed through byte for byte. */

#define SEQUENCE_REPEAT_MAX_COUNT 1000u      /* per '*N' */
#define SEQUENCE_REPEAT_MAX_DEPTH 4u         /* nested groups */
#define SEQUENCE_REPEAT_MAX_TOKENS 1024u     /* expanded tokens per sequence */
#define SEQUENCE_EXPAND_CAPACITY (64u * 1024u) /* output buffer the caller allocates */

typedef enum {
    SequenceExpandOk = 0,
    SequenceExpandBadSyntax,     /* stray ')', unclosed '(', '*' without a count,
                                  * '*' without a token, empty group, control byte */
    SequenceExpandCountRange,    /* count outside 1..SEQUENCE_REPEAT_MAX_COUNT */
    SequenceExpandTooDeep,       /* groups nested deeper than SEQUENCE_REPEAT_MAX_DEPTH */
    SequenceExpandTooManyTokens, /* above SEQUENCE_REPEAT_MAX_TOKENS */
    SequenceExpandTooLong,       /* expansion does not fit the caller's buffer */
} SequenceExpandResult;

typedef struct {
    size_t errorIndex;     /* input byte offset the failure refers to */
    size_t tokenCount;     /* expanded token count (filled in on success) */
    uint64_t estimatedMs;  /* see `clickCostMs` below (filled in on success) */
} SequenceExpandInfo;

/* Expands `sequence` into `out`, which comes back NUL-terminated with the tokens
 * joined by single commas (no empty tokens, no trailing comma).
 *
 * `clickCostMs` is the runtime model behind `info->estimatedMs`: a `W<ms>` token
 * costs its own milliseconds, `+`/`-`/`%`/`&` tokens are single state writes and
 * everything else is a click, which holds the button for that long. Callers pass
 * their button click delay; the estimate is advisory (it sizes a client's read
 * timeout), never a limit. */
SequenceExpandResult sequenceExpand(const char* sequence, char* out, size_t outSize,
                                    uint64_t clickCostMs, SequenceExpandInfo* info);

/* Short name used in the `ERR code=BAD_SEQUENCE ... detail=` line. */
const char* sequenceExpandResultName(SequenceExpandResult result);
