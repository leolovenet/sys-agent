#include "process_memory_select.h"

int32_t processMemorySelectBackend(PmSelectPolicy policy, PmSelectOpenDmnt openDmnt,
    PmSelectOpenDirect openDirect, void* context, PmSelectBackend* selected)
{
    *selected = PmSelectNone;
    if (policy == PmSelectDirect) {
        int32_t rc = openDirect(context);
        if (rc == 0)
            *selected = PmSelectDirectBackend;
        return rc;
    }

    bool acquired = false;
    int32_t rc = openDmnt(context, &acquired);
    if (rc == 0) {
        *selected = PmSelectDmntBackend;
        return 0;
    }
    if (policy == PmSelectDmnt || acquired)
        return rc;

    rc = openDirect(context);
    if (rc == 0)
        *selected = PmSelectDirectBackend;
    return rc;
}
