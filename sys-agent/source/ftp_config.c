#include "ftp_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FTP_CONFIG_FILE_LIMIT 4096

static char* trim(char* value)
{
    while (isspace((unsigned char)*value))
        value++;
    char* end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
        *--end = 0;
    return value;
}

static bool parseBool(const char* value, bool* output)
{
    if (!strcmp(value, "1") || !strcmp(value, "true")) {
        *output = true;
        return true;
    }
    if (!strcmp(value, "0") || !strcmp(value, "false")) {
        *output = false;
        return true;
    }
    return false;
}

static bool copyCredential(char* output, const char* value)
{
    size_t length = strlen(value);
    if (length >= FTP_CREDENTIAL_SIZE)
        return false;
    if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
        value++;
        length -= 2;
    }
    if (length >= FTP_CREDENTIAL_SIZE)
        return false;
    memcpy(output, value, length);
    output[length] = 0;
    return true;
}

void ftpConfigDefaults(FtpConfig* config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->port = FTP_DEFAULT_PORT;
    config->anonymous = true;
    config->timeout = FTP_DEFAULT_TIMEOUT;
    config->use_localtime = true;
}

FtpConfigResult ftpConfigParseText(const char* text, FtpConfig* config)
{
    if (!text || !config)
        return FtpConfigInvalidValue;

    ftpConfigDefaults(config);
    char* copy = malloc(strlen(text) + 1);
    if (!copy)
        return FtpConfigIoError;
    strcpy(copy, text);

    bool inFtpSection = false;
    FtpConfigResult result = FtpConfigOk;
    char* save = NULL;
    for (char* line = strtok_r(copy, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        line = trim(line);
        if (!*line || *line == '#' || *line == ';')
            continue;
        if (*line == '[') {
            char* close = strchr(line, ']');
            if (!close) {
                result = FtpConfigInvalidValue;
                break;
            }
            *close = 0;
            inFtpSection = !strcmp(trim(line + 1), "ftp");
            continue;
        }
        if (!inFtpSection)
            continue;

        char* equals = strchr(line, '=');
        if (!equals) {
            result = FtpConfigInvalidValue;
            break;
        }
        *equals = 0;
        char* key = trim(line);
        char* value = trim(equals + 1);

        if (!strcmp(key, "enabled")) {
            if (!parseBool(value, &config->enabled)) result = FtpConfigInvalidValue;
        } else if (!strcmp(key, "anonymous")) {
            if (!parseBool(value, &config->anonymous)) result = FtpConfigInvalidValue;
        } else if (!strcmp(key, "port")) {
            char* end = NULL;
            errno = 0;
            unsigned long port = strtoul(value, &end, 10);
            if (errno || !*value || *trim(end) || port == 0 || port > UINT16_MAX)
                result = FtpConfigInvalidValue;
            else
                config->port = (uint16_t)port;
        } else if (!strcmp(key, "timeout")) {
            char* end = NULL;
            errno = 0;
            unsigned long timeout = strtoul(value, &end, 10);
            if (errno || !*value || *trim(end) || timeout > UINT32_MAX)
                result = FtpConfigInvalidValue;
            else
                config->timeout = (uint32_t)timeout;
        } else if (!strcmp(key, "use_localtime")) {
            if (!parseBool(value, &config->use_localtime)) result = FtpConfigInvalidValue;
        } else if (!strcmp(key, "username")) {
            if (!copyCredential(config->username, value)) result = FtpConfigInvalidValue;
        } else if (!strcmp(key, "password")) {
            if (!copyCredential(config->password, value)) result = FtpConfigInvalidValue;
        }
        if (result != FtpConfigOk)
            break;
    }

    free(copy);
    if (result == FtpConfigOk && !config->anonymous
        && (!config->username[0] || !config->password[0]))
        result = FtpConfigCredentialsRequired;
    return result;
}

FtpConfigResult ftpConfigLoad(const char* path, FtpConfig* config)
{
    FILE* file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            ftpConfigDefaults(config);
            return FtpConfigOk;
        }
        return FtpConfigIoError;
    }

    char buffer[FTP_CONFIG_FILE_LIMIT + 1];
    size_t size = fread(buffer, 1, FTP_CONFIG_FILE_LIMIT + 1, file);
    bool failed = ferror(file) || size > FTP_CONFIG_FILE_LIMIT;
    fclose(file);
    if (failed)
        return FtpConfigIoError;
    buffer[size] = 0;
    return ftpConfigParseText(buffer, config);
}

const char* ftpConfigResultName(FtpConfigResult result)
{
    switch (result) {
    case FtpConfigOk: return "OK";
    case FtpConfigInvalidValue: return "invalid_value";
    case FtpConfigCredentialsRequired: return "credentials_required";
    case FtpConfigIoError: return "io_error";
    default: return "unknown";
    }
}
