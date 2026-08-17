#include "ftp_vfs_sd.h"

#include "ftp_path.h"
#include "ftp_server.h"
#include "search.h"
#include "ftpsrv_vfs.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <switch/runtime/devices/fs_dev.h>

#define FTP_NATIVE_WRITE_CHUNK (1024 * 1024)

static int setFsError(Result result)
{
    ftpServerRecordFsResult(result);
    switch (result) {
    case 0x202: errno = ENOENT; break;
    case 0x402: errno = EEXIST; break;
    case 0xE02: errno = EBUSY; break;
    case 0x4E02: errno = ENOSPC; break;
    case 0x2EE602: errno = ENAMETOOLONG; break;
    default: errno = EIO; break;
    }
    return -1;
}

static bool translate(const char* path, FsFileSystem** fileSystem, char nativePath[FS_MAX_PATH])
{
    char resolved[FS_MAX_PATH + sizeof(FTP_SD_PREFIX)];
    if (!ftpPathResolve(path, resolved, sizeof(resolved))) {
        errno = EACCES;
        return false;
    }
    return fsdevTranslatePath(resolved, fileSystem, nativePath) == 0;
}

static bool mutationAllowed(const char* path)
{
    if (!ftpPathCanMutate(path, searchIsActive())) {
        errno = EBUSY;
        return false;
    }
    return true;
}

static void fillFileStatus(FsFileSystem* fileSystem, FsFile* file, const char* nativePath,
    struct stat* status)
{
    memset(status, 0, sizeof(*status));
    s64 size = 0;
    fsFileGetSize(file, &size);
    status->st_nlink = 1;
    status->st_size = size;
    status->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    FsTimeStampRaw timestamp;
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fileSystem, nativePath, &timestamp))
        && timestamp.is_valid) {
        status->st_ctime = timestamp.created;
        status->st_mtime = timestamp.modified;
        status->st_atime = timestamp.accessed;
    }
}

int ftp_vfs_open(struct FtpVfsFile* file, const char* path, enum FtpVfsOpenMode mode)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!translate(path, &fileSystem, nativePath)
        || (mode != FtpVfsOpenMode_READ && !mutationAllowed(path)))
        return -1;

    memset(file, 0, sizeof(*file));
    u32 openMode = FsOpenMode_Read;
    if (mode != FtpVfsOpenMode_READ) {
        Result createResult = fsFsCreateFile(fileSystem, nativePath, 0, 0);
        if (R_FAILED(createResult) && createResult != 0x402)
            return setFsError(createResult);
        openMode = FsOpenMode_Write;
        file->writable = true;
    }

    Result result = fsFsOpenFile(fileSystem, nativePath, openMode, &file->file);
    if (R_FAILED(result))
        return setFsError(result);
    file->open = true;

    if (mode == FtpVfsOpenMode_WRITE) {
        result = fsFileSetSize(&file->file, 0);
    } else if (mode == FtpVfsOpenMode_APPEND) {
        result = fsFileGetSize(&file->file, &file->offset);
        file->allocatedSize = file->offset;
    }
    if (R_FAILED(result)) {
        fsFileClose(&file->file);
        file->open = false;
        return setFsError(result);
    }

    ftpServerTransferOpened();
    ftpServerRecordFsResult(0);
    return 0;
}

int ftp_vfs_read(struct FtpVfsFile* file, void* buffer, size_t size)
{
    u64 read = 0;
    Result result = fsFileRead(&file->file, file->offset, buffer, size, FsReadOption_None, &read);
    if (R_FAILED(result))
        return setFsError(result);
    file->offset += read;
    ftpServerBytesSent(read);
    return (int)read;
}

int ftp_vfs_write(struct FtpVfsFile* file, const void* buffer, size_t size)
{
    s64 required = file->offset + (s64)size;
    if (required < file->offset) {
        errno = EOVERFLOW;
        return -1;
    }
    if (required > file->allocatedSize) {
        s64 allocation = (required + FTP_NATIVE_WRITE_CHUNK - 1)
            & ~((s64)FTP_NATIVE_WRITE_CHUNK - 1);
        Result result = fsFileSetSize(&file->file, allocation);
        if (R_FAILED(result))
            return setFsError(result);
        file->allocatedSize = allocation;
    }
    Result result = fsFileWrite(&file->file, file->offset, buffer, size, FsWriteOption_None);
    if (R_FAILED(result))
        return setFsError(result);
    file->offset = required;
    ftpServerBytesReceived(size);
    return (int)size;
}

int ftp_vfs_seek(struct FtpVfsFile* file, const void* buffer, size_t size, size_t offset)
{
    (void)buffer;
    (void)size;
    if (offset > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    file->offset = (s64)offset;
    return 0;
}

int ftp_vfs_close(struct FtpVfsFile* file)
{
    if (!file->open)
        return -1;
    if (file->writable) {
        fsFileSetSize(&file->file, file->offset);
        fsFileFlush(&file->file);
    }
    fsFileClose(&file->file);
    file->open = false;
    ftpServerTransferClosed();
    return 0;
}

int ftp_vfs_isfile_open(struct FtpVfsFile* file) { return file->open; }
int ftp_vfs_isfile_ready(struct FtpVfsFile* file) { return file->open; }

int ftp_vfs_opendir(struct FtpVfsDir* directory, const char* path)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!translate(path, &fileSystem, nativePath))
        return -1;
    memset(directory, 0, sizeof(*directory));
    Result result = fsFsOpenDirectory(fileSystem, nativePath,
        FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &directory->dir);
    if (R_FAILED(result))
        return setFsError(result);
    directory->open = true;
    return 0;
}

