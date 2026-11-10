#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <m-dict.h>
#include <psp2/kernel/error.h>
#include <psp2common/types.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "fs.h"
#include "host.h"
#undef st_ctime
#undef st_atime
#undef st_mtime
#if defined(__APPLE__)
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#endif
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>

#include "log.h"
#include "module.h"
#include "util.h"

#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceSysmem.h"

typedef struct {
    uint32_t index;
    SceUID uid;
    FILE *fp;
    char *file;
} VitaOpenedFile;

typedef struct {
    uint32_t index;
    SceUID uid;
    DIR *dir;
    char *path;
} VitaOpenedDir;

static UEvent g_process_exit_event;
static atomic_int g_process_exit_res;

DICT_DEF2(vita_opened_files_dict, SceUID, M_DEFAULT_OPLIST, VitaOpenedFile *, M_POD_OPLIST)
static vita_opened_files_dict_t g_vita_opened_files;
static RwLock g_vita_opened_files_lock;

DICT_DEF2(vita_opened_dirs_dict, SceUID, M_DEFAULT_OPLIST, VitaOpenedDir *, M_POD_OPLIST)
static vita_opened_dirs_dict_t g_vita_opened_dirs;
static RwLock g_vita_opened_dirs_lock;

static VitaOpenedFile *get_opened_file_for_fd(SceUID fd)
{
    VitaOpenedFile *vfile;
    VitaOpenedFile **slot;

    rwlockReadLock(&g_vita_opened_files_lock);
    slot = vita_opened_files_dict_get(g_vita_opened_files, fd);
    vfile = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_opened_files_lock);

    return vfile;
}

static VitaOpenedDir *get_opened_dir_for_fd(SceUID fd)
{
    VitaOpenedDir *vdir;
    VitaOpenedDir **slot;

    rwlockReadLock(&g_vita_opened_dirs_lock);
    slot = vita_opened_dirs_dict_get(g_vita_opened_dirs, fd);
    vdir = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_opened_dirs_lock);

    return vdir;
}

/* Vita errno values are the host values with the SCE error bit set. */
static inline uint32_t sce_error_from_errno(int err)
{
    return 0x80010000u | (uint32_t)err;
}

EXPORT(SceLibKernel, 0xB295EB61, void *, sceKernelGetTLSAddr, int key)
{
    VitaThreadInfo *ti;

    if (key >= 0 && key <= 0x100) {
        ti = SceKernelThreadMgr_get_thread_info();
        return &ti->vita_tls[key];
    }

    return NULL;
}

EXPORT(SceLibKernel, 0x7595D9AA, int, sceKernelExitProcess, int res)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    LOG("sceKernelExitProcess called! Return value %d ra=%p thread=0x%x (%s) host=%p", res,
        __builtin_return_address(0), ti->uid, ti->name[0] ? ti->name : "?",
        (void *)pthread_self());

    atomic_store(&g_process_exit_res, res);
    ueventSignal(&g_process_exit_event);

    threadExit();

    return 0;
}

static const char *vita_io_open_flags_to_fopen_flags(int flags)
{
    int access = flags & (SCE_O_RDONLY | SCE_O_WRONLY);
    int read_only = access == SCE_O_RDONLY;

    if (access != SCE_O_RDONLY && access != SCE_O_WRONLY && access != SCE_O_RDWR)
        return NULL;

    if (flags & SCE_O_APPEND)
        return read_only ? "a+" : (access == SCE_O_RDWR ? "a+" : "a");

    if (flags & SCE_O_TRUNC)
        return read_only ? "w+" : (access == SCE_O_RDWR ? "w+" : "w");

    /*
     * CREAT without TRUNC must keep an existing file intact. Try to open it
     * normally first; sceIoOpen retries with a creating mode on ENOENT.
     */
    return read_only ? "r" : "r+";
}

