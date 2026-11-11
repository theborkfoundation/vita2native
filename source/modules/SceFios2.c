/*
 * SceFios2: the Vita's modern file API, backed by the host filesystem.
 *
 * Only the synchronous entry points are implemented for now. Async operations
 * complete through the same code and are exposed as synchronously-completed
 * op handles where their ABI is known. Everything else resolves to a stub that
 * reports SCE_FIOS_ERROR_UNSUPPORTED, so modules always link.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <psp2common/types.h>

#include "fs.h"
#include "log.h"
#include "module.h"
#include "modules/SceFios2.h"
#include "settings.h"

#define SCE_FIOS_ERROR_UNSUPPORTED  ((int32_t)0x80C90001)
#define SCE_FIOS_ERROR_BAD_ARGUMENT ((int32_t)0x80C90003)
#define SCE_FIOS_ERROR_BAD_PATH     ((int32_t)0x80C90007)
#define SCE_FIOS_ERROR_EXISTS       ((int32_t)0x80C9000Au)
#define SCE_FIOS_ERROR_NOT_EMPTY    ((int32_t)0x80C9000Bu)
#define SCE_FIOS_ERROR_ACCESS       ((int32_t)0x80C90005u)
#define SCE_FIOS_ERROR_BAD_FH       ((int32_t)0x80C90011u)
#define SCE_FIOS_ERROR_BAD_DH       ((int32_t)0x80C90012u)
#define SCE_FIOS_ERROR_EOF          ((int32_t)0x80C9001Fu)

#define FIOS_MAX_FILES 1024
#define FIOS_MAX_DIRS  256
#define FIOS_MAX_FILTERS 16
#define FIOS_DEFAULT_PATH_MAX 1024

typedef struct {
    FILE *fp;
    char *path;
    uint32_t flags;
} VitaFiosFile;

typedef struct {
    DIR *dir;
    char *guest_path;
} VitaFiosDir;

typedef struct {
    void (*callback)(void);
    void *context;
    int used;
} VitaFiosFilter;

static VitaFiosFile *g_files[FIOS_MAX_FILES];
static VitaFiosDir *g_dirs[FIOS_MAX_DIRS];
static VitaFiosFilter g_filters[FIOS_MAX_FILTERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized;
static uint32_t g_path_max = FIOS_DEFAULT_PATH_MAX;

static int32_t errno_to_fios(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return SCE_FIOS_ERROR_BAD_PATH;
    case EEXIST:
        return SCE_FIOS_ERROR_EXISTS;
    case ENOTEMPTY:
        return SCE_FIOS_ERROR_NOT_EMPTY;
    case EACCES:
    case EPERM:
        return SCE_FIOS_ERROR_ACCESS;
    default:
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }
}

static int resolve(const char *path, char *out, size_t size)
{
    if (!path || !path[0])
        return -1;
    return host_fs_resolve_path(path, out, size);
}

static int trace_fios(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = vita2native_env_flag("VITA2NATIVE_TRACE_FIOS");
    return enabled;
}

static int fios_result(const char *what, const char *path, int result)
{
    if (trace_fios())
        LOG("Fios%s(\"%s\") -> 0x%x ra=%p", what, path ? path : "(null)", (unsigned)result,
            __builtin_return_address(0));
    return result;
}

static void stat_from_host(const struct stat *st, SceFiosStat *out)
{
    memset(out, 0, sizeof(*out));
    out->fileSize = S_ISDIR(st->st_mode) ? 0 : (SceFiosOffset)st->st_size;
    out->accessDate = (SceFiosDate)st->st_atime * SCE_FIOS_DATE_MICROSECONDS;
    out->modificationDate = (SceFiosDate)st->st_mtime * SCE_FIOS_DATE_MICROSECONDS;
    out->creationDate = (SceFiosDate)st->st_ctime * SCE_FIOS_DATE_MICROSECONDS;
    out->statFlags = S_ISDIR(st->st_mode) ? SCE_FIOS_STAT_DIRECTORY : 0;
    out->uid = (int64_t)st->st_uid;
    out->gid = (int64_t)st->st_gid;
    out->dev = (int64_t)st->st_dev;
    out->ino = (int64_t)st->st_ino;
    out->mode = (int64_t)st->st_mode;
}

static VitaFiosFile *file_lookup(SceFiosFH fh)
{
    if (fh <= 0 || fh > FIOS_MAX_FILES)
        return NULL;
    return g_files[fh - 1];
}

static VitaFiosDir *dir_lookup(SceFiosDH dh)
{
    if (dh <= 0 || dh > FIOS_MAX_DIRS)
        return NULL;
    return g_dirs[dh - 1];
}

static SceFiosFH file_alloc(VitaFiosFile *file)
{
    for (int i = 0; i < FIOS_MAX_FILES; i++) {
        if (!g_files[i]) {
            g_files[i] = file;
            return (SceFiosFH)(i + 1);
        }
    }
    return SCE_FIOS_ERROR_UNSUPPORTED;
}

static SceFiosDH dir_alloc(VitaFiosDir *dir)
{
    for (int i = 0; i < FIOS_MAX_DIRS; i++) {
        if (!g_dirs[i]) {
            g_dirs[i] = dir;
            return (SceFiosDH)(i + 1);
        }
    }
    return SCE_FIOS_ERROR_UNSUPPORTED;
}

static const char *open_mode_for_flags(uint32_t flags, int *create_only)
{
    int read = (flags & SCE_FIOS_O_RDONLY) != 0;
    int write = (flags & SCE_FIOS_O_WRONLY) != 0;
    int append = (flags & SCE_FIOS_O_APPEND) != 0;
    int trunc = (flags & SCE_FIOS_O_TRUNC) != 0;

    *create_only = 0;

    if (!read && !write)
        read = 1;
    if (read && write) {
        if (append)
            return "a+b";
        if (trunc)
            return "w+b";
        return "r+b";
    }
    if (write) {
        if (append)
            return "ab";
        if (trunc)
            return "wb";
        *create_only = 1;
        return "r+b";
    }
    return "rb";
}

EXPORT(SceFios2, 0x774C2C05, int, sceFiosInitialize, const SceFiosParams *params)
{
    if (!params)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    g_path_max = params->pathMax ? params->pathMax : FIOS_DEFAULT_PATH_MAX;
    g_initialized = 1;
    pthread_mutex_unlock(&g_lock);

    LOG("SceFios2: initialized (pathMax=%u)", g_path_max);
    return 0;
}

EXPORT(SceFios2, 0xF4F54E09, int, sceFiosIsInitialized)
{
    return g_initialized;
}

EXPORT(SceFios2, 0x2904B539, int, sceFiosTerminate)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < FIOS_MAX_FILES; i++) {
        if (g_files[i]) {
            fclose(g_files[i]->fp);
            free(g_files[i]->path);
            free(g_files[i]);
            g_files[i] = NULL;
        }
    }
    for (int i = 0; i < FIOS_MAX_DIRS; i++) {
        if (g_dirs[i]) {
            closedir(g_dirs[i]->dir);
            free(g_dirs[i]->guest_path);
            free(g_dirs[i]);
            g_dirs[i] = NULL;
        }
    }
    memset(g_filters, 0, sizeof(g_filters));
    g_initialized = 0;
    pthread_mutex_unlock(&g_lock);

    return 0;
}

EXPORT(SceFios2, 0xF85C208B, int, sceFiosCloseAllFiles)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < FIOS_MAX_FILES; i++) {
        if (g_files[i]) {
            fclose(g_files[i]->fp);
            free(g_files[i]->path);
            free(g_files[i]);
            g_files[i] = NULL;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

EXPORT(SceFios2, 0xACBAF3E0, int, sceFiosStatSync, const void *attr, const char *path,
       SceFiosStat *out_stat)
{
    char host[PATH_MAX];
    struct stat st;

    (void)attr;

    if (!path || !out_stat)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return fios_result("StatSync", path, SCE_FIOS_ERROR_BAD_PATH);
    if (stat(host, &st) < 0)
        return fios_result("StatSync", path, errno_to_fios(errno));

    stat_from_host(&st, out_stat);
    return fios_result("StatSync", path, 0);
}

static int path_exists(const char *path, int want_dir)
{
    char host[PATH_MAX];
    struct stat st;

    if (!path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return fios_result("Exists", path, SCE_FIOS_ERROR_BAD_PATH);
    if (stat(host, &st) < 0)
        return fios_result("Exists", path, errno_to_fios(errno));
    if (want_dir == 1 && !S_ISDIR(st.st_mode))
        return fios_result("Exists", path, SCE_FIOS_ERROR_BAD_PATH);
    if (want_dir == 0 && S_ISDIR(st.st_mode))
        return fios_result("Exists", path, SCE_FIOS_ERROR_BAD_PATH);
    return fios_result("Exists", path, 0);
}

EXPORT(SceFios2, 0x125EFD34, int, sceFiosExistsSync, const void *attr, const char *path)
{
    (void)attr;
    return path_exists(path, -1);
}

EXPORT(SceFios2, 0x233B070C, int, sceFiosFileExistsSync, const void *attr, const char *path)
{
    (void)attr;
    return path_exists(path, 0);
}

EXPORT(SceFios2, 0x726E01BE, int, sceFiosDirectoryExistsSync, const void *attr, const char *path)
{
    (void)attr;
    return path_exists(path, 1);
}

EXPORT(SceFios2, 0x789215C3, SceFiosSize, sceFiosFileGetSizeSync, const void *attr,
       const char *path)
{
    char host[PATH_MAX];
    struct stat st;

    (void)attr;

    if (!path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;
    if (stat(host, &st) < 0)
        return errno_to_fios(errno);

    return (SceFiosSize)st.st_size;
}

EXPORT(SceFios2, 0xC3E7C3DB, int, sceFiosFHOpenSync, const void *attr, SceFiosFH *fh,
       const char *path, const void *params)
{
    char host[PATH_MAX];
    uint32_t flags = SCE_FIOS_O_RDONLY;
    VitaFiosFile *file;
    FILE *fp;
    int create_only;
    const char *mode;
    SceFiosFH handle;

    (void)attr;

    if (!fh || !path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    *fh = 0;

    if (params)
        flags = *(const uint32_t *)params;

    if (resolve(path, host, sizeof(host)) < 0)
        return fios_result("FHOpenSync", path, SCE_FIOS_ERROR_BAD_PATH);

    mode = open_mode_for_flags(flags, &create_only);
    fp = fopen(host, mode);
    if (!fp && create_only && errno == ENOENT)
        fp = fopen(host, "wb");
    if (!fp)
        return fios_result("FHOpenSync", path, errno_to_fios(errno));

    file = calloc(1, sizeof(*file));
    if (!file) {
        fclose(fp);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }
    file->fp = fp;
    file->path = strdup(path);
    file->flags = flags;
    if (!file->path) {
        fclose(fp);
        free(file);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }

    pthread_mutex_lock(&g_lock);
    handle = file_alloc(file);
    pthread_mutex_unlock(&g_lock);
    if (handle < 0) {
        fclose(fp);
        free(file->path);
        free(file);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }

    *fh = handle;
    return 0;
}

EXPORT(SceFios2, 0x45182328, int, sceFiosFHCloseSync, const void *attr, SceFiosFH fh)
{
    VitaFiosFile *file;

    (void)attr;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (file)
        g_files[fh - 1] = NULL;
    pthread_mutex_unlock(&g_lock);

    if (!file)
        return SCE_FIOS_ERROR_BAD_FH;

    fclose(file->fp);
    free(file->path);
    free(file);
    return 0;
}

EXPORT(SceFios2, 0x76945919, SceFiosSize, sceFiosFHReadSync, const void *attr, SceFiosFH fh,
       void *data, SceFiosSize size)
{
    VitaFiosFile *file;
    size_t read_size;

    (void)attr;

    if (!data || size < 0)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    read_size = fread(data, 1, (size_t)size, file->fp);
    if (read_size == 0 && ferror(file->fp)) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }
    pthread_mutex_unlock(&g_lock);

    return (SceFiosSize)read_size;
}

EXPORT(SceFios2, 0x984024E5, SceFiosSize, sceFiosFHWriteSync, const void *attr, SceFiosFH fh,
       const void *data, SceFiosSize size)
{
    VitaFiosFile *file;
    size_t written;

    (void)attr;

    if (!data || size < 0)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    written = fwrite(data, 1, (size_t)size, file->fp);
    if (written == 0 && ferror(file->fp)) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }
    pthread_mutex_unlock(&g_lock);

    return (SceFiosSize)written;
}

EXPORT(SceFios2, 0xA75F3C4A, SceFiosOffset, sceFiosFHSeek, SceFiosFH fh, SceFiosOffset offset,
       int whence)
{
    VitaFiosFile *file;
    int host_whence;

    switch (whence) {
    case 0:
        host_whence = SEEK_SET;
        break;
    case 1:
        host_whence = SEEK_CUR;
        break;
    case 2:
        host_whence = SEEK_END;
        break;
    default:
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    }

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    if (fseeko(file->fp, (off_t)offset, host_whence) != 0) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    }
    offset = (SceFiosOffset)ftello(file->fp);
    pthread_mutex_unlock(&g_lock);

    return offset;
}

EXPORT(SceFios2, 0xD7F33130, SceFiosOffset, sceFiosFHTell, SceFiosFH fh)
{
    VitaFiosFile *file;
    SceFiosOffset offset;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    offset = (SceFiosOffset)ftello(file->fp);
    pthread_mutex_unlock(&g_lock);

    return offset;
}

EXPORT(SceFios2, 0xF8BEAC88, int, sceFiosFHStatSync, const void *attr, SceFiosFH fh,
       SceFiosStat *stat)
{
    VitaFiosFile *file;
    struct stat st;

    (void)attr;

    if (!stat)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    if (fstat(fileno(file->fp), &st) < 0) {
        int err = errno;
        pthread_mutex_unlock(&g_lock);
        return errno_to_fios(err);
    }
    pthread_mutex_unlock(&g_lock);

    stat_from_host(&st, stat);
    return 0;
}

EXPORT(SceFios2, 0xC5C26581, SceFiosSize, sceFiosFHGetSize, SceFiosFH fh)
{
    VitaFiosFile *file;
    struct stat st;

    pthread_mutex_lock(&g_lock);
    file = file_lookup(fh);
    if (!file) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_FH;
    }
    if (fstat(fileno(file->fp), &st) < 0) {
        int err = errno;
        pthread_mutex_unlock(&g_lock);
        return errno_to_fios(err);
    }
    pthread_mutex_unlock(&g_lock);

    return (SceFiosSize)st.st_size;
}

EXPORT(SceFios2, 0x34BC3713, int, sceFiosDHOpenSync, const void *attr, SceFiosDH *dh,
       const char *path, SceFiosBuffer buffer)
{
    char host[PATH_MAX];
    VitaFiosDir *dir;
    DIR *host_dir;
    SceFiosDH handle;

    (void)attr;
    (void)buffer;

    if (!dh || !path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    *dh = 0;

    if (resolve(path, host, sizeof(host)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;

    host_dir = opendir(host);
    if (!host_dir)
        return errno_to_fios(errno);

    dir = calloc(1, sizeof(*dir));
    if (!dir) {
        closedir(host_dir);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }
    dir->dir = host_dir;
    dir->guest_path = strdup(path);
    if (!dir->guest_path) {
        closedir(host_dir);
        free(dir);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }

    pthread_mutex_lock(&g_lock);
    handle = dir_alloc(dir);
    pthread_mutex_unlock(&g_lock);
    if (handle < 0) {
        closedir(host_dir);
        free(dir->guest_path);
        free(dir);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }

    *dh = handle;
    return 0;
}

EXPORT(SceFios2, 0xB7E79CAD, int, sceFiosDHReadSync, const void *attr, SceFiosDH dh,
       SceFiosDirEntry *entry)
{
    VitaFiosDir *dir;
    struct dirent *host_entry;
    char full_path[PATH_MAX];
    char host_full[PATH_MAX];
    struct stat st;
    size_t guest_len;
    size_t name_len;

    (void)attr;

    if (!entry)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    memset(entry, 0, sizeof(*entry));

    pthread_mutex_lock(&g_lock);
    dir = dir_lookup(dh);
    if (!dir) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_DH;
    }

    do {
        host_entry = readdir(dir->dir);
    } while (host_entry && (strcmp(host_entry->d_name, ".") == 0 ||
                            strcmp(host_entry->d_name, "..") == 0));

    if (!host_entry) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_EOF;
    }

    guest_len = strlen(dir->guest_path);
    if (guest_len == 0 || dir->guest_path[guest_len - 1] != '/')
        snprintf(full_path, sizeof(full_path), "%s/%s", dir->guest_path, host_entry->d_name);
    else
        snprintf(full_path, sizeof(full_path), "%s%s", dir->guest_path, host_entry->d_name);

    name_len = strlen(host_entry->d_name);
    if (strlen(full_path) >= sizeof(entry->fullPath)) {
        pthread_mutex_unlock(&g_lock);
        return SCE_FIOS_ERROR_BAD_PATH;
    }
    strcpy(entry->fullPath, full_path);
    entry->fullPathLength = (uint16_t)strlen(full_path);
    entry->offsetToName = (uint16_t)(strlen(full_path) - name_len);
    entry->nameLength = (uint16_t)name_len;

    if (resolve(full_path, host_full, sizeof(host_full)) == 0 && stat(host_full, &st) == 0) {
        entry->fileSize = S_ISDIR(st.st_mode) ? 0 : (SceFiosOffset)st.st_size;
        entry->statFlags = S_ISDIR(st.st_mode) ? SCE_FIOS_STAT_DIRECTORY : 0;
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

EXPORT(SceFios2, 0x1F3CC428, int, sceFiosDHCloseSync, const void *attr, SceFiosDH dh)
{
    VitaFiosDir *dir;

    (void)attr;

    pthread_mutex_lock(&g_lock);
    dir = dir_lookup(dh);
    if (dir)
        g_dirs[dh - 1] = NULL;
    pthread_mutex_unlock(&g_lock);

    if (!dir)
        return SCE_FIOS_ERROR_BAD_DH;

    closedir(dir->dir);
    free(dir->guest_path);
    free(dir);
    return 0;
}

static int make_directory(const char *path, mode_t mode)
{
    char host[PATH_MAX];

    if (!path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;
    if (mkdir(host, mode) < 0)
        return errno_to_fios(errno);
    return 0;
}

EXPORT(SceFios2, 0xE037B076, int, sceFiosDirectoryCreateSync, const void *attr, const char *path)
{
    (void)attr;
    return make_directory(path, 0777);
}

EXPORT(SceFios2, 0xA2C610E6, int, sceFiosDirectoryCreateWithModeSync, const void *attr,
       const char *path, unsigned int mode)
{
    (void)attr;
    return make_directory(path, (mode_t)(mode & 0777));
}

static int remove_path(const char *path, int want_dir)
{
    char host[PATH_MAX];
    struct stat st;

    if (!path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;
    if (stat(host, &st) < 0)
        return errno_to_fios(errno);

    if (want_dir < 0) {
        if (S_ISDIR(st.st_mode)) {
            if (rmdir(host) < 0)
                return errno_to_fios(errno);
        } else if (unlink(host) < 0) {
            return errno_to_fios(errno);
        }
        return 0;
    }

    if (want_dir) {
        if (!S_ISDIR(st.st_mode))
            return SCE_FIOS_ERROR_BAD_PATH;
        if (rmdir(host) < 0)
            return errno_to_fios(errno);
    } else {
        if (S_ISDIR(st.st_mode))
            return SCE_FIOS_ERROR_BAD_PATH;
        if (unlink(host) < 0)
            return errno_to_fios(errno);
    }
    return 0;
}

EXPORT(SceFios2, 0xB5302E30, int, sceFiosFileDeleteSync, const void *attr, const char *path)
{
    (void)attr;
    return remove_path(path, 0);
}

EXPORT(SceFios2, 0xB9573146, int, sceFiosDirectoryDeleteSync, const void *attr, const char *path)
{
    (void)attr;
    return remove_path(path, 1);
}

EXPORT(SceFios2, 0xAAC54B44, int, sceFiosDeleteSync, const void *attr, const char *path)
{
    (void)attr;
    return remove_path(path, -1);
}

EXPORT(SceFios2, 0x030306F4, int, sceFiosRenameSync, const void *attr, const char *old_path,
       const char *new_path)
{
    char host_old[PATH_MAX];
    char host_new[PATH_MAX];

    (void)attr;

    if (!old_path || !new_path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(old_path, host_old, sizeof(host_old)) < 0 ||
        resolve(new_path, host_new, sizeof(host_new)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;
    if (rename(host_old, host_new) < 0)
        return errno_to_fios(errno);
    return 0;
}

EXPORT(SceFios2, 0x94FDFFEE, SceDateTime *, sceFiosDateToSceDateTime, SceFiosDate date,
       SceDateTime *out)
{
    time_t seconds;
    uint32_t microseconds;
    struct tm tm;

    if (!out)
        return NULL;

    seconds = (time_t)(date / SCE_FIOS_DATE_MICROSECONDS);
    microseconds = (uint32_t)(date % SCE_FIOS_DATE_MICROSECONDS);
    if (!gmtime_r(&seconds, &tm))
        return NULL;

    memset(out, 0, sizeof(*out));
    out->year = (unsigned short)(tm.tm_year + 1900);
    out->month = (unsigned short)(tm.tm_mon + 1);
    out->day = (unsigned short)tm.tm_mday;
    out->hour = (unsigned short)tm.tm_hour;
    out->minute = (unsigned short)tm.tm_min;
    out->second = (unsigned short)tm.tm_sec;
    out->microsecond = microseconds;

    return out;
}

EXPORT(SceFios2, 0x6EC156DA, SceFiosDate, sceFiosDateFromSceDateTime, const SceDateTime *date)
{
    struct tm tm;
    time_t seconds;

    if (!date)
        return 0;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = (int)date->year - 1900;
    tm.tm_mon = (int)date->month - 1;
    tm.tm_mday = (int)date->day;
    tm.tm_hour = (int)date->hour;
    tm.tm_min = (int)date->minute;
    tm.tm_sec = (int)date->second;
    tm.tm_isdst = 0;

    seconds = timegm(&tm);
    if (seconds < 0)
        return 0;

    return (SceFiosDate)seconds * SCE_FIOS_DATE_MICROSECONDS + date->microsecond;
}

EXPORT(SceFios2, 0x5C593C1E, SceFiosDate, sceFiosDateGetCurrent)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (SceFiosDate)ts.tv_sec * SCE_FIOS_DATE_MICROSECONDS + (SceFiosDate)ts.tv_nsec / 1000;
}

EXPORT(SceFios2, 0xD55B8555, int, sceFiosIOFilterAdd, int index, void (*callback)(void),
       void *context)
{
    if (index < 0 || index >= FIOS_MAX_FILTERS)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (!callback)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    g_filters[index].callback = callback;
    g_filters[index].context = context;
    g_filters[index].used = 1;
    pthread_mutex_unlock(&g_lock);

    return 0;
}

EXPORT(SceFios2, 0x22E35018, int, sceFiosIOFilterRemove, int index)
{
    if (index < 0 || index >= FIOS_MAX_FILTERS)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_lock);
    memset(&g_filters[index], 0, sizeof(g_filters[index]));
    pthread_mutex_unlock(&g_lock);

    return 0;
}

/*
 * This entry point is only ever used as an opaque callback token passed to
 * sceFiosIOFilterAdd; the host never calls into guest code.
 */
