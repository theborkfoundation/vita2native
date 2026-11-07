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
        enabled = getenv("VITA2NATIVE_TRACE_FIOS") != NULL;
    return enabled;
}

static int fios_result(const char *what, const char *path, int result)
{
    if (trace_fios())
        LOG("Fios%s(\"%s\") -> 0x%x", what, path ? path : "(null)", (unsigned)result);
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

DECLARE_LIBRARY(SceFios2, SCE_FIOS2_LIBRARY_NID);

int SceFios2_init(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
    memset(g_filters, 0, sizeof(g_filters));
    g_initialized = 0;
    g_path_max = FIOS_DEFAULT_PATH_MAX;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int SceFios2_finish(void)
{
    sceFiosTerminate();
    return 0;
}