EXPORT(SceLibKernel, 0x6C60AC61, SceUID, sceIoOpen, const char *file, int flags, SceMode mode)
{
    VitaOpenedFile *vfile;
    const char *open_flags;
    const char *host_file;
    char resolved[PATH_MAX];
    FILE *fp;

    if (host_fs_resolve_path(file, resolved, sizeof(resolved)) < 0) {
        if (getenv("VITA2NATIVE_TRACE_FILEIO"))
            LOG("sceIoOpen(\"%s\") resolve failed", file ? file : "(null)");
        return SCE_ERROR_ERRNO_ENODEV;
    }
    host_file = resolved;

    if (*host_file == '\0')
        return SCE_ERROR_ERRNO_ENODEV;

    open_flags = vita_io_open_flags_to_fopen_flags(flags);
    if (!open_flags)
        return SCE_ERROR_ERRNO_EINVAL;

    fp = fopen(host_file, open_flags);
    if (!fp && (flags & SCE_O_CREAT) && errno == ENOENT) {
        /* RDONLY|CREAT still needs a readable stream, hence w+. */
        int access = flags & (SCE_O_RDONLY | SCE_O_WRONLY);
        fp = fopen(host_file, access == SCE_O_WRONLY ? "w" : "w+");
    }
    if (!fp) {
        if (getenv("VITA2NATIVE_TRACE_FILEIO"))
            LOG("sceIoOpen(\"%s\") failed: %s (host \"%s\")", file ? file : "(null)",
                strerror(errno), host_file);
        if (file && strstr(file, "savedata") && getenv("VITA2NATIVE_INPUT_DEBUG")) {
            fprintf(stderr, "vita2native: sceIoOpen %s failed: %s (host %s)\n", file,
                    strerror(errno), host_file);
            fflush(stderr);
        }
        return sce_error_from_errno(errno);
    }

    if (file && strstr(file, "savedata") && getenv("VITA2NATIVE_INPUT_DEBUG")) {
        fprintf(stderr, "vita2native: sceIoOpen %s -> %s\n", file, host_file);
        fflush(stderr);
    }

    (void)mode;

    vfile = malloc(sizeof(*vfile));
    if (!vfile) {
        fclose(fp);
        return SCE_ERROR_ERRNO_EMFILE;
    }

    memset(vfile, 0, sizeof(*vfile));
    vfile->uid = SceSysmem_get_next_uid();
    vfile->fp = fp;
    vfile->file = strdup(host_file);

    rwlockWriteLock(&g_vita_opened_files_lock);
    vita_opened_files_dict_set_at(g_vita_opened_files, vfile->uid, vfile);
    rwlockWriteUnlock(&g_vita_opened_files_lock);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoOpen(\"%s\") -> uid=0x%x", file ? file : "(null)", vfile->uid);

    return vfile->uid;
}

EXPORT(SceIofilemgr, 0xC70B8886, int, sceIoClose, SceUID fd)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    int ret;

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    ret = fclose(vfile->fp);
    rwlockWriteLock(&g_vita_opened_files_lock);
    vita_opened_files_dict_erase(g_vita_opened_files, fd);
    rwlockWriteUnlock(&g_vita_opened_files_lock);
    free(vfile->file);
    free(vfile);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoClose(0x%x)", fd);

    if (ret)
        return SCE_ERROR_ERRNO_EBADF;

    return 0;
}

EXPORT(SceIofilemgr, 0xFDB32293, SceSSize, sceIoRead, SceUID fd, void *buf, SceSize nbyte)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    SceSSize result;

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    result = (SceSSize)fread(buf, 1, nbyte, vfile->fp);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoRead(0x%x, %u) -> %d", fd, (unsigned)nbyte, (int)result);

    return result;
}

EXPORT(SceIofilemgr, 0x34EFD876, SceSSize, sceIoWrite, SceUID fd, const void *buf, SceSize nbyte)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    return fwrite(buf, 1, nbyte, vfile->fp);
}

EXPORT(SceIofilemgr, 0x49252B9B, long, sceIoLseek32, SceUID fd, long offset, int whence)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    if (fseek(vfile->fp, offset, whence) != 0)
        return -1;

    return ftell(vfile->fp);
}

EXPORT(SceIofilemgr, 0x16512F59, int, sceIoSyncByFd, SceUID fd, int flag)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    return fflush(vfile->fp);
}

int SceLibKernel_fd_truncate(SceUID fd, int64_t size)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);

    if (!vfile)
        return -1;

    fflush(vfile->fp);
    return ftruncate(fileno(vfile->fp), (off_t)size);
}

int SceLibKernel_fd_setlock(SceUID fd, int exclusive, int wait)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    struct flock lock;

    if (!vfile)
        return -1;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = exclusive < 0 ? F_UNLCK : (exclusive ? F_WRLCK : F_RDLCK);
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    return fcntl(fileno(vfile->fp), wait ? F_SETLKW : F_SETLK, &lock);
}

