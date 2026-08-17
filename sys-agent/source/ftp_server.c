#include "ftp_server.h"

#include "ftpsrv.h"

#include <errno.h>
#include <string.h>
#include <switch.h>

#define FTP_THREAD_STACK_SIZE 0x18000
#define FTP_THREAD_PRIORITY 0x2D
#define FTP_LOOP_TIMEOUT_MS 100

typedef struct {
    Mutex mutex;
    Thread thread;
    bool threadCreated;
    bool shutdown;
    bool desiredRunning;
    bool restartRequested;
    bool reloadRequested;
    bool serverInitialized;
    FtpServerStatus status;
} FtpManager;

static FtpManager manager;

static void setState(FtpServerState state, int error)
{
    mutexLock(&manager.mutex);
    manager.status.state = state;
    manager.status.lastError = error;
    mutexUnlock(&manager.mutex);
}

static void loadConfig(void)
{
    FtpConfig config;
    ftpConfigDefaults(&config);
    FtpConfigResult result = ftpConfigLoad(FTP_CONFIG_PATH, &config);
    mutexLock(&manager.mutex);
    manager.status.config = config;
    manager.status.configResult = result;
    if (result != FtpConfigOk) {
        manager.desiredRunning = false;
        manager.status.state = FtpServerError;
        manager.status.lastError = EINVAL;
    } else {
        manager.desiredRunning = config.enabled;
        manager.status.state = config.enabled ? FtpServerStarting : FtpServerStopped;
        manager.status.lastError = 0;
    }
    mutexUnlock(&manager.mutex);
}

static void populateLibraryConfig(struct FtpSrvConfig* output, const FtpConfig* input)
{
    memset(output, 0, sizeof(*output));
    memcpy(output->user, input->username, sizeof(output->user));
    memcpy(output->pass, input->password, sizeof(output->pass));
    output->port = input->port;
    output->anon = input->anonymous;
    output->timeout = input->timeout;
}

static void ftpWorker(void* argument)
{
    (void)argument;
    while (true) {
        mutexLock(&manager.mutex);
        bool shutdown = manager.shutdown;
        bool desired = manager.desiredRunning;
        bool restart = manager.restartRequested;
        bool reload = manager.reloadRequested;
        manager.restartRequested = false;
        manager.reloadRequested = false;
        bool initialized = manager.serverInitialized;
        FtpConfig config = manager.status.config;
        mutexUnlock(&manager.mutex);

        if (shutdown)
            break;

        if ((restart || reload) && initialized) {
            setState(FtpServerStopping, 0);
            ftpsrv_exit();
            mutexLock(&manager.mutex);
            manager.serverInitialized = false;
            mutexUnlock(&manager.mutex);
            initialized = false;
        }
        if (reload) {
            loadConfig();
            mutexLock(&manager.mutex);
            desired = manager.desiredRunning;
            config = manager.status.config;
            mutexUnlock(&manager.mutex);
        } else if (restart) {
            desired = true;
            mutexLock(&manager.mutex);
            manager.desiredRunning = true;
            mutexUnlock(&manager.mutex);
        }

        if (!desired && initialized) {
            setState(FtpServerStopping, 0);
            ftpsrv_exit();
            mutexLock(&manager.mutex);
            manager.serverInitialized = false;
            manager.status.state = FtpServerStopped;
            manager.status.activeTransfers = 0;
            mutexUnlock(&manager.mutex);
            initialized = false;
        }

        if (desired && !initialized) {
            setState(FtpServerStarting, 0);
            struct FtpSrvConfig libraryConfig;
            populateLibraryConfig(&libraryConfig, &config);
            int result = ftpsrv_init(&libraryConfig);
            if (result < 0) {
                int error = errno ? errno : EIO;
                ftpsrv_exit();
                mutexLock(&manager.mutex);
                manager.status.state = FtpServerError;
                manager.status.lastError = error;
                mutexUnlock(&manager.mutex);
                svcSleepThread(1000 * 1000 * 1000L);
            } else {
                mutexLock(&manager.mutex);
                manager.serverInitialized = true;
                manager.status.state = FtpServerRunning;
                manager.status.lastError = 0;
                mutexUnlock(&manager.mutex);
                initialized = true;
            }
        }

        if (initialized) {
            int result = ftpsrv_loop(FTP_LOOP_TIMEOUT_MS);
            if (result != FTP_API_LOOP_ERROR_OK) {
                int error = errno ? errno : EIO;
                ftpsrv_exit();
                mutexLock(&manager.mutex);
                manager.serverInitialized = false;
                manager.status.state = FtpServerError;
                manager.status.lastError = error;
                manager.status.activeTransfers = 0;
                mutexUnlock(&manager.mutex);
                svcSleepThread(1000 * 1000 * 1000L);
            }
        } else {
            svcSleepThread(20 * 1000 * 1000L);
        }
    }

    mutexLock(&manager.mutex);
    bool initialized = manager.serverInitialized;
    manager.serverInitialized = false;
    mutexUnlock(&manager.mutex);
    if (initialized)
        ftpsrv_exit();
}

