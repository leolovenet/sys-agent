#pragma once

/* Token splitter for the clickSeq sequences (`A,W600,B`, `+DLEFT`, ...).
 * Kept free of libnx headers so tests/ can compile it off-target.
 *
 * The header exists because the old code passed the address of a single ','
 * to strtok(). strtok() wants a NUL-terminated delimiter *string*, so it read
 * past that one byte into whatever the stack held; on hardware the leaked bytes
 * were "HO_", which turned "HOME" into the unknown token "ME" and made
 * "DRIGHT" two unknown tokens. Unknown tokens send no button at all, so such a
 * sequence still answered "done" while doing nothing. Splitting on a single
 * ',' character cannot read out of the buffer it is given. */

/* Returns the next comma-separated token of `*cursor`, NUL-terminating it in
 * the caller's buffer, and advances `*cursor` past it; NULL once the sequence
 * is exhausted. Pass the sequence on the first call and the same cursor
 * variable on every later call. Comma runs collapse, so empty tokens (leading,
 * trailing or doubled commas) are skipped just like strtok() skipped them.
 *
 * No state is kept between calls: a sequence on the click thread cannot be
 * disturbed by the command line the main thread is parsing. */
char* sequenceTokenNext(char** cursor);