EXPORT(SceLibKernel, 0xA9283DD0, SceUID, sceIoDopen, const char *dirname)
{
    VitaOpenedDir *vdir;
    const char *host_dirname;
    char resolved[PATH_MAX];
    DIR *dir;

    if (host_fs_resolve_path(dirname, resolved, sizeof(resolved)) < 0)
        return SCE_ERROR_ERRNO_ENODEV;
    host_dirname = resolved;

    dir = opendir(host_dirname);
    if (!dir) {
        if (getenv("VITA2NATIVE_TRACE_FILEIO"))
            LOG("sceIoDopen(\"%s\") failed: %s (host \"%s\")", dirname, strerror(errno),
                host_dirname);
        return SCE_ERROR_ERRNO_ENODEV;
    }

    vdir = malloc(sizeof(*vdir));
    if (!vdir) {
        closedir(dir);
        return SCE_ERROR_ERRNO_EMFILE;
    }

    memset(vdir, 0, sizeof(*vdir));
    vdir->uid = SceSysmem_get_next_uid();
    vdir->dir = dir;
    vdir->path = strdup(host_dirname);

    rwlockWriteLock(&g_vita_opened_dirs_lock);
    vita_opened_dirs_dict_set_at(g_vita_opened_dirs, vdir->uid, vdir);
    rwlockWriteUnlock(&g_vita_opened_dirs_lock);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoDopen(\"%s\") -> uid=0x%x", dirname, vdir->uid);

    return vdir->uid;
}

EXPORT(SceIofilemgr, 0x422A221A, int, sceIoDclose, SceUID fd)
{
    VitaOpenedDir *vdir = get_opened_dir_for_fd(fd);
    int ret;

    if (!vdir)
        return SCE_ERROR_ERRNO_EBADF;

    ret = closedir(vdir->dir);
    free(vdir->path);
    rwlockWriteLock(&g_vita_opened_dirs_lock);
    vita_opened_dirs_dict_erase(g_vita_opened_dirs, fd);
    rwlockWriteUnlock(&g_vita_opened_dirs_lock);
    free(vdir);

    if (ret)
        return SCE_ERROR_ERRNO_EBADF;

    return 0;
}

static inline void tm_to_sce_datetime(SceDateTime *dt, const struct tm *tm)
{
    dt->year = tm->tm_year + 1900;
    dt->month = tm->tm_mon + 1;
    dt->day = tm->tm_mday;
    dt->hour = tm->tm_hour;
    dt->minute = tm->tm_min;
    dt->second = tm->tm_sec;
    dt->microsecond = 0;
}

static void stat_to_sce_iostat(const struct stat *sb, SceIoStat *st)
{
    struct tm tm;

    memset(st, 0, sizeof(*st));

    if (S_ISDIR(sb->st_mode)) {
        st->st_mode = SCE_S_IFDIR | SCE_S_IRWXU | SCE_S_IRWXG | SCE_S_IRWXS;
        st->st_attr = SCE_SO_IFDIR;
    } else {
        st->st_mode = SCE_S_IFREG | SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRGRP | SCE_S_IWGRP |
                      SCE_S_IRSYS | SCE_S_IWSYS;
        st->st_attr = SCE_SO_IFREG;
    }

    st->st_size = sb->st_size;

    gmtime_r(&sb->st_ctim.tv_sec, &tm);
    tm_to_sce_datetime(&st->st_ctime, &tm);
    gmtime_r(&sb->st_atim.tv_sec, &tm);
    tm_to_sce_datetime(&st->st_atime, &tm);
    gmtime_r(&sb->st_mtim.tv_sec, &tm);
    tm_to_sce_datetime(&st->st_mtime, &tm);
}

/*
 * Map a Vita path to a host path. Returns NULL when the device is unknown or
 * the path would escape the configured root.
 */
static const char *host_path_from_vita(const char *vita_path, char *buffer, size_t size)
{
    if (host_fs_resolve_path(vita_path, buffer, size) < 0)
        return NULL;
    return buffer;
}

EXPORT(SceLibKernel, 0x9C8B6624, int, sceIoDread, SceUID fd, SceIoDirent *dirent)
{
    VitaOpenedDir *vdir = get_opened_dir_for_fd(fd);
    struct dirent *de;
    struct stat statbuf;
    const char *path;
    char full_path[PATH_MAX];

    if (!vdir)
        return SCE_ERROR_ERRNO_EBADF;

    /* Vita callers (VitaShell included) add their own ".." entry and recurse
       through every name returned here. The host readdir supplies ``.'' and
       ``..''; returning them would duplicate the parent row and can make a
       recursive copy/remove walk the parent tree. */
    do {
        de = readdir(vdir->dir);
        if (!de)
            return 0;
    } while (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0);

    if (strcmp(vdir->path, "/") == 0) {
        path = de->d_name;
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", vdir->path, de->d_name);
        path = full_path;
    }

    if (stat(path, &statbuf) != 0)
        return sce_error_from_errno(errno);

    stat_to_sce_iostat(&statbuf, &dirent->d_stat);
    strncpy(dirent->d_name, de->d_name, sizeof(dirent->d_name));
    dirent->d_private = NULL;

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoDread(0x%x) -> \"%s\"", fd, de->d_name);

    return 1;
}