EXPORT(SceFios2, 0x057252F2, void, sceFiosIOFilterPsarcDearchiver, void)
{
}

EXPORT(SceFios2, 0xDF3352FC, SceFiosSize, sceFiosArchiveGetMountBufferSizeSync, const void *attr,
       const char *path, void *params)
{
    char host[PATH_MAX];
    struct stat st;

    (void)attr;
    (void)params;

    if (trace_fios())
        LOG("FiosArchiveGetMountBufferSizeSync(path=\"%s\")", path ? path : "(null)");

    if (!path)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    if (resolve(path, host, sizeof(host)) < 0)
        return SCE_FIOS_ERROR_BAD_PATH;
    if (stat(host, &st) < 0)
        return errno_to_fios(errno);

    /* Mounting currently unsupported; still report a usable size so callers
     * can distinguish "not an archive" from "not implemented". */
    return (SceFiosSize)0x400000;
}

EXPORT(SceFios2, 0xC4822276, int, sceFiosArchiveMountSync, const void *attr, SceFiosFH *fh,
       const char *path, const char *mount_point, SceFiosBuffer mount_buffer, void *params)
{
    (void)attr;
    (void)params;

    if (trace_fios())
        LOG("FiosArchiveMountSync(path=\"%s\", mount=\"%s\") fh=%p", path ? path : "(null)",
            mount_point ? mount_point : "(null)", fh ? (void *)*fh : NULL);

    (void)mount_buffer;
    (void)params;

    if (!fh || !path || !mount_point)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    *fh = 0;

    LOG("SceFios2: archive mounting is not implemented (%s)", path);
    return SCE_FIOS_ERROR_UNSUPPORTED;
}