void ftpServerInitialize(bool storageAvailable)
{
    memset(&manager, 0, sizeof(manager));
    mutexInit(&manager.mutex);
    ftpConfigDefaults(&manager.status.config);
    if (!storageAvailable) {
        manager.status.state = FtpServerUnavailable;
        manager.status.configResult = FtpConfigIoError;
        return;
    }

    loadConfig();
    Result result = threadCreate(&manager.thread, ftpWorker, NULL, NULL,
        FTP_THREAD_STACK_SIZE, FTP_THREAD_PRIORITY, -2);
    if (R_FAILED(result)) {
        manager.status.state = FtpServerError;
        manager.status.lastError = (int)result;
        manager.desiredRunning = false;
        return;
    }
    result = threadStart(&manager.thread);
    if (R_FAILED(result)) {
        threadClose(&manager.thread);
        manager.status.state = FtpServerError;
        manager.status.lastError = (int)result;
        manager.desiredRunning = false;
        return;
    }
    manager.threadCreated = true;
}

void ftpServerShutdown(void)
{
    mutexLock(&manager.mutex);
    bool created = manager.threadCreated;
    manager.shutdown = true;
    mutexUnlock(&manager.mutex);
    if (created) {
        threadWaitForExit(&manager.thread);
        threadClose(&manager.thread);
    }
}

bool ftpServerStart(void)
{
    mutexLock(&manager.mutex);
    bool available = manager.threadCreated && manager.status.configResult == FtpConfigOk;
    if (available) {
        manager.desiredRunning = true;
        manager.status.state = FtpServerStarting;
        manager.status.lastError = 0;
    }
    mutexUnlock(&manager.mutex);
    return available;
}

bool ftpServerStop(void)
{
    mutexLock(&manager.mutex);
    bool available = manager.threadCreated;
    if (available) {
        manager.desiredRunning = false;
        if (manager.serverInitialized)
            manager.status.state = FtpServerStopping;
        else
            manager.status.state = FtpServerStopped;
    }
    mutexUnlock(&manager.mutex);
    return available;
}

bool ftpServerRestart(bool reloadConfig)
{
    mutexLock(&manager.mutex);
    bool available = manager.threadCreated
        && (reloadConfig || manager.status.configResult == FtpConfigOk);
    if (available) {
        manager.restartRequested = !reloadConfig;
        manager.reloadRequested = reloadConfig;
        manager.status.state = FtpServerStopping;
    }
    mutexUnlock(&manager.mutex);
    return available;
}

void ftpServerGetStatus(FtpServerStatus* status)
{
    mutexLock(&manager.mutex);
    *status = manager.status;
    mutexUnlock(&manager.mutex);
}

const char* ftpServerStateName(FtpServerState state)
{
    switch (state) {
    case FtpServerUnavailable: return "unavailable";
    case FtpServerStopped: return "stopped";
    case FtpServerStarting: return "starting";
    case FtpServerRunning: return "running";
    case FtpServerStopping: return "stopping";
    case FtpServerError: return "error";
    default: return "unknown";
    }
}

void ftpServerTransferOpened(void)
{
    mutexLock(&manager.mutex);
    manager.status.activeTransfers++;
    mutexUnlock(&manager.mutex);
}

void ftpServerTransferClosed(void)
{
    mutexLock(&manager.mutex);
    if (manager.status.activeTransfers)
        manager.status.activeTransfers--;
    mutexUnlock(&manager.mutex);
}

void ftpServerBytesSent(uint64_t count)
{
    mutexLock(&manager.mutex);
    manager.status.bytesSent += count;
    mutexUnlock(&manager.mutex);
}

void ftpServerBytesReceived(uint64_t count)
{
    mutexLock(&manager.mutex);
    manager.status.bytesReceived += count;
    mutexUnlock(&manager.mutex);
}

void ftpServerRecordFsResult(uint32_t result)
{
    mutexLock(&manager.mutex);
    manager.status.lastFsResult = result;
    mutexUnlock(&manager.mutex);
}
