#include "ftp_vfs_sd.h"

#include "ftp_path.h"
#include "ftp_server.h"
#include "search.h"
#include "ftpsrv_vfs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <switch/runtime/devices/fs_dev.h>

#define FTP_NATIVE_WRITE_CHUNK (1024 * 1024)

/* -------------------------------------------------------------------------------------------
   Files above 4 GiB.

   FAT32 cannot describe a file of 4 GiB or more -- the directory entry's size field is 32 bits --
   and this console's `fs` enforces that on writes: measured on hardware, appending to a plain
   file stops at exactly 4 GiB (4,290,772,992 + one 4 MiB chunk = 4,294,967,296) and
   `fsFileSetSize(4.5 GB)` fails outright.  What the platform supports instead is the
   *concatenation file*; libnx documents the query:

       fsFsSetConcatenationFileAttribute(fs, path):
           "Uses fsFsQueryEntry to set the archive bit on the specified absolute directory path.
            This will cause HOS to treat the directory as if it were a file containing the
            directory's concatenated contents."

   So a big file is a directory of numbered parts that every program reads as one file.  That is
   how the console's own content storage writes placeholders -- Atmosphere's ncm calls
   `fs::CreateFile(path, size, fs::CreateOption_BigFile)`, with the size known up front -- and it
   is the layout the PC-side splitting tools produce: parts of 0xFFFF0000 bytes named 00, 01, ...
   Every such file measured on a console -- the system's own 4,650,239,771-byte package and this
   server's 6,694,728,444-byte upload (00 = 4,294,901,760, 01 = 2,399,826,684) -- splits at
   exactly that size.

   An FTP server learns the size only when the transfer is over, so this adapter starts with a
   plain file -- what every ordinary upload needs, unchanged -- and converts it *as metadata* the
   moment the next write would cross the limit:

       1. close the handle, rename the file aside      <name>            -> <name>.bigfiletmp
                                                       (the console's rename refuses an existing
                                                       destination: a temp file a conversion that
                                                       lost power left behind is deleted and the
                                                       rename retried once, so one power cut does
                                                       not cost this name its place above the
                                                       ceiling)
       2. create the directory it has to become        <name>
       3. move the file in as its first part           <name>.bigfiletmp -> <name>/00
       4. set the archive bit on that directory        fsFsSetConcatenationFileAttribute
       5. re-read the parent directory                 the entry's type and size are served from
                                                       a cached directory read: the platform's
                                                       own repair sets the same flag and then
                                                       re-reads the whole directory (ncm's
                                                       TraverseDirectory), and a stale entry is
                                                       why an earlier probe read size 0 here
       6. reopen the path and keep writing             now one file of any size: the filesystem
                                                       adds 01, 02, ... by itself, so a 15 GB
                                                       upload needs nothing more from us

   The console's own content tools also commit the filesystem after creating a file.  That call is
   deliberately not made here: it sits on the path a working 6,694,728,444-byte upload already
   exercised, and nothing observed so far needs it, so it stays out until a build that makes it
   can be measured against this one.

   No byte of the upload is copied: renames and one query.  If any step fails the file is put back
   the way it was and the write reports EFBIG -- "this card cannot hold a file this big" -- rather
   than a transfer that dies silently at 91%.
   ------------------------------------------------------------------------------------------- */
#define FTP_CONCATENATION_TEMP_SUFFIX ".bigfiletmp"
/* The plain-file limit is the 4 GiB mark itself, and the write path pre-allocates in
   FTP_NATIVE_WRITE_CHUNK steps rounded *up*: a write is therefore already too late once its
   allocation would reach 4 GiB.  Measured on hardware: a plain upload stopped at 4,293,918,264
   (0xFFEFFE38) -- the write that failed was the first whose rounded-up allocation, 0x100000000,
   reached the mark.

   Overridable at build time (`make DEFINES=-DFTP_VFS_PLAIN_FILE_MAX=0x03FFFFFF`) so the
   conversion -- the one part of this that is new behaviour -- can be exercised on hardware with
   a small file instead of moving 4 GiB over Wi-Fi.  Everything below the threshold is the code
   that is already in use, so the override changes only *when* the conversion happens. */
#ifndef FTP_VFS_PLAIN_FILE_MAX
#define FTP_VFS_PLAIN_FILE_MAX 0xFFFFFFFFLL
#endif
#define FTP_PLAIN_FILE_MAX FTP_VFS_PLAIN_FILE_MAX