EXPORT(SceFios2, 0xB26DC24D, int, sceFiosArchiveUnmountSync, const void *attr, SceFiosFH fh)
{
    (void)attr;
    (void)fh;
    return 0;
}

EXPORT(SceFios2, 0x0DF32816, int, sceFiosSync, const void *attr)
{
    (void)attr;
    return 0;
}

EXPORT(SceFios2, 0x06D97629, int, sceFiosSyncSync, const void *attr)
{
    (void)attr;
    return 0;
}

EXPORT(SceFios2, 0xB309E327, int, sceFiosIsValidHandle, int handle)
{
    if (handle <= 0)
        return 0;
    if (handle <= FIOS_MAX_FILES && g_files[handle - 1])
        return 1;
    if (handle <= FIOS_MAX_DIRS && g_dirs[handle - 1])
        return 1;
    return 0;
}

/*
 * Any remaining SceFios2 NID resolves to this stub. Returning int64_t zeroes
 * both result registers so callers expecting SceFiosSize also get a defined
 * value.
 */
static int64_t fios_unimplemented(void)
{
    return SCE_FIOS_ERROR_UNSUPPORTED;
}

#include "SceFios2_stubs.inc"

/*
 * SceFios2User: the kernel-side overlay API used by applications that ship
 * their own libfios2.suprx. The module resolves guest paths such as
 * "/app0/package_info.txt" through this library; return the equivalent
 * device-qualified path so the host filesystem resolver can open it.
 */
