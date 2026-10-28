#pragma once

#include <switch.h>

#define DEBUG_WAIT_STATE_IDLE 0
#define DEBUG_WAIT_STATE_WAITING 1
#define DEBUG_WAIT_STATE_PARKED 2
#define DEBUG_WAIT_STATE_EXISTS 3
#define DEBUG_WAIT_STATE_TIMEOUT 4
#define DEBUG_WAIT_STATE_ERROR 5
#define DEBUG_WAIT_STATE_STARTED 6

typedef struct {
    bool active;
    u32 state;
    bool application;
    bool waited;
    u64 titleId;
    u64 processId;
    u64 timeoutMs;
    Result lastError;
    char stage[32];
} DebugWaitStatus;

void debugWaitInitialize(void);
bool debugWaitStartTitle(u64 titleId, u64 timeoutMs);
bool debugWaitStartApplication(u64 timeoutMs);
void debugWaitMarkStarted(u64 processId);
bool debugWaitGetStatus(DebugWaitStatus* out);
const char* debugWaitStateName(u32 state);