EXPORT(SceLibKernel, 0xBCA5B623, int, sceIoGetstat, const char *file, SceIoStat *st)
{
    char resolved[PATH_MAX];
    const char *host_path;
    struct stat sb;

    if (!file || !st)
        return sce_error_from_errno(EINVAL);

    host_path = host_path_from_vita(file, resolved, sizeof(resolved));
    if (!host_path)
        return SCE_ERROR_ERRNO_ENODEV;

    if (stat(host_path, &sb) != 0)
        return sce_error_from_errno(errno);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoGetstat(\"%s\") -> size=%lld", file, (long long)sb.st_size);

    stat_to_sce_iostat(&sb, st);
    return 0;
}

EXPORT(SceLibKernel, 0x57F8CD25, int, sceIoGetstatByFd, SceUID fd, SceIoStat *st)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    struct stat sb;

    if (!st)
        return sce_error_from_errno(EINVAL);

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    if (fstat(fileno(vfile->fp), &sb) != 0)
        return sce_error_from_errno(errno);

    stat_to_sce_iostat(&sb, st);
    return 0;
}

EXPORT(SceLibKernel, 0x9670D39F, int, sceIoMkdir, const char *dir, SceMode mode)
{
    char resolved[PATH_MAX];
    const char *host_path;

    if (!dir)
        return sce_error_from_errno(EINVAL);

    host_path = host_path_from_vita(dir, resolved, sizeof(resolved));
    if (!host_path)
        return SCE_ERROR_ERRNO_ENODEV;

    if (mkdir(host_path, mode & 07777) != 0)
        return sce_error_from_errno(errno);

    return 0;
}

EXPORT(SceLibKernel, 0xE9F91EC8, int, sceIoRmdir, const char *dir)
{
    char resolved[PATH_MAX];
    const char *host_path;

    if (!dir)
        return sce_error_from_errno(EINVAL);

    host_path = host_path_from_vita(dir, resolved, sizeof(resolved));
    if (!host_path)
        return SCE_ERROR_ERRNO_ENODEV;

    if (rmdir(host_path) != 0)
        return sce_error_from_errno(errno);

    return 0;
}

EXPORT(SceLibKernel, 0xE20ED0F3, int, sceIoRemove, const char *file)
{
    char resolved[PATH_MAX];
    const char *host_path;

    if (!file)
        return sce_error_from_errno(EINVAL);

    host_path = host_path_from_vita(file, resolved, sizeof(resolved));
    if (!host_path)
        return SCE_ERROR_ERRNO_ENODEV;

    if (unlink(host_path) != 0)
        return sce_error_from_errno(errno);

    return 0;
}

EXPORT(SceLibKernel, 0xF737E369, int, sceIoRename, const char *old_path, const char *new_path)
{
    char old_resolved[PATH_MAX];
    char new_resolved[PATH_MAX];
    const char *host_old;
    const char *host_new;

    if (!old_path || !new_path)
        return sce_error_from_errno(EINVAL);

    host_old = host_path_from_vita(old_path, old_resolved, sizeof(old_resolved));
    host_new = host_path_from_vita(new_path, new_resolved, sizeof(new_resolved));
    if (!host_old || !host_new)
        return SCE_ERROR_ERRNO_ENODEV;

    if (rename(host_old, host_new) != 0)
        return sce_error_from_errno(errno);

    return 0;
}

EXPORT(SceLibKernel, 0x29482F7F, int, sceIoChstat, const char *file, SceIoStat *st, int bits)
{
    char resolved[PATH_MAX];
    const char *host_path;

    if (!file || !st)
        return sce_error_from_errno(EINVAL);

    host_path = host_path_from_vita(file, resolved, sizeof(resolved));
    if (!host_path)
        return SCE_ERROR_ERRNO_ENODEV;

    /* Only the permission bits are portable; timestamps are ignored. */
    if ((bits & SCE_CST_MODE) && chmod(host_path, st->st_mode & 07777) != 0)
        return sce_error_from_errno(errno);

    return 0;
}