#define SCE_FIOS2USER_LIBRARY_NID 0xC2ED82D7
#define FIOS_MAX_OVERLAYS 64
#define FIOS_OVERLAY_POINT_MAX 292

enum {
    SCE_FIOS_OVERLAY_TYPE_OPAQUE = 0,
};

typedef struct {
    uint8_t type;
    uint8_t order;
    int16_t src_size;
    int16_t dst_size;
    uint32_t process_id;
    uint32_t reserved;
    char dst[FIOS_OVERLAY_POINT_MAX];
    char src[FIOS_OVERLAY_POINT_MAX];
} SceFiosProcessOverlay;

typedef struct {
    int used;
    int32_t id;
    uint8_t type;
    uint8_t order;
    uint32_t process_id;
    char dst[FIOS_OVERLAY_POINT_MAX];
    char src[FIOS_OVERLAY_POINT_MAX];
} VitaFiosOverlay;

static VitaFiosOverlay g_overlays[FIOS_MAX_OVERLAYS];
static int32_t g_next_overlay_id = 1;
static pthread_mutex_t g_overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_overlay_trace_count;

static void copy_overlay_path(char *dest, const char *source)
{
    size_t length = strnlen(source, FIOS_OVERLAY_POINT_MAX - 1);

    memcpy(dest, source, length);
    dest[length] = '\0';
}

