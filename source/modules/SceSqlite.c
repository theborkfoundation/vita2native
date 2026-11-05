/*
 * SceSqlite: the firmware exposes a stock SQLite C API under its own library.
 *
 * This module links the SQLite 3.11 amalgamation (the API generation the 3.60
 * firmware shipped) and exports it directly: the NID table below points at the
 * real sqlite3_* symbols, so guest code gets the complete engine instead of a
 * fail-open shim.
 *
 * The only glue is a VFS named "psp2" that resolves Vita device paths
 * ("ux0:/...") into the host root before calling the normal SceIo entry
 * points. Its file object deliberately matches the layout Sony's VFS uses
 * (`sqlite3_file` followed by an `int *` holding the SceUID), because homebrew
 * such as VitaShell copies the default VFS and wraps xOpen/xWrite/xSync.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* glibc aliases these struct stat members to macros. */
#undef st_atime
#undef st_mtime
#undef st_ctime

#include <psp2/io/fcntl.h>
#include <psp2common/kernel/iofilemgr.h>

#include "fs.h"
#include "host.h"
#include "module.h"

#include "modules/SceLibKernel.h"
#include "modules/SceSqlite.h"

#include "sqlite3.h"

/* The file layout Sony's VFS hands to guest overrides. */
typedef struct {
    sqlite3_file base;
    int *pFd;
    int fd;
} VitaSqliteFile;

static int vita_sqlite_close(sqlite3_file *file)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;

    if (handle->fd > 0)
        sceIoClose(handle->fd);
    handle->fd = 0;

    return SQLITE_OK;
}

static int vita_sqlite_read(sqlite3_file *file, void *buffer, int count, sqlite3_int64 offset)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;
    int read;

    if (sceIoLseek(handle->fd, offset, SCE_SEEK_SET) != offset)
        return SQLITE_IOERR_READ;

    read = (int)sceIoRead(handle->fd, buffer, (SceSize)count);
    if (read < 0)
        return SQLITE_IOERR_READ;
    if (read < count) {
        memset((uint8_t *)buffer + read, 0, (size_t)(count - read));
        return SQLITE_IOERR_SHORT_READ;
    }

    return SQLITE_OK;
}

static int vita_sqlite_write(sqlite3_file *file, const void *buffer, int count,
                             sqlite3_int64 offset)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;

    if (sceIoLseek(handle->fd, offset, SCE_SEEK_SET) != offset)
        return SQLITE_IOERR_WRITE;
    if ((int)sceIoWrite(handle->fd, buffer, (SceSize)count) != count)
        return SQLITE_IOERR_WRITE;

    return SQLITE_OK;
}

static int vita_sqlite_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;

    return SceLibKernel_fd_truncate(handle->fd, size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int vita_sqlite_sync(sqlite3_file *file, int flags)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;

    (void)flags;
    return sceIoSyncByFd(handle->fd, 0) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int vita_sqlite_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;
    SceOff current = sceIoLseek(handle->fd, 0, SCE_SEEK_CUR);
    SceOff end;

    if (current < 0)
        return SQLITE_IOERR_FSTAT;

    end = sceIoLseek(handle->fd, 0, SCE_SEEK_END);
    sceIoLseek(handle->fd, current, SCE_SEEK_SET);
    if (end < 0)
        return SQLITE_IOERR_FSTAT;

    *size = (sqlite3_int64)end;
    return SQLITE_OK;
}

static int vita_sqlite_lock(sqlite3_file *file, int level)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;

    switch (level) {
    case SQLITE_LOCK_NONE:
        return SceLibKernel_fd_setlock(handle->fd, -1, 0) == 0 ? SQLITE_OK : SQLITE_IOERR_UNLOCK;
    case SQLITE_LOCK_SHARED:
        return SceLibKernel_fd_setlock(handle->fd, 0, 1) == 0 ? SQLITE_OK : SQLITE_IOERR_LOCK;
    default:
        return SceLibKernel_fd_setlock(handle->fd, 1, 1) == 0 ? SQLITE_OK : SQLITE_IOERR_LOCK;
    }
}

static int vita_sqlite_unlock(sqlite3_file *file, int level)
{
    return vita_sqlite_lock(file, level == SQLITE_LOCK_NONE ? SQLITE_LOCK_NONE : level);
}