static bool ftpVfsAtPlainLimit(s64 position)
{
    if (position < 0)
        return true;
    const s64 allocation = (position + FTP_NATIVE_WRITE_CHUNK - 1)
        & ~((s64)FTP_NATIVE_WRITE_CHUNK - 1);
    return allocation > FTP_PLAIN_FILE_MAX;
}

/* What the conversion needs and `struct FtpVfsFile` cannot hold: the path and the filesystem of
   the file being written.  The struct belongs to the FTP core (`ftpsrv.c` embeds it in its
   session array), and a transfer that never goes near the ceiling must behave *exactly* as it
   did before this feature existed -- so the state lives here, off to the side, and everything
   below the ceiling is the original code path with nothing added to it.

   The FTP server runs at most FTP_MAX_SESSIONS transfers at once; the table is twice that and a
   full table only means "cannot convert" (reported as EFBIG), never a broken write. */
#define FTP_VFS_STATE_SLOTS 8
struct FtpVfsState {
    struct FtpVfsFile* key; /* NULL = free */
    FsFileSystem* fileSystem;
    char path[FS_MAX_PATH];
    bool concatenation;
    bool truncatedOpen; /* the target was opened in the truncating STOR mode */
};
static struct FtpVfsState g_vfsState[FTP_VFS_STATE_SLOTS];

static struct FtpVfsState* ftpVfsStateFind(struct FtpVfsFile* file)
{
    for (size_t i = 0; i < FTP_VFS_STATE_SLOTS; i++) {
        if (g_vfsState[i].key == file)
            return &g_vfsState[i];
    }
    return NULL;
}

static struct FtpVfsState* ftpVfsStateStore(struct FtpVfsFile* file, FsFileSystem* fileSystem,
    const char* nativePath)
{
    for (size_t i = 0; i < FTP_VFS_STATE_SLOTS; i++) {
        if (g_vfsState[i].key == NULL || g_vfsState[i].key == file) {
            struct FtpVfsState* state = &g_vfsState[i];
            state->key = file;
            state->fileSystem = fileSystem;
            state->concatenation = false;
            state->truncatedOpen = false;
            const int written = snprintf(state->path, sizeof(state->path), "%s", nativePath);
            if (written <= 0 || written >= (int)sizeof(state->path)) {
                state->key = NULL;
                return NULL;
            }
            return state;
        }
    }
    return NULL;
}

static void ftpVfsStateRelease(struct FtpVfsFile* file)
{
    struct FtpVfsState* state = ftpVfsStateFind(file);
    if (state != NULL)
        state->key = NULL;
}

static int setFsError(Result result);

/* Only a concatenation file has parts to open: for a plain file this fails, and asking whether
   the path itself opens as a directory answers "no" for both forms (measured), so this is the
   discriminator that works in both directions. */
static bool ftpVfsIsConcatenationFile(FsFileSystem* fileSystem, const char* nativePath)
{
    char part_path[FS_MAX_PATH + 4];
    const int written = snprintf(part_path, sizeof(part_path), "%s/00", nativePath);
    if (written <= 0 || written >= (int)sizeof(part_path))
        return false;
    FsFile part;
    if (R_FAILED(fsFsOpenFile(fileSystem, part_path, FsOpenMode_Read, &part)))
        return false;
    fsFileClose(&part);
    return true;
}

/* Setting the archive bit changes what the directory entry *is*, and the filesystem serves the
   new type and size to a fresh enumeration -- the same reason the platform's repair path asks its
   traversal to read the directory again after calling the query. */
static void ftpVfsRefreshParentDirectory(FsFileSystem* fileSystem, const char* nativePath)
{
    const char* slash = strrchr(nativePath, '/');
    if (slash == NULL || slash == nativePath)
        return;
    char parent[FS_MAX_PATH];
    const size_t parent_len = (size_t)(slash - nativePath);
    if (parent_len == 0 || parent_len >= sizeof(parent))
        return;
    memcpy(parent, nativePath, parent_len);
    parent[parent_len] = '\0';

    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(fileSystem, parent,
            FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir)))
        return;
    FsDirectoryEntry entry;
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, 1, &entry)) && count == 1) {
        /* Walking the directory is the refresh; nothing here reads the entries. */
    }
    fsDirClose(&dir);
}

/* Put the file back the way it was before a failed conversion, so the client can still resume it
   and so a failure never leaves the data stranded inside an unmarked directory. */
