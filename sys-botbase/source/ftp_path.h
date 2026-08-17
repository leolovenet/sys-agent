#ifndef SYSBOT_FTP_PATH_H
#define SYSBOT_FTP_PATH_H

#include <stdbool.h>
#include <stddef.h>

#define FTP_SD_PREFIX "sdmc:"
#define FTP_SEARCH_PATH "/switch/sys-botbase/search"

bool ftpPathResolve(const char* ftpPath, char* output, size_t outputSize);
bool ftpPathIsSearchStorage(const char* ftpPath);
bool ftpPathCanMutate(const char* ftpPath, bool searchActive);

#endif
