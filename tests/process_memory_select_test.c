#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "process_memory_select.h"

typedef struct {
    int32_t dmntResult;
    int32_t directResult;
    bool acquired;
    int dmntCalls;
    int directCalls;
} FakeBackends;

static int32_t openDmnt(void* context, bool* acquired)
{
    FakeBackends* fake = context;
    fake->dmntCalls++;
    *acquired = fake->acquired;
    return fake->dmntResult;
}

static int32_t openDirect(void* context)
{
    FakeBackends* fake = context;
    fake->directCalls++;
    return fake->directResult;
}

static PmSelectBackend select(PmSelectPolicy policy, FakeBackends* fake, int32_t* result)
{
    PmSelectBackend selected;
    *result = processMemorySelectBackend(policy, openDmnt, openDirect, fake, &selected);
    return selected;
}

int main(void)
{
    int32_t rc;
    FakeBackends attached = { 0 };
    assert(select(PmSelectAuto, &attached, &rc) == PmSelectDmntBackend && rc == 0);
    assert(attached.dmntCalls == 1 && attached.directCalls == 0);

    FakeBackends fallback = { .dmntResult = 10, .directResult = 0, .acquired = false };
    assert(select(PmSelectAuto, &fallback, &rc) == PmSelectDirectBackend && rc == 0);
    assert(fallback.dmntCalls == 1 && fallback.directCalls == 1);

    FakeBackends ownedFailure = { .dmntResult = 11, .directResult = 0, .acquired = true };
    assert(select(PmSelectAuto, &ownedFailure, &rc) == PmSelectNone && rc == 11);
    assert(ownedFailure.directCalls == 0);

    FakeBackends forcedDmnt = { .dmntResult = 12, .directResult = 0 };
    assert(select(PmSelectDmnt, &forcedDmnt, &rc) == PmSelectNone && rc == 12);
    assert(forcedDmnt.directCalls == 0);

    FakeBackends forcedDirect = { .dmntResult = 0, .directResult = 13 };
    assert(select(PmSelectDirect, &forcedDirect, &rc) == PmSelectNone && rc == 13);
    assert(forcedDirect.dmntCalls == 0 && forcedDirect.directCalls == 1);

    puts("process_memory_select_test: ok");
    return 0;
}