EXPORT(SceLibKernel, 0x99BA173E, SceOff, sceIoLseek, SceUID fd, SceOff offset, int whence)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    if (fseeko(vfile->fp, (off_t)offset, whence) != 0)
        return -1;

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoLseek(0x%x, %lld, %d) -> %lld", fd, (long long)offset, whence,
            (long long)ftello(vfile->fp));

    return (SceOff)ftello(vfile->fp);
}

EXPORT_FUNC(SceLibKernel, 0x632980D7, memset)
EXPORT_FUNC(SceLibKernel, 0x14E9DBD7, memcpy)
EXPORT_FUNC(SceLibKernel, 0x736753C8, memmove)
EXPORT_FUNC(SceLibKernel, 0xFA26BC62, printf)

EXPORT(SceLibKernel, 0x0FB972F9, int, sceKernelGetThreadId)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();
    static int trace = -1;

    if (trace < 0)
        trace = getenv("VITA2NATIVE_TRACE_THREADID") != NULL;
    if (trace)
        LOG("sceKernelGetThreadId -> 0x%x (%s) ti=%p host=%p ra=%p", ti->uid,
            ti->name[0] ? ti->name : "?", (void *)ti, (void *)pthread_self(),
            __builtin_return_address(0));

    return ti->uid;
}

/* Unity's assert path. Log the message (the varargs are guest pointers, which
   are mapped on the host) and keep running so the failure is visible. */
EXPORT(SceDbg, 0x1AF3678B, int, sceDbgAssertionHandler, const char *filename, int line,
       int do_stop, const char *component, const char *format, ...)
{
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format ? format : "(no message)", args);
    va_end(args);

    LOG("Assertion %s:%d [%s] stop=%d: %s", filename ? filename : "?", line,
        component ? component : "?", do_stop, message);

    return 0;
}

DECLARE_LIBRARY(SceLibKernel, 0xcae9ace6);
DECLARE_LIBRARY(SceIofilemgr, 0xf2ff276e);
DECLARE_LIBRARY(SceDbg, 0x623df986);

int SceLibKernel_init(void)
{
    ueventCreate(&g_process_exit_event, false);

    vita_opened_files_dict_init(g_vita_opened_files);
    vita_opened_dirs_dict_init(g_vita_opened_dirs);
    rwlockInit(&g_vita_opened_files_lock);
    rwlockInit(&g_vita_opened_dirs_lock);

    return 0;
}

UEvent *SceLibKernel_get_process_exit_uevent(void)
{
    return &g_process_exit_event;
}

int SceLibKernel_get_process_exit_res(void)
{
    return atomic_load(&g_process_exit_res);
}

EXPORT(SceLibKernel, 0x52315AD7, SceSSize, sceIoPread, SceUID fd, void *buf, SceSize nbyte,
       SceOff offset)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    long saved;
    size_t read;

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    saved = ftell(vfile->fp);
    if (fseek(vfile->fp, (long)offset, SEEK_SET) != 0)
        return SCE_ERROR_ERRNO_EBADF;

    read = fread(buf, 1, nbyte, vfile->fp);
    if (saved >= 0)
        fseek(vfile->fp, saved, SEEK_SET);

    if (getenv("VITA2NATIVE_TRACE_FILEIO"))
        LOG("sceIoPread(0x%x, %u, %lld) -> %d", fd, (unsigned)nbyte, (long long)offset,
            (int)read);

    return (SceSSize)read;
}

EXPORT(SceLibKernel, 0x8FFFF5A8, SceSSize, sceIoPwrite, SceUID fd, const void *buf, SceSize nbyte,
       SceOff offset)
{
    VitaOpenedFile *vfile = get_opened_file_for_fd(fd);
    long saved;
    size_t written;

    if (!vfile)
        return SCE_ERROR_ERRNO_EBADF;

    saved = ftell(vfile->fp);
    if (fseek(vfile->fp, (long)offset, SEEK_SET) != 0)
        return SCE_ERROR_ERRNO_EBADF;

    written = fwrite(buf, 1, nbyte, vfile->fp);
    fflush(vfile->fp);
    if (saved >= 0)
        fseek(vfile->fp, saved, SEEK_SET);

    return (SceSSize)written;
}

