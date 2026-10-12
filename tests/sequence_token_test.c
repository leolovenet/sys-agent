#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sequence_token.h"

#define MAX_TOKENS 16

/* Walks a sequence exactly like clickSequence() does: first call with the
 * sequence, every later call with the same cursor. */
static size_t tokenize(const char* seq, char tokens[MAX_TOKENS][32])
{
    static char buffer[256];

    assert(strlen(seq) < sizeof(buffer));
    strcpy(buffer, seq);

    size_t count = 0;
    char* cursor = buffer;
    for (char* token = sequenceTokenNext(&cursor); token != NULL;
         token = sequenceTokenNext(&cursor)) {
        assert(count < MAX_TOKENS);
        strcpy(tokens[count], token);
        count++;
    }
    return count;
}

static void expectTokens(const char* seq, const char* const* expected, size_t expectedCount)
{
    char tokens[MAX_TOKENS][32] = { 0 };
    size_t count = tokenize(seq, tokens);

    assert(count == expectedCount);
    for (size_t i = 0; i < count; i++)
        assert(strcmp(tokens[i], expected[i]) == 0);
}

/* The 2.11.0 symptom: strtok() was handed the address of a single ',' and read
 * past it, so the bytes that happened to follow ("HO_") acted as delimiters.
 * "HOME" then arrived at parseStringToButton() as "ME" - an unknown name, which
 * sends no button - and the command still answered "done". Tokens must survive
 * whole whatever letters they contain. */
static void testTokenLettersAreNotDelimiters(void)
{
    static const char* const home[] = { "HOME" };
    static const char* const dright[] = { "DRIGHT" };
    static const char* const ddown[] = { "DDOWN" };
    static const char* const held[] = { "+HOME" };

    expectTokens("HOME", home, 1);
    expectTokens("DRIGHT", dright, 1);
    expectTokens("DDOWN", ddown, 1);
    expectTokens("+HOME", held, 1);

    /* HOME followed by a comma is two tokens, not one truncated name. */
    static const char* const pair[] = { "HOME", "DRIGHT" };
    expectTokens("HOME,DRIGHT", pair, 2);
}

/* The examples in commands.md and client/README.md. */
static void testDocumentedSequences(void)
{
    static const char* const longSequence[] = {
        "A", "W1000", "B", "W200", "DUP", "W500", "DD", "W350", "%5000", "1500", "W2650", "%0", "0"
    };
    static const char* const clicks[] = { "A", "W600", "B" };
    static const char* const leftStick[] = { "%5000", "1500" };
    static const char* const rightStick[] = { "&0", "0" };
    static const char* const pressRelease[] = { "+X", "-X" };

    expectTokens("A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0",
                 longSequence, sizeof(longSequence) / sizeof(longSequence[0]));
    expectTokens("A,W600,B", clicks, sizeof(clicks) / sizeof(clicks[0]));
    expectTokens("%5000,1500", leftStick, sizeof(leftStick) / sizeof(leftStick[0]));
    expectTokens("&0,0", rightStick, sizeof(rightStick) / sizeof(rightStick[0]));
    expectTokens("+X,-X", pressRelease, sizeof(pressRelease) / sizeof(pressRelease[0]));
}

/* Comma runs collapse, which is what strtok() did before. */
static void testEmptyTokensAreSkipped(void)
{
    static const char* const skipped[] = { "A", "B" };

    expectTokens("A,,B", skipped, 2);
    expectTokens(",A,B,", skipped, 2);
    expectTokens(",,,", NULL, 0);
    expectTokens("", NULL, 0);
    expectTokens("W1000,", (const char* const[]){ "W1000" }, 1);
}

/* Two walks must not share state: the sequence runs on the click thread while
 * the main thread tokenises the next incoming command line. */
static void testWalksDoNotShareState(void)
{
    static char first[] = "A,W600,B";
    static char second[] = "X,Y";

    char* firstCursor = first;
    char* secondCursor = second;
    assert(strcmp(sequenceTokenNext(&firstCursor), "A") == 0);

    /* A complete second walk in between must leave the first one untouched. */
    assert(strcmp(sequenceTokenNext(&secondCursor), "X") == 0);
    assert(strcmp(sequenceTokenNext(&secondCursor), "Y") == 0);
    assert(sequenceTokenNext(&secondCursor) == NULL);

    assert(strcmp(sequenceTokenNext(&firstCursor), "W600") == 0);
    assert(strcmp(sequenceTokenNext(&firstCursor), "B") == 0);
    assert(sequenceTokenNext(&firstCursor) == NULL);

    /* An exhausted cursor keeps answering NULL. */
    assert(sequenceTokenNext(&firstCursor) == NULL);
    assert(sequenceTokenNext(NULL) == NULL);
    char* exhausted = NULL;
    assert(sequenceTokenNext(&exhausted) == NULL);
}

/* The 2.11.0 bug was the delimiter argument, not the splitting rule: the
 * splitter has to stay interchangeable with a correctly used strtok(). */
static void testMatchesStrtokSemantics(void)
{
    static const char* const sequences[] = {
        "A",
        "HOME",
        "HOME,DRIGHT",
        "A,W1000,B,W200,DUP,W500,DD,W350,%5000,1500,W2650,%0,0",
        "+DLEFT,-DLEFT",
        "A,,B",
        ",A,B,",
        ",",
        "",
        "W1000,",
        "&0,0",
        "X,Y,Z,W",
    };

    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        char reference[256];
        char subject[256];
        strcpy(reference, sequences[i]);
        strcpy(subject, sequences[i]);

        char* referenceSave = NULL;
        char* subjectCursor = subject;
        for (char* expected = strtok_r(reference, ",", &referenceSave),
                 * actual = sequenceTokenNext(&subjectCursor);
             ;
             expected = strtok_r(NULL, ",", &referenceSave),
                 actual = sequenceTokenNext(&subjectCursor)) {
            assert((expected == NULL) == (actual == NULL));
            if (expected == NULL)
                break;
            assert(strcmp(expected, actual) == 0);
        }
    }
}

int main(void)
{
    testTokenLettersAreNotDelimiters();
    testDocumentedSequences();
    testEmptyTokensAreSkipped();
    testWalksDoNotShareState();
    testMatchesStrtokSemantics();
    printf("sequence_token tests passed\n");
    return 0;
}