static void ftpVfsUndoConversion(FsFileSystem* fileSystem, const char* path, const char* temp,
    bool part_moved, bool directory_created)
{
    if (part_moved) {
        char part_path[FS_MAX_PATH + 8];
        if (snprintf(part_path, sizeof(part_path), "%s/00", path) > 0)
            ftpServerRecordFsResult(fsFsRenameFile(fileSystem, part_path, temp));
    }
    if (directory_created)
        ftpServerRecordFsResult(fsFsDeleteDirectory(fileSystem, path));
    ftpServerRecordFsResult(fsFsRenameFile(fileSystem, temp, path));
}

/* Reopen the file we are writing.  The caller's offset is left alone on purpose: it is the
   position the client asked for (a resumed upload may sit above what is on the card), while the
   allocated size is read back so the next pre-allocation is right. */
static int ftpVfsReopenForWrite(struct FtpVfsFile* file, struct FtpVfsState* state)
{
    Result result = fsFsOpenFile(state->fileSystem, state->path, FsOpenMode_Write, &file->file);
    if (R_FAILED(result))
        return setFsError(result);
    file->open = true;
    s64 size = 0;
    if (R_SUCCEEDED(fsFileGetSize(&file->file, &size)))
        file->allocatedSize = size;
    return 0;
}