/* app0:foo and app0:/foo name the same guest path. */
static void standardize_overlay_path(char *path)
{
    size_t length;

    if (strncmp(path, "app0:", 5) != 0 || path[5] == '/')
        return;

    length = strlen(path + 5);
    if (5 + 1 + length >= FIOS_OVERLAY_POINT_MAX)
        return;
    memmove(path + 6, path + 5, length + 1);
    path[5] = '/';
}

static void overlay_resolve_path(const char *input, char *out, size_t out_size,
                                 uint32_t min_order, uint32_t max_order)
{
    char current[FIOS_OVERLAY_POINT_MAX * 2];

    snprintf(current, sizeof(current), "%s", input ? input : "");
    pthread_mutex_lock(&g_overlay_lock);

    for (uint32_t order = 0; order < 256; order++) {
        if (order < min_order || order > max_order)
            continue;

        for (int i = 0; i < FIOS_MAX_OVERLAYS; i++) {
            VitaFiosOverlay *overlay = &g_overlays[i];
            size_t dst_length;

            if (!overlay->used || overlay->order != order)
                continue;
            dst_length = strlen(overlay->dst);
            if (dst_length == 0 || strncmp(current, overlay->dst, dst_length) != 0)
                continue;

            {
                char next[FIOS_OVERLAY_POINT_MAX * 2];

                snprintf(next, sizeof(next), "%s%s", overlay->src, current + dst_length);
                snprintf(current, sizeof(current), "%s", next);
            }
        }
    }

    pthread_mutex_unlock(&g_overlay_lock);

    /* Default guest mounts when the title did not install an explicit overlay. */
    if (strncmp(current, "/app0", 5) == 0 && (current[5] == '/' || current[5] == '\0'))
        snprintf(out, out_size, "app0:%s", current + 5);
    else if (strncmp(current, "/ux0", 4) == 0 && (current[4] == '/' || current[4] == '\0'))
        snprintf(out, out_size, "ux0:%s", current + 4);
    else if (strncmp(current, "/savedata0", 10) == 0 &&
             (current[10] == '/' || current[10] == '\0'))
        snprintf(out, out_size, "savedata0:%s", current + 10);
    else if (strncmp(current, "/os0", 4) == 0 && (current[4] == '/' || current[4] == '\0'))
        snprintf(out, out_size, "os0:%s", current + 4);
    else if (strncmp(current, "/vs0", 4) == 0 && (current[4] == '/' || current[4] == '\0'))
        snprintf(out, out_size, "vs0:%s", current + 4);
    else
        snprintf(out, out_size, "%s", current);
}

