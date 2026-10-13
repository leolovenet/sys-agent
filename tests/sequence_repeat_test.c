#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sequence_repeat.h"

#define CLICK_COST_MS 50u /* the sysmodule's buttonClickSleepTime */

static char output[SEQUENCE_EXPAND_CAPACITY];

static SequenceExpandResult expand(const char* sequence)
{
    SequenceExpandInfo info;
    memset(output, 0xAA, sizeof(output));
    return sequenceExpand(sequence, output, sizeof(output), CLICK_COST_MS, &info);
}

static void expectExpand(const char* sequence, const char* expected)
{
    SequenceExpandInfo info;
    SequenceExpandResult result =
        sequenceExpand(sequence, output, sizeof(output), CLICK_COST_MS, &info);
    if (result != SequenceExpandOk) {
        printf("unexpected %s for %s\n", sequenceExpandResultName(result), sequence);
        assert(0);
    }
    assert(strcmp(output, expected) == 0);

    /* The expanded text must be a plain sequence: expanding it again is a no-op. */
    char again[SEQUENCE_EXPAND_CAPACITY];
    assert(sequenceExpand(output, again, sizeof(again), CLICK_COST_MS, &info) == SequenceExpandOk);
    assert(strcmp(again, expected) == 0);
}

static void expectError(const char* sequence, SequenceExpandResult expected, size_t errorIndex)
{
    SequenceExpandInfo info;
    SequenceExpandResult result =
        sequenceExpand(sequence, output, sizeof(output), CLICK_COST_MS, &info);
    if (result != expected) {
        printf("expected %s for %s, got %s\n", sequenceExpandResultName(expected), sequence,
               sequenceExpandResultName(result));
        assert(0);
    }
    assert(info.errorIndex == errorIndex);
}

/* Sequences without the new syntax must pass through untouched: that is what
 * every existing caller sends. */
static void testPlainSequencesAreUnchanged(void)
{
    expectExpand("", "");
    expectExpand("A", "A");
    expectExpand("A,W1000,B", "A,W1000,B");
    expectExpand("HOME,DRIGHT", "HOME,DRIGHT");
    expectExpand("+DLEFT,-DLEFT", "+DLEFT,-DLEFT");
    expectExpand("A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0",
                 "A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0");
    expectExpand(",A,,B,", "A,B");
}

static void testTokenAndGroupRepeats(void)
{
    expectExpand("DDOWN*3", "DDOWN,DDOWN,DDOWN");
    expectExpand("(DDOWN,W100)*2", "DDOWN,W100,DDOWN,W100");
    expectExpand("A*1", "A");
    expectExpand("(A,W100)*1", "A,W100");
    /* The sequence from the field report, ten presses with 100 ms between them. */
    expectExpand("PLUS,W500,Y,W300,(DDOWN,W100)*10,A,W200",
                 "PLUS,W500,Y,W300,DDOWN,W100,DDOWN,W100,DDOWN,W100,DDOWN,W100,DDOWN,W100,"
                 "DDOWN,W100,DDOWN,W100,DDOWN,W100,DDOWN,W100,DDOWN,W100,A,W200");
    /* Stick tokens stay paired inside the group, and empty elements collapse. */
    expectExpand("(%5000,1500)*2", "%5000,1500,%5000,1500");
    expectExpand("(A,,B)*2", "A,B,A,B");
    expectExpand("A*2,W50,B*3", "A,A,W50,B,B,B");
}

static void testNestedGroups(void)
{
    expectExpand("((A)*2,B)*2", "A,A,B,A,A,B");
    expectExpand("(A*2,B*2)*2", "A,A,B,B,A,A,B,B");
    expectExpand("((A,B)*2)*2", "A,B,A,B,A,B,A,B");
}