EXPORT(SceLibKernel, 0x54ABACFA, int, sceIoIoctl, SceUID fd, unsigned int cmd, void *indata,
       int inlen, void *outdata, int outlen)
{
    (void)fd;
    (void)cmd;
    (void)indata;
    (void)inlen;

    if (outdata && outlen > 0)
        memset(outdata, 0, (size_t)outlen);

    return 0;
}

EXPORT(SceLibKernel, 0x04B30CB2, int, sceIoDevctl, const char *dev, unsigned int cmd,
       void *indata, int inlen, void *outdata, int outlen)
{
    (void)indata;
    (void)inlen;

    /* 0x3001 is the device-info query every file manager uses. */
    if (cmd == 0x3001 && outdata && outlen >= (int)sizeof(SceIoDevInfo)) {
        SceIoDevInfo *info = outdata;
        struct statvfs vfs;
        char path[PATH_MAX];

        if (!dev || host_fs_resolve_path(dev, path, sizeof(path)) < 0 || statvfs(path, &vfs) != 0)
            return SCE_ERROR_ERRNO_EINVAL;

        memset(info, 0, sizeof(*info));
        info->max_size = (SceOff)((uint64_t)vfs.f_blocks * vfs.f_frsize);
        info->free_size = (SceOff)((uint64_t)vfs.f_bavail * vfs.f_frsize);
        info->cluster_size = (SceSize)vfs.f_frsize;
        info->unk = NULL;
        return 0;
    }

    if (outdata && outlen > 0)
        memset(outdata, 0, (size_t)outlen);

    return 0;
}

EXPORT(SceLibKernel, 0x98ACED6D, int, sceIoSync, const char *device, unsigned int unk)
{
    (void)device;
    (void)unk;

    return 0;
}

/*
 * Diagnostics, module hooks and SBL helpers. The exact signatures of the
 * backtrace and SBL entry points are not public; they take only scalar or
 * pointer arguments, all of which arrive in registers, so ignoring them is
 * safe. Returning success without filling a buffer is preferable to guessing
 * at an output layout.
 */

EXPORT(SceLibKernel, 0x894037E8, int, sceKernelBacktrace, void *unk0, void *unk1, int unk2,
       int unk3, void *unk4)
{
    (void)unk0;
    (void)unk1;
    (void)unk2;
    (void)unk3;
    (void)unk4;
    return 0;
}

EXPORT(SceLibKernel, 0xD16C03B0, int, sceKernelBacktraceSelf, void *unk0, int unk1)
{
    (void)unk0;
    (void)unk1;
    return 0;
}

EXPORT(SceLibKernel, 0x1C02A434, int, sceKernelPrintBacktrace, void *unk0)
{
    (void)unk0;
    return 0;
}

EXPORT(SceLibKernel, 0x15E2A45D, int, sceKernelCallModuleExit, SceUID module_id)
{
    (void)module_id;
    return 0;
}

EXPORT(SceLibKernel, 0xAFE2B300, int, sceKernelCallWithChangeStack, void *stack, void *function,
       void *argument)
{
    (void)stack;
    (void)function;
    (void)argument;
    return SCE_ERROR_ERRNO_EINVAL;
}

EXPORT(SceLibKernel, 0xF8A99FDF, SceUInt32, sceKernelGetPMUSERENR, void)
{
    return 0;
}

EXPORT(SceLibKernel, 0x963F4A99, int, sceSblACMgrIsGameProgram, int *result)
{
    if (result)
        *result = 1;
    return 0;
}

EXPORT(SceLibKernel, 0x5F9F0D61, int, sceSblGcAuthMgrAdhocBB160Shutdown, void)
{
    return SCE_ERROR_ERRNO_EINVAL;
}

EXPORT(SceLibKernel, 0xE633BAD1, int, sceSblGcAuthMgrAdhocBB224Shutdown, void)
{
    return SCE_ERROR_ERRNO_EINVAL;
}

EXPORT(SceLibKernel, 0xEB02F15D, int, sceSblGcAuthMgrMsSaveBBCipherFinal, void *unk0)
{
    (void)unk0;
    return SCE_ERROR_ERRNO_EINVAL;
}

EXPORT(SceLibKernel, 0xEE2D40F7, int, sceSblGcAuthMgrMsSaveBBMacUpdate, void *unk0, void *unk1)
{
    (void)unk0;
    (void)unk1;
    return SCE_ERROR_ERRNO_EINVAL;
}

EXPORT(SceLibKernel, 0x17C0CEF4, int, sceSblGcAuthMgrPcactActivation, void *unk0)
{
    (void)unk0;
    return SCE_ERROR_ERRNO_EINVAL;
}
