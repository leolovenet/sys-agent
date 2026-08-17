#ifndef SYSBOT_FTP_SERVER_H
#define SYSBOT_FTP_SERVER_H

#include "ftp_config.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FtpServerUnavailable = 0,
    FtpServerStopped,
    FtpServerStarting,
    FtpServerRunning,
    FtpServerStopping,
    FtpServerError
} FtpServerState;

typedef struct {
    FtpServerState state;
    FtpConfig config;
    FtpConfigResult configResult;
    int lastError;
    uint32_t lastFsResult;
    uint32_t activeTransfers;
    uint64_t bytesSent;
    uint64_t bytesReceived;
} FtpServerStatus;

void ftpServerInitialize(bool storageAvailable);
void ftpServerShutdown(void);
bool ftpServerStart(void);
bool ftpServerStop(void);
bool ftpServerRestart(bool reloadConfig);
void ftpServerGetStatus(FtpServerStatus* status);
const char* ftpServerStateName(FtpServerState state);

void ftpServerTransferOpened(void);
void ftpServerTransferClosed(void);
void ftpServerBytesSent(uint64_t count);
void ftpServerBytesReceived(uint64_t count);
void ftpServerRecordFsResult(uint32_t result);

#endif
