#include "result_check.h"

bool resultIsOutOfSessions(uint32_t rc)
{
    return rc != 0
        && (rc & 0x1FFu) == SYS_AGENT_SM_MODULE
        && ((rc >> 9) & 0x1FFFu) == SYS_AGENT_SM_OUT_OF_SESSIONS;
}
