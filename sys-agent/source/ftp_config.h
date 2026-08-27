#ifndef SYSAGENT_FTP_CONFIG_H
#define SYSAGENT_FTP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FTP_CONFIG_PATH "sdmc:/config/sys-agent/ftp.ini"
#define FTP_DEFAULT_PORT 6001
#define FTP_DEFAULT_TIMEOUT 30
#define FTP_CREDENTIAL_SIZE 128

typedef struct {
    bool enabled;
    uint16_t port;
    bool anonymous;
    char username[FTP_CREDENTIAL_SIZE];
    char password[FTP_CREDENTIAL_SIZE];
    uint32_t timeout;
    bool use_localtime;
} FtpConfig;

typedef enum {
    FtpConfigOk = 0,
    FtpConfigInvalidValue,
    FtpConfigCredentialsRequired,
    FtpConfigIoError
} FtpConfigResult;

void ftpConfigDefaults(FtpConfig* config);
FtpConfigResult ftpConfigParseText(const char* text, FtpConfig* config);
FtpConfigResult ftpConfigLoad(const char* path, FtpConfig* config);
const char* ftpConfigResultName(FtpConfigResult result);

#endif