const char* ftp_vfs_readdir(struct FtpVfsDir* directory, struct FtpVfsDirEntry* entry)
{
    s64 count = 0;
    Result result = fsDirRead(&directory->dir, &count, 1, &entry->entry);
    if (R_FAILED(result)) {
        setFsError(result);
        return NULL;
    }
    return count == 1 ? entry->entry.name : NULL;
}

int ftp_vfs_dirlstat(struct FtpVfsDir* directory, const struct FtpVfsDirEntry* entry,
    const char* path, struct stat* status)
{
    (void)directory;
    memset(status, 0, sizeof(*status));
    status->st_nlink = 1;
    if (entry->entry.type == FsDirEntryType_Dir) {
        status->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        return 0;
    }
    status->st_size = entry->entry.file_size;
    status->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (translate(path, &fileSystem, nativePath)) {
        FsTimeStampRaw timestamp;
        if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fileSystem, nativePath, &timestamp))
            && timestamp.is_valid) {
            status->st_ctime = timestamp.created;
            status->st_mtime = timestamp.modified;
            status->st_atime = timestamp.accessed;
        }
    }
    return 0;
}

int ftp_vfs_closedir(struct FtpVfsDir* directory)
{
    if (!directory->open)
        return 0;
    fsDirClose(&directory->dir);
    directory->open = false;
    return 0;
}

int ftp_vfs_isdir_open(struct FtpVfsDir* directory) { return directory->open; }
int ftp_vfs_isdir_ready(struct FtpVfsDir* directory) { return directory->open; }

int ftp_vfs_stat(const char* path, struct stat* status)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!translate(path, &fileSystem, nativePath))
        return -1;
    FsDirEntryType type;
    Result result = fsFsGetEntryType(fileSystem, nativePath, &type);
    if (R_FAILED(result))
        return setFsError(result);
    if (type == FsDirEntryType_Dir) {
        memset(status, 0, sizeof(*status));
        status->st_nlink = 1;
        status->st_mode = S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO;
        return 0;
    }
    FsFile file;
    result = fsFsOpenFile(fileSystem, nativePath, FsOpenMode_Read, &file);
    if (R_FAILED(result))
        return setFsError(result);
    fillFileStatus(fileSystem, &file, nativePath, status);
    fsFileClose(&file);
    return 0;
}

int ftp_vfs_lstat(const char* path, struct stat* status) { return ftp_vfs_stat(path, status); }

int ftp_vfs_mkdir(const char* path)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!mutationAllowed(path) || !translate(path, &fileSystem, nativePath))
        return -1;
    Result result = fsFsCreateDirectory(fileSystem, nativePath);
    return R_SUCCEEDED(result) ? 0 : setFsError(result);
}

int ftp_vfs_unlink(const char* path)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!mutationAllowed(path) || !translate(path, &fileSystem, nativePath))
        return -1;
    Result result = fsFsDeleteFile(fileSystem, nativePath);
    return R_SUCCEEDED(result) ? 0 : setFsError(result);
}

int ftp_vfs_rmdir(const char* path)
{
    FsFileSystem* fileSystem = NULL;
    char nativePath[FS_MAX_PATH];
    if (!mutationAllowed(path) || !translate(path, &fileSystem, nativePath))
        return -1;
    Result result = fsFsDeleteDirectory(fileSystem, nativePath);
    return R_SUCCEEDED(result) ? 0 : setFsError(result);
}

int ftp_vfs_rename(const char* source, const char* destination)
{
    FsFileSystem* sourceFs = NULL;
    FsFileSystem* destinationFs = NULL;
    char nativeSource[FS_MAX_PATH];
    char nativeDestination[FS_MAX_PATH];
    if (!mutationAllowed(source) || !mutationAllowed(destination)
        || !translate(source, &sourceFs, nativeSource)
        || !translate(destination, &destinationFs, nativeDestination))
        return -1;
    if (sourceFs != destinationFs) {
        errno = EXDEV;
        return -1;
    }
    FsDirEntryType type;
    Result result = fsFsGetEntryType(sourceFs, nativeSource, &type);
    if (R_SUCCEEDED(result)) {
        if (type == FsDirEntryType_File)
            result = fsFsRenameFile(sourceFs, nativeSource, nativeDestination);
        else
            result = fsFsRenameDirectory(sourceFs, nativeSource, nativeDestination);
    }
    return R_SUCCEEDED(result) ? 0 : setFsError(result);
}

int ftp_vfs_readlink(const char* path, char* buffer, size_t bufferLength)
{
    (void)path;
    (void)buffer;
    (void)bufferLength;
    errno = ENOTSUP;
    return -1;
}

const char* ftp_vfs_getpwuid(const struct stat* status) { (void)status; return "switch"; }
const char* ftp_vfs_getgrgid(const struct stat* status) { (void)status; return "switch"; }