EXPORT(SceFios2User, 0x6C4BE9CD, int, sceFiosOverlayAddForProcess02, uint32_t process_id,
       const SceFiosProcessOverlay *overlay, int32_t *out_id)
{
    int slot = -1;
    int32_t id;

    if (!overlay || !out_id)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_overlay_lock);
    for (int i = 0; i < FIOS_MAX_OVERLAYS; i++) {
        if (!g_overlays[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_overlay_lock);
        return SCE_FIOS_ERROR_UNSUPPORTED;
    }

    memset(&g_overlays[slot], 0, sizeof(g_overlays[slot]));
    g_overlays[slot].used = 1;
    g_overlays[slot].id = id = g_next_overlay_id++;
    g_overlays[slot].type = overlay->type;
    g_overlays[slot].order = overlay->order;
    g_overlays[slot].process_id = process_id ? process_id : overlay->process_id;
    copy_overlay_path(g_overlays[slot].dst, overlay->dst);
    copy_overlay_path(g_overlays[slot].src, overlay->src);
    standardize_overlay_path(g_overlays[slot].dst);
    standardize_overlay_path(g_overlays[slot].src);
    pthread_mutex_unlock(&g_overlay_lock);

    *out_id = id;
    if (trace_fios() && g_overlay_trace_count++ < 16)
        LOG("SceFios2User: overlay %d order %u '%s' -> '%s'", id, overlay->order,
            g_overlays[slot].dst, g_overlays[slot].src);
    return 0;
}

