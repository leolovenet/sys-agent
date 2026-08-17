#include "ftp_path.h"

#include <string.h>

bool ftpPathResolve(const char* ftpPath, char* output, size_t outputSize)
{
    if (!ftpPath || !output || ftpPath[0] != '/' || strchr(ftpPath, ':'))
        return false;

    const char* segment = ftpPath + 1;
    while (*segment) {
        const char* end = strchr(segment, '/');
        size_t length = end ? (size_t)(end - segment) : strlen(segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.')
            return false;
        segment = end ? end + 1 : segment + length;
    }

    size_t prefixLength = strlen(FTP_SD_PREFIX);
    size_t pathLength = strlen(ftpPath);
    if (prefixLength + pathLength + 1 > outputSize)
        return false;
    memcpy(output, FTP_SD_PREFIX, prefixLength);
    memcpy(output + prefixLength, ftpPath, pathLength + 1);
    return true;
}

bool ftpPathIsSearchStorage(const char* ftpPath)
{
    if (!ftpPath)
        return false;
    size_t length = strlen(FTP_SEARCH_PATH);
    return !strncmp(ftpPath, FTP_SEARCH_PATH, length)
        && (ftpPath[length] == 0 || ftpPath[length] == '/');
}

bool ftpPathCanMutate(const char* ftpPath, bool searchActive)
{
    return !searchActive || !ftpPathIsSearchStorage(ftpPath);
}