/* Turn the plain file we are writing into a concatenation file.  Metadata only. */
static int ftpVfsConvertToConcatenationFile(struct FtpVfsFile* file, struct FtpVfsState* state)
{
    FsFileSystem* fileSystem = state->fileSystem;
    const char* path = state->path;
    char temp[FS_MAX_PATH + 32];
    char part_path[FS_MAX_PATH + 8];
    if (snprintf(temp, sizeof(temp), "%s%s", path, FTP_CONCATENATION_TEMP_SUFFIX) >=
            (int)sizeof(temp) ||
        snprintf(part_path, sizeof(part_path), "%s/00", path) >= (int)sizeof(part_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* 1. The handle has to go: the file is renamed below. */
    fsFileClose(&file->file);
    file->open = false;

    Result result = fsFsRenameFile(fileSystem, path, temp);
    if (R_FAILED(result)) {
        /* The usual reason for a refused rename is the temp file of a conversion that lost power,
           which this name then cannot get past again.  It holds what the interrupted upload had
           stored, which the client is re-sending, so clearing it costs nothing and the rename gets
           one more try -- the name keeps working without anyone having to know about it.  This is
           the adapter's own temp name; a file a user parked at exactly `<name>.bigfiletmp` goes
           with it, and a directory at that name cannot be removed by this call (measured), so it
           survives and the failure below is reported instead. */
        const Result removed = fsFsDeleteFile(fileSystem, temp);
        ftpServerRecordFsResult(removed);
        if (R_SUCCEEDED(removed))
            result = fsFsRenameFile(fileSystem, path, temp);
    }
    ftpServerRecordFsResult(result);
    if (R_FAILED(result)) {
        /* Even the cleanup could not free the name (or the rename failed for another reason): the
           file goes back to being a plain file and the client sees EFBIG with the failing call in
           `ftpStatus`'s `lastFsResult`.  A leftover that survives this is removed by hand --
           `DELE <name>.bigfiletmp` from any FTP client -- and the name works again. */
        errno = EFBIG;
        goto reopen;
    }

    /* 2. + 3. The directory the file becomes, with the file itself as part 00. */
    result = fsFsCreateDirectory(fileSystem, path);
    ftpServerRecordFsResult(result);
    if (R_FAILED(result)) {
        fsFsRenameFile(fileSystem, temp, path);
        errno = EFBIG;
        goto reopen;
    }
    result = fsFsRenameFile(fileSystem, temp, part_path);
    ftpServerRecordFsResult(result);
    if (R_FAILED(result)) {
        ftpVfsUndoConversion(fileSystem, path, temp, false, true);
        errno = EFBIG;
        goto reopen;
    }

    /* 4. The archive bit. */
    result = fsFsSetConcatenationFileAttribute(fileSystem, path);
    ftpServerRecordFsResult(result);
    if (R_FAILED(result)) {
        ftpVfsUndoConversion(fileSystem, path, temp, true, true);
        errno = EFBIG;
        goto reopen;
    }

    ftpVfsRefreshParentDirectory(fileSystem, path);
    state->concatenation = true;

    /* 6. Same file, no ceiling. */
    if (ftpVfsReopenForWrite(file, state) != 0) {
        /* The data is safe (it is a valid concatenation file), only this transfer cannot go on. */
        errno = EFBIG;
        return -1;
    }
    return 0;

reopen:
    /* The file is back to being a plain file; reopening it lets the caller report the failure and
       lets the client resume the upload later. */
    ftpVfsReopenForWrite(file, state);
    return -1;
}

/* The write itself: this is the original function, byte for byte, so an upload that stays below
   the ceiling takes exactly the path it always took -- pre-allocate in 1 MiB steps, write, count
   the bytes.  Only the two failure branches know about the ceiling, and only when there is a
   state entry to convert with (which there is for every open file). */
static int ftpVfsWriteChunk(struct FtpVfsFile* file, const void* buffer, size_t size)
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
        /* A target the console already reads as one big file (a concatenation file) has to be
           removed before anything truncates it.  A WRITE-mode open truncates the file, which takes
           a concatenation file's parts away with it and leaves the next ceiling check unable to
           recognise the shape -- that is exactly how an earlier build ended up with parts nested
           inside parts ("<name>/00/00").  Deleting it is also what "overwrite" means to the
           player, and it leaves a plain file behind. */
        if (ftpVfsIsConcatenationFile(fileSystem, nativePath))
            fsFsDeleteFile(fileSystem, nativePath);
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

    /* Remember where this file lives, for the day it outgrows the ceiling.  No filesystem call
       happens here: whether the target is already a concatenation file is asked when it matters
       (at the ceiling), so nothing about an ordinary upload changes. */
    struct FtpVfsState* state = ftpVfsStateStore(file, fileSystem, nativePath);
    if (state != NULL)
        state->truncatedOpen = (mode == FtpVfsOpenMode_WRITE);
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
    struct FtpVfsState* state = ftpVfsStateFind(file);
    const s64 required = file->offset + (s64)size;
    /* Away from the limit this is the original write path, unchanged: no conversion state is
       consulted, so an ordinary upload cannot be affected by this feature. */
    if (state == NULL || (!state->concatenation && !ftpVfsAtPlainLimit(required)))
        return ftpVfsWriteChunk(file, buffer, size);

    const u8* bytes = buffer;
    size_t done = 0;
    while (done < size) {
        const s64 want = file->offset + (s64)(size - done);
        /* A plain file cannot grow past the limit, so the file is converted *before* the write that
           would reach it -- a step early, because the pre-allocation rounds up.  A file that is
           already a concatenation file needs no conversion, which is what a resumed transfer
           sees; the first part then ends just under the limit and stays a legal part. */
        if (!state->concatenation && ftpVfsAtPlainLimit(want)) {
            state->concatenation = ftpVfsIsConcatenationFile(state->fileSystem, state->path);
            if (!state->concatenation && ftpVfsConvertToConcatenationFile(file, state) != 0)
                return -1;
            continue;
        }
        const int written = ftpVfsWriteChunk(file, bytes + done, size - done);
        if (written < 0) {
            /* The limit can also arrive as a refused pre-allocation one step early; converting and
               retrying the same bytes is what turns that into a working upload.  Anywhere else the
               failure is a real one and belongs to the caller. */
            if (state->concatenation || !ftpVfsAtPlainLimit(want) ||
                ftpVfsConvertToConcatenationFile(file, state) != 0)
                return -1;
            continue;
        }
        done += (size_t)written;
    }
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
    /* Refuse a resumed *upload* instead of serving it.  This core opens `STOR` with
       FtpVfsOpenMode_WRITE ("create and truncate is implicitly implied"), so honouring the REST
       marker would restart the file at the marker and silently drop everything before it -- the
       client would see a successful transfer of a corrupt file.  A refusal it can see is worth
       more: the client restarts the file from the beginning.  Reads (`RETR` with REST) resume as
       before, and `APPE` (which never truncates) is untouched. */
    const struct FtpVfsState* state = ftpVfsStateFind(file);
    if (state != NULL && state->truncatedOpen && offset != 0) {
        errno = ENOTSUP;
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
    ftpVfsStateRelease(file);
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
        if (type == FsDirEntryType_File) {
            result = fsFsRenameFile(sourceFs, nativeSource, nativeDestination);
            /* FTP RNTO replaces an existing file, but the console FS refuses to
             * rename onto an existing path. Only after a failed rename, and only
             * when a file destination really exists, drop it and retry -- so a
             * "source missing" failure can never delete the destination. */
            if (R_FAILED(result)) {
                FsDirEntryType destinationType;
                if (R_SUCCEEDED(fsFsGetEntryType(sourceFs, nativeDestination, &destinationType))
                    && destinationType == FsDirEntryType_File) {
                    Result removed = fsFsDeleteFile(sourceFs, nativeDestination);
                    if (R_SUCCEEDED(removed))
                        result = fsFsRenameFile(sourceFs, nativeSource, nativeDestination);
                }
            }
        } else {
            result = fsFsRenameDirectory(sourceFs, nativeSource, nativeDestination);
        }
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
