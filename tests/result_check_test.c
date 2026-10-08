#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "result_check.h"

/* The raw code Nick saw rendered as "2021-0003" in the fatal report. */
#define SM_OUT_OF_SESSIONS 0x615u

static void testKnownResultValues(void)
{
    /* Pin the module/description pair to the raw code: module is the low 9
     * bits, description the next 13. */
    assert((SYS_AGENT_SM_MODULE | (SYS_AGENT_SM_OUT_OF_SESSIONS << 9)) == SM_OUT_OF_SESSIONS);
    assert(resultIsOutOfSessions(SM_OUT_OF_SESSIONS));
}

static void testRejectsOtherResults(void)
{
    assert(!resultIsOutOfSessions(0));                 /* success */
    assert(!resultIsOutOfSessions(0x10801u));          /* svc LimitReached (2021-0132) */
    assert(!resultIsOutOfSessions(SYS_AGENT_SM_MODULE));               /* module match, no description */
    assert(!resultIsOutOfSessions(SYS_AGENT_SM_OUT_OF_SESSIONS << 9)); /* description match, module 0 */
    assert(!resultIsOutOfSessions(SYS_AGENT_SM_MODULE | (1u << 9)));   /* sm OutOfProcesses (desc 1) */
}

int main(void)
{
    testKnownResultValues();
    testRejectsOtherResults();
    printf("result_check tests passed\n");
    return 0;
}
