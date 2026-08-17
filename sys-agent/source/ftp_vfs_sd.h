#ifndef SYSAGENT_FTP_VFS_SD_H
#define SYSAGENT_FTP_VFS_SD_H

#include <stdbool.h>
#include <switch.h>

struct FtpVfsFile {
    FsFile file;
    s64 offset;
    s64 allocatedSize;
    bool open;
    bool writable;
};
struct FtpVfsDir { FsDir dir; bool open; };
struct FtpVfsDirEntry { FsDirectoryEntry entry; };

#endif