static int vita_sqlite_check_reserved_lock(sqlite3_file *file, int *reserved)
{
    (void)file;

    if (reserved)
        *reserved = 0;
    return SQLITE_OK;
}

static int vita_sqlite_file_control(sqlite3_file *file, int op, void *arg)
{
    (void)file;
    (void)op;
    (void)arg;

    return SQLITE_NOTFOUND;
}

static int vita_sqlite_sector_size(sqlite3_file *file)
{
    (void)file;
    return 4096;
}

static int vita_sqlite_device_characteristics(sqlite3_file *file)
{
    (void)file;
    return SQLITE_IOCAP_ATOMIC | SQLITE_IOCAP_SAFE_APPEND;
}

static const sqlite3_io_methods g_vita_io_methods = {
    .iVersion = 2,
    .xClose = vita_sqlite_close,
    .xRead = vita_sqlite_read,
    .xWrite = vita_sqlite_write,
    .xTruncate = vita_sqlite_truncate,
    .xSync = vita_sqlite_sync,
    .xFileSize = vita_sqlite_file_size,
    .xLock = vita_sqlite_lock,
    .xUnlock = vita_sqlite_unlock,
    .xCheckReservedLock = vita_sqlite_check_reserved_lock,
    .xFileControl = vita_sqlite_file_control,
    .xSectorSize = vita_sqlite_sector_size,
    .xDeviceCharacteristics = vita_sqlite_device_characteristics,
};

static int vita_sqlite_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags,
                            int *out_flags)
{
    VitaSqliteFile *handle = (VitaSqliteFile *)file;
    int sce_flags;
    SceUID fd;

    (void)vfs;

    if (!name)
        return SQLITE_CANTOPEN;

    if (flags & SQLITE_OPEN_READONLY)
        sce_flags = SCE_O_RDONLY;
    else
        sce_flags = SCE_O_RDWR;
    if (flags & SQLITE_OPEN_CREATE)
        sce_flags |= SCE_O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE)
        sce_flags |= SCE_O_EXCL;

    fd = sceIoOpen(name, sce_flags, 0777);
    if (fd < 0)
        return SQLITE_CANTOPEN;

    memset(handle, 0, sizeof(*handle));
    handle->fd = fd;
    handle->pFd = &handle->fd;
    handle->base.pMethods = &g_vita_io_methods;

    if (out_flags)
        *out_flags = flags & (SQLITE_OPEN_READONLY | SQLITE_OPEN_READWRITE);

    return SQLITE_OK;
}

static int vita_sqlite_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    char path[1024];

    (void)vfs;
    (void)sync_dir;

    if (!name || host_fs_resolve_path(name, path, sizeof(path)) < 0)
        return SQLITE_IOERR_DELETE;

    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "sqlite vfs delete: %s -> %s\n", path, strerror(errno));
        return SQLITE_IOERR_DELETE;
    }
    return SQLITE_OK;
}

static int vita_sqlite_access(sqlite3_vfs *vfs, const char *name, int flags, int *exists)
{
    char path[1024];
    int mode;

    (void)vfs;

    if (!name || host_fs_resolve_path(name, path, sizeof(path)) < 0) {
        *exists = 0;
        return SQLITE_OK;
    }

    switch (flags) {
    case SQLITE_ACCESS_READ:
        mode = R_OK;
        break;
    case SQLITE_ACCESS_READWRITE:
        mode = R_OK | W_OK;
        break;
    default:
        mode = F_OK;
        break;
    }

    *exists = access(path, mode) == 0;
    return SQLITE_OK;
}

static int vita_sqlite_full_pathname(sqlite3_vfs *vfs, const char *name, int out_size, char *out)
{
    (void)vfs;

    if (!name || out_size <= 0)
        return SQLITE_CANTOPEN;

    snprintf(out, (size_t)out_size, "%s", name);
    return SQLITE_OK;
}

static void *vita_sqlite_dl_open(sqlite3_vfs *vfs, const char *filename)
{
    (void)vfs;
    (void)filename;
    return NULL;
}

static void vita_sqlite_dl_error(sqlite3_vfs *vfs, int size, char *message)
{
    (void)vfs;

    if (message && size > 0)
        snprintf(message, (size_t)size, "dynamic extensions are not supported");
}

