#pragma once

#include <switch.h>

typedef enum {
    NetworkHttpSubmitted = 0,
    NetworkHttpBusy,
    NetworkHttpInvalidUrl,
    NetworkHttpUnavailable,
} NetworkHttpSubmitResult;

Result networkHttpInitialize(void);
NetworkHttpSubmitResult networkHttpSubmit(bool publicIpDefault, const char* url);