EXPORT(SceFios2User, 0xAB7B4213, int, sceFiosOverlayGetInfoForProcess02, uint32_t process_id,
       int32_t overlay_id, SceFiosProcessOverlay *out_overlay)
{
    int found = 0;

    (void)process_id;
    if (!out_overlay)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;

    pthread_mutex_lock(&g_overlay_lock);
    for (int i = 0; i < FIOS_MAX_OVERLAYS; i++) {
        VitaFiosOverlay *overlay = &g_overlays[i];

        if (!overlay->used || overlay->id != overlay_id)
            continue;

        memset(out_overlay, 0, sizeof(*out_overlay));
        out_overlay->type = overlay->type;
        out_overlay->order = overlay->order;
        out_overlay->process_id = overlay->process_id;
        out_overlay->dst_size = (int16_t)strlen(overlay->dst);
        out_overlay->src_size = (int16_t)strlen(overlay->src);
        memcpy(out_overlay->dst, overlay->dst, (size_t)out_overlay->dst_size + 1);
        memcpy(out_overlay->src, overlay->src, (size_t)out_overlay->src_size + 1);
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_overlay_lock);

    if (trace_fios() && g_overlay_trace_count++ < 16)
        LOG("SceFios2User: get info %d -> %s", overlay_id, found ? "ok" : "missing");
    return found ? 0 : SCE_FIOS_ERROR_BAD_ARGUMENT;
}

