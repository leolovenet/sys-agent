#include "debug_wait.h"

#include <string.h>

#define DEBUG_WAIT_THREAD_STACK_SIZE 0x4000
#define DEBUG_WAIT_THREAD_PRIORITY 0x2F

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadStarted;
    bool active;
    u32 state;
    bool application;
    bool waited;
    u64 titleId;
    u64 processId;
    u64 timeoutMs;
    Event event;
    Result lastError;
    char stage[32];
} DebugWaitContext;

static DebugWaitContext waitContext;

static void setStage(const char* stage)
{
    strncpy(waitContext.stage, stage, sizeof(waitContext.stage) - 1);
    waitContext.stage[sizeof(waitContext.stage) - 1] = 0;
}

static void waitThreadMain(void* arg)
{
    (void)arg;
    Event event;
    u64 titleId = 0;
    u64 timeoutMs = 0;
    bool application = false;

    mutexLock(&waitContext.mutex);
    event = waitContext.event;
    titleId = waitContext.titleId;
    timeoutMs = waitContext.timeoutMs;
    application = waitContext.application;
    mutexUnlock(&waitContext.mutex);

    Result rc = eventWait(&event, timeoutMs * 1000000ULL);
    u64 pid = 0;
    if (R_SUCCEEDED(rc)) {
        for (u32 attempt = 0; attempt < 50; attempt++) {
            rc = application
                ? pmdmntGetApplicationProcessId(&pid)
                : pmdmntGetProcessId(&pid, titleId);
            if (R_SUCCEEDED(rc) && pid != 0)
                break;
            svcSleepThread(20000000LL);
        }
    }

    pmdmntClearHook(application ? 2 : 1);
    eventClose(&event);

    mutexLock(&waitContext.mutex);
    waitContext.event.revent = INVALID_HANDLE;
    waitContext.event.wevent = INVALID_HANDLE;
    waitContext.event.autoclear = false;
    waitContext.active = false;
    waitContext.processId = pid;
    waitContext.lastError = rc;
    if (R_SUCCEEDED(rc) && pid != 0) {
        waitContext.state = DEBUG_WAIT_STATE_PARKED;
        waitContext.waited = true;
        setStage("parked");
    } else if (rc == KERNELRESULT(TimedOut)) {
        waitContext.state = DEBUG_WAIT_STATE_TIMEOUT;
        setStage("timeout");
    } else {
        waitContext.state = DEBUG_WAIT_STATE_ERROR;
        setStage("queryProcess");
    }
    mutexUnlock(&waitContext.mutex);
    threadExit();
}

void debugWaitInitialize(void)
{
    memset(&waitContext, 0, sizeof(waitContext));
    mutexInit(&waitContext.mutex);
    waitContext.event.revent = INVALID_HANDLE;
    waitContext.event.wevent = INVALID_HANDLE;
    waitContext.event.autoclear = false;
    waitContext.state = DEBUG_WAIT_STATE_IDLE;
}