static void testTokenCap(void)
{
    char sequence[64];
    SequenceExpandInfo info;

    /* Exactly at the cap is fine. */
    snprintf(sequence, sizeof(sequence), "(A,B)*%u", SEQUENCE_REPEAT_MAX_TOKENS / 2);
    assert(sequenceExpand(sequence, output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandOk);
    assert(info.tokenCount == SEQUENCE_REPEAT_MAX_TOKENS);

    /* One token over: the cap is on the expanded sequence, not on the count,
     * so a repeated pair can reach it with a count inside the legal range. */
    snprintf(sequence, sizeof(sequence), "(A,B)*%u", SEQUENCE_REPEAT_MAX_TOKENS / 2 + 1);
    assert(sequenceExpand(sequence, output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandTooManyTokens);
    assert(info.tokenCount == SEQUENCE_REPEAT_MAX_TOKENS);
}

static void testCountRange(void)
{
    expectError("*3", SequenceExpandBadSyntax, 0);
    expectError("A*0", SequenceExpandCountRange, 1);
    expectError("A*", SequenceExpandBadSyntax, 1);
    expectError("A*x2", SequenceExpandBadSyntax, 1);
    expectError("A*2x", SequenceExpandBadSyntax, 3);
    expectError("(A)*0", SequenceExpandCountRange, 3);

    char sequence[64];
    snprintf(sequence, sizeof(sequence), "A*%u", SEQUENCE_REPEAT_MAX_COUNT + 1);
    expectError(sequence, SequenceExpandCountRange, 1);
    snprintf(sequence, sizeof(sequence), "A*%u", SEQUENCE_REPEAT_MAX_COUNT);
    assert(expand(sequence) == SequenceExpandOk);
    /* A long digit run must be reported, not overflowed. */
    expectError("A*99999999999999999999", SequenceExpandCountRange, 1);
}

static void testBadSyntax(void)
{
    expectError("(A*2", SequenceExpandBadSyntax, 0);       /* unclosed group */
    expectError("A)*2", SequenceExpandBadSyntax, 1);       /* stray ')' */
    expectError("()*2", SequenceExpandBadSyntax, 0);       /* empty group */
    expectError("(,)*2", SequenceExpandBadSyntax, 0);
    expectError("(A),B)", SequenceExpandBadSyntax, 5);
    expectError("A*2*2", SequenceExpandBadSyntax, 3);
    expectError("A\tB", SequenceExpandBadSyntax, 1);       /* control byte */
    expectError("(A,W100)*2,W50*", SequenceExpandBadSyntax, 14);
}

static void testDepthCap(void)
{
    expectExpand("(((A)))", "A");
    expectExpand("((((A))))", "A");
    expectError("(((((A)))))", SequenceExpandTooDeep, 4);
}

static void testTooLongReportsTheOffendingToken(void)
{
    char sequence[512];
    SequenceExpandInfo info;

    /* 64 KiB cannot hold 1000 tokens of 100 bytes each. */
    memset(sequence, 'x', 100);
    sequence[100] = '\0';
    char repeat[512];
    snprintf(repeat, sizeof(repeat), "(%s)*1000", sequence);

    assert(sequenceExpand(repeat, output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandTooLong);
    assert(info.errorIndex == 1);

    /* A caller with a smaller buffer gets the same answer. */
    char small[32];
    assert(sequenceExpand("(DDOWN,W100)*10", small, sizeof(small), CLICK_COST_MS, &info) ==
           SequenceExpandTooLong);
    assert(sequenceExpand(NULL, NULL, 0, CLICK_COST_MS, &info) == SequenceExpandTooLong);
    assert(sequenceExpand(NULL, small, sizeof(small), CLICK_COST_MS, &info) == SequenceExpandOk);
    assert(small[0] == '\0');
}

/* The estimate is what the client sizes its read timeout with, so it has to
 * follow the same runtime model the sequence walker uses. */
static void testRuntimeEstimate(void)
{
    SequenceExpandInfo info;

    assert(sequenceExpand("A", output, sizeof(output), CLICK_COST_MS, &info) == SequenceExpandOk);
    assert(info.estimatedMs == CLICK_COST_MS);

    assert(sequenceExpand("A,W100,B", output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandOk);
    assert(info.estimatedMs == 2 * CLICK_COST_MS + 100);

    /* Press/release and stick tokens are state writes, not holds. */
    assert(sequenceExpand("+DLEFT,-DLEFT,%0,0,W20", output, sizeof(output), CLICK_COST_MS, &info)
           == SequenceExpandOk);
    assert(info.estimatedMs == 20);

    /* Waits follow parseStringToInt(): hex with a prefix, 0 ms when unparseable. */
    assert(sequenceExpand("W0x64", output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandOk);
    assert(info.estimatedMs == 100);
    assert(sequenceExpand("Wabc", output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandOk);
    assert(info.estimatedMs == 0);

    /* Repeats multiply, which is the whole point of reporting the estimate. */
    assert(sequenceExpand("(DDOWN,W100)*10", output, sizeof(output), CLICK_COST_MS, &info) ==
           SequenceExpandOk);
    assert(info.estimatedMs == 10 * (CLICK_COST_MS + 100));
}

int main(void)
{
    testPlainSequencesAreUnchanged();
    testTokenAndGroupRepeats();
    testNestedGroups();
    testTokenCap();
    testCountRange();
    testBadSyntax();
    testDepthCap();
    testTooLongReportsTheOffendingToken();
    testRuntimeEstimate();
    printf("sequence_repeat tests passed\n");
    return 0;
}