static void (*vita_sqlite_dl_sym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void)
{
    (void)vfs;
    (void)handle;
    (void)symbol;
    return NULL;
}

static void vita_sqlite_dl_close(sqlite3_vfs *vfs, void *handle)
{
    (void)vfs;
    (void)handle;
}

static int vita_sqlite_randomness(sqlite3_vfs *vfs, int size, char *buffer)
{
    FILE *random;

    (void)vfs;

    random = fopen("/dev/urandom", "rb");
    if (random) {
        size_t got = fread(buffer, 1, (size_t)size, random);

        fclose(random);
        if ((int)got == size)
            return size;
    }

    for (int i = 0; i < size; i++)
        buffer[i] = (char)(rand() & 0xff);
    return size;
}

static int vita_sqlite_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;

    host_sleep_ns((int64_t)microseconds * 1000);
    return microseconds;
}

static int vita_sqlite_current_time(sqlite3_vfs *vfs, double *time)
{
    struct timeval tv;

    (void)vfs;

    gettimeofday(&tv, NULL);
    *time = 2440587.5 + ((double)tv.tv_sec + (double)tv.tv_usec / 1000000.0) / 86400.0;
    return SQLITE_OK;
}

static int vita_sqlite_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *time)
{
    double julian;

    vita_sqlite_current_time(vfs, &julian);
    *time = (sqlite3_int64)(julian * 86400000.0);
    return SQLITE_OK;
}

static int vita_sqlite_last_error(sqlite3_vfs *vfs, int size, char *message)
{
    (void)vfs;
    (void)size;

    if (message && size > 0)
        message[0] = '\0';
    return 0;
}

static sqlite3_vfs g_vita_vfs = {
    .iVersion = 3,
    .szOsFile = sizeof(VitaSqliteFile),
    .mxPathname = 1024,
    .pNext = NULL,
    .zName = "psp2",
    .pAppData = NULL,
    .xOpen = vita_sqlite_open,
    .xDelete = vita_sqlite_delete,
    .xAccess = vita_sqlite_access,
    .xFullPathname = vita_sqlite_full_pathname,
    .xDlOpen = vita_sqlite_dl_open,
    .xDlError = vita_sqlite_dl_error,
    .xDlSym = vita_sqlite_dl_sym,
    .xDlClose = vita_sqlite_dl_close,
    .xRandomness = vita_sqlite_randomness,
    .xSleep = vita_sqlite_sleep,
    .xCurrentTime = vita_sqlite_current_time,
    .xGetLastError = vita_sqlite_last_error,
    .xCurrentTimeInt64 = vita_sqlite_current_time_int64,
};

static bool g_sqlite_initialized;

int SceSqlite_init(void)
{
    if (g_sqlite_initialized)
        return 0;

    if (sqlite3_initialize() != SQLITE_OK)
        return -1;

    if (sqlite3_vfs_find("psp2") == NULL)
        sqlite3_vfs_register(&g_vita_vfs, 1);

    g_sqlite_initialized = true;
    return 0;
}

int SceSqlite_finish(void)
{
    if (!g_sqlite_initialized)
        return 0;

    sqlite3_vfs_unregister(&g_vita_vfs);
    sqlite3_shutdown();
    g_sqlite_initialized = false;

    return 0;
}

/*
 * Sony's malloc-methods hook only describes three functions. The guest's
 * allocator is the host libc allocator anyway, so keep SQLite's default
 * methods rather than feeding a short struct to sqlite3_config().
 */
EXPORT(SceSqlite, 0x2034E5B7, int, sceSqliteConfigMallocMethods, const void *methods)
{
    (void)methods;
    return SQLITE_OK;
}

/*
 * The guest asks for the default VFS, copies it, and registers a wrapper. Make
 * sure our path-translating VFS is present before answering.
 */
EXPORT(SceSqlite, 0x0C6DD8C3, sqlite3_vfs *, vita_sqlite_vfs_find, const char *name)
{
    SceSqlite_init();
    return sqlite3_vfs_find(name);
}

/* Data symbols have no function export slot; keep their NIDs resolvable. */
static int sqlite_unimplemented(void)
{
    return SQLITE_ERROR;
}

#include "SceSqlite_exports.inc"

DECLARE_LIBRARY(SceSqlite, 0x67F02E1D);