static bool startWait(bool application, u64 titleId, u64 timeoutMs)
{
    if (timeoutMs == 0 || (!application && titleId == 0))
        return false;

    Event event;
    memset(&event, 0, sizeof(event));

    mutexLock(&waitContext.mutex);
    if (waitContext.active) {
        mutexUnlock(&waitContext.mutex);
        return false;
    }
    if (waitContext.threadStarted) {
        mutexUnlock(&waitContext.mutex);
        threadWaitForExit(&waitContext.thread);
        threadClose(&waitContext.thread);
        mutexLock(&waitContext.mutex);
        waitContext.threadStarted = false;
    }
    waitContext.active = true;
    waitContext.state = DEBUG_WAIT_STATE_WAITING;
    waitContext.application = application;
    waitContext.waited = false;
    waitContext.titleId = titleId;
    waitContext.processId = 0;
    waitContext.timeoutMs = timeoutMs;
    waitContext.lastError = 0;
    waitContext.event.revent = INVALID_HANDLE;
    waitContext.event.wevent = INVALID_HANDLE;
    waitContext.event.autoclear = false;
    setStage("registerHook");
    mutexUnlock(&waitContext.mutex);

    Result rc = application
        ? pmdmntHookToCreateApplicationProcess(&event)
        : pmdmntHookToCreateProcess(&event, titleId);
    if (R_FAILED(rc)) {
        mutexLock(&waitContext.mutex);
        waitContext.active = false;
        waitContext.state = DEBUG_WAIT_STATE_ERROR;
        waitContext.lastError = rc;
        setStage("registerHook");
        mutexUnlock(&waitContext.mutex);
        return false;
    }

    u64 pid = 0;
    rc = application
        ? pmdmntGetApplicationProcessId(&pid)
        : pmdmntGetProcessId(&pid, titleId);
    if (R_SUCCEEDED(rc) && pid != 0) {
        pmdmntClearHook(application ? 2 : 1);
        eventClose(&event);
        mutexLock(&waitContext.mutex);
        waitContext.active = false;
        waitContext.state = DEBUG_WAIT_STATE_EXISTS;
        waitContext.processId = pid;
        waitContext.waited = false;
        setStage("exists");
        mutexUnlock(&waitContext.mutex);
        return true;
    }

    mutexLock(&waitContext.mutex);
    waitContext.event = event;
    waitContext.threadStarted = true;
    setStage("waitEvent");
    mutexUnlock(&waitContext.mutex);

    Result createRc = threadCreate(&waitContext.thread, waitThreadMain, NULL, NULL,
        DEBUG_WAIT_THREAD_STACK_SIZE, DEBUG_WAIT_THREAD_PRIORITY, -2);
    Result startRc = R_FAILED(createRc)
        ? createRc : threadStart(&waitContext.thread);
    if (R_FAILED(startRc)) {
        pmdmntClearHook(application ? 2 : 1);
        eventClose(&event);
        mutexLock(&waitContext.mutex);
        waitContext.event.revent = INVALID_HANDLE;
        waitContext.event.wevent = INVALID_HANDLE;
        waitContext.event.autoclear = false;
        waitContext.threadStarted = false;
        waitContext.active = false;
        waitContext.state = DEBUG_WAIT_STATE_ERROR;
        waitContext.lastError = startRc;
        setStage("threadStart");
        mutexUnlock(&waitContext.mutex);
        if (R_SUCCEEDED(createRc))
            threadClose(&waitContext.thread);
        return false;
    }
    return true;
}

bool debugWaitStartTitle(u64 titleId, u64 timeoutMs)
{
    return startWait(false, titleId, timeoutMs);
}

bool debugWaitStartApplication(u64 timeoutMs)
{
    return startWait(true, 0, timeoutMs);
}

void debugWaitMarkStarted(u64 processId)
{
    mutexLock(&waitContext.mutex);
    if (waitContext.processId == processId
        && waitContext.state == DEBUG_WAIT_STATE_PARKED) {
        waitContext.state = DEBUG_WAIT_STATE_STARTED;
        setStage("started");
    }
    mutexUnlock(&waitContext.mutex);
}

bool debugWaitGetStatus(DebugWaitStatus* out)
{
    if (out == NULL)
        return false;
    memset(out, 0, sizeof(*out));
    mutexLock(&waitContext.mutex);
    out->active = waitContext.active;
    out->state = waitContext.state;
    out->application = waitContext.application;
    out->waited = waitContext.waited;
    out->titleId = waitContext.titleId;
    out->processId = waitContext.processId;
    out->timeoutMs = waitContext.timeoutMs;
    out->lastError = waitContext.lastError;
    strncpy(out->stage, waitContext.stage, sizeof(out->stage) - 1);
    out->stage[sizeof(out->stage) - 1] = 0;
    mutexUnlock(&waitContext.mutex);
    return true;
}

const char* debugWaitStateName(u32 state)
{
    switch (state) {
        case DEBUG_WAIT_STATE_IDLE: return "idle";
        case DEBUG_WAIT_STATE_WAITING: return "waiting";
        case DEBUG_WAIT_STATE_PARKED: return "parked";
        case DEBUG_WAIT_STATE_EXISTS: return "exists";
        case DEBUG_WAIT_STATE_TIMEOUT: return "timeout";
        case DEBUG_WAIT_STATE_ERROR: return "error";
        case DEBUG_WAIT_STATE_STARTED: return "started";
        default: return "unknown";
    }
}