EXPORT(SceFios2User, 0x1DD808D1, int, sceFiosOverlayGetList02, uint32_t process_id,
       uint32_t min_order, uint32_t max_order, int32_t *out_ids, uint32_t max_ids,
       uint32_t *actual_ids)
{
    uint32_t count = 0;

    (void)process_id;
    pthread_mutex_lock(&g_overlay_lock);
    for (int i = 0; i < FIOS_MAX_OVERLAYS; i++) {
        VitaFiosOverlay *overlay = &g_overlays[i];

        if (!overlay->used || overlay->order < min_order || overlay->order > max_order)
            continue;
        if (out_ids && count < max_ids)
            out_ids[count] = overlay->id;
        count++;
    }
    pthread_mutex_unlock(&g_overlay_lock);

    if (actual_ids)
        *actual_ids = count;
    if (trace_fios() && g_overlay_trace_count++ < 16)
        LOG("SceFios2User: get list -> %u overlay(s)", count);
    return 0;
}

EXPORT(SceFios2User, 0xF5C1F928, int, sceFiosOverlayGetRecommendedScheduler02, int param1,
       const char *path)
{
    /* Matches Vita3K: host0: paths use a different scheduler. */
    if (param1 <= 1 || !path || strlen(path) < 6)
        return 0;
    return memcmp(path, "host", 4) == 0 && path[4] <= '9' && path[5] == ':';
}

EXPORT(SceFios2User, 0xCA388053, int, sceFiosOverlayModifyForProcess02)
{
    return SCE_FIOS_ERROR_UNSUPPORTED;
}

EXPORT(SceFios2User, 0xE5D1B6F5, int, sceFiosOverlayRemoveForProcess02, uint32_t process_id,
       int32_t overlay_id)
{
    (void)process_id;
    pthread_mutex_lock(&g_overlay_lock);
    for (int i = 0; i < FIOS_MAX_OVERLAYS; i++) {
        if (g_overlays[i].used && g_overlays[i].id == overlay_id) {
            memset(&g_overlays[i], 0, sizeof(g_overlays[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_overlay_lock);
    return 0;
}

EXPORT(SceFios2User, 0x9C3AAAF0, int, sceFiosOverlayResolveSync02, uint32_t process_id, int mode,
       const char *in_path, char *out_path, uint32_t max_path)
{
    (void)process_id;
    (void)mode;
    if (!in_path || !out_path || max_path == 0)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    overlay_resolve_path(in_path, out_path, max_path, 0, 255);
    return 0;
}

EXPORT(SceFios2User, 0x61C4AAC4, int, sceFiosOverlayResolveWithRangeSync02, uint32_t process_id,
       int mode, const char *in_path, char *out_path, uint32_t max_path, uint32_t min_order,
       uint32_t max_order)
{
    (void)process_id;
    (void)mode;
    if (!in_path || !out_path || max_path == 0)
        return SCE_FIOS_ERROR_BAD_ARGUMENT;
    overlay_resolve_path(in_path, out_path, max_path, min_order, max_order);
    if (trace_fios() && g_overlay_trace_count++ < 32)
        LOG("SceFios2User: resolve '%s' -> '%s'", in_path, out_path);
    return 0;
}

EXPORT(SceFios2User, 0x23B8DB1D, int, sceFiosOverlayThreadIsDisabled02)
{
    return 0;
}

EXPORT(SceFios2User, 0x70321220, int, sceFiosOverlayThreadSetDisabled02)
{
    return 0;
}

DECLARE_LIBRARY(SceFios2User, SCE_FIOS2USER_LIBRARY_NID);
DECLARE_LIBRARY(SceFios2, SCE_FIOS2_LIBRARY_NID);

int SceFios2_init(void)
{
    static const struct {
        const char *dst;
        const char *src;
    } defaults[] = {
        { "/app0/", "app0:/" },
        { "/ux0/", "ux0:/" },
        { "/savedata0/", "savedata0:/" },
        { "/os0/", "os0:/" },
        { "/vs0/", "vs0:/" },
    };

    pthread_mutex_lock(&g_lock);
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    memset(g_filters, 0, sizeof(g_filters));
    g_initialized = 0;
    g_path_max = FIOS_DEFAULT_PATH_MAX;
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_overlay_lock);
    memset(g_overlays, 0, sizeof(g_overlays));
    g_next_overlay_id = 1;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        VitaFiosOverlay *overlay = &g_overlays[i];

        overlay->used = 1;
        overlay->id = g_next_overlay_id++;
        overlay->type = SCE_FIOS_OVERLAY_TYPE_OPAQUE;
        overlay->order = 0;
        copy_overlay_path(overlay->dst, defaults[i].dst);
        copy_overlay_path(overlay->src, defaults[i].src);
    }
    pthread_mutex_unlock(&g_overlay_lock);
    return 0;
}

int SceFios2_finish(void)
{
    sceFiosTerminate();
    return 0;
}
