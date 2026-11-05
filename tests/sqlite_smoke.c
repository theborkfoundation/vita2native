/*
 * Exercises the real SQLite backend behind SceSqlite:
 *   - open/create a database through a Vita device path,
 *   - insert, query, bind and column access,
 *   - persistence across close/reopen,
 *   - the guest-style VFS wrapper VitaShell registers (it copies the default
 *     VFS and replaces xOpen/xDelete), including the Sony file layout.
 *
 * The SceIo entry points are reimplemented here on stdio so the module can be
 * tested without linking the whole kernel.
 */

#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* glibc aliases these struct stat members to macros. */
#undef st_atime
#undef st_mtime
#undef st_ctime

#include <psp2/io/fcntl.h>
#include <psp2common/kernel/iofilemgr.h>

#include "fs.h"
#include "modules/SceSqlite.h"

#include "sqlite3.h"

#define TEST_MAX_FILES 32

typedef struct {
    int used;
    SceUID uid;
    FILE *fp;
} TestFile;

static TestFile g_files[TEST_MAX_FILES];
static _Atomic SceUID g_next_uid = 1;

SceUID SceSysmem_get_next_uid(void)
{
    return atomic_fetch_add(&g_next_uid, 1);
}

static TestFile *file_for_fd(SceUID uid)
{
    for (int i = 0; i < TEST_MAX_FILES; i++) {
        if (g_files[i].used && g_files[i].uid == uid)
            return &g_files[i];
    }
    return NULL;
}

SceUID sceIoOpen(const char *path, int flags, SceMode mode)
{
    char host_path[1024];
    const char *open_mode;
    TestFile *slot = NULL;
    FILE *fp;

    (void)mode;

    if (host_fs_resolve_path(path, host_path, sizeof(host_path)) < 0)
        return -1;

    if ((flags & SCE_O_RDWR) == SCE_O_RDWR)
        open_mode = "rb+";
    else if (flags & SCE_O_WRONLY)
        open_mode = "wb";
    else
        open_mode = "rb";

    fp = fopen(host_path, open_mode);
    if (!fp && (flags & SCE_O_CREAT))
        fp = fopen(host_path, "wb+");
    if (!fp)
        return -1;

    for (int i = 0; i < TEST_MAX_FILES; i++) {
        if (!g_files[i].used) {
            slot = &g_files[i];
            break;
        }
    }
    if (!slot) {
        fclose(fp);
        return -1;
    }

    slot->used = 1;
    slot->uid = SceSysmem_get_next_uid();
    slot->fp = fp;
    return slot->uid;
}

int sceIoClose(SceUID fd)
{
    TestFile *file = file_for_fd(fd);

    if (!file)
        return -1;

    fclose(file->fp);
    memset(file, 0, sizeof(*file));
    return 0;
}

SceSSize sceIoRead(SceUID fd, void *buffer, SceSize size)
{
    TestFile *file = file_for_fd(fd);

    if (!file)
        return -1;
    return (SceSSize)fread(buffer, 1, size, file->fp);
}

SceSSize sceIoWrite(SceUID fd, const void *buffer, SceSize size)
{
    TestFile *file = file_for_fd(fd);

    if (!file)
        return -1;
    return (SceSSize)fwrite(buffer, 1, size, file->fp);
}

SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    TestFile *file = file_for_fd(fd);

    if (!file)
        return -1;
    if (fseeko(file->fp, (off_t)offset, whence) != 0)
        return -1;
    return (SceOff)ftello(file->fp);
}

int sceIoSyncByFd(SceUID fd, int flag)
{
    TestFile *file = file_for_fd(fd);

    (void)flag;

    if (!file)
        return -1;
    return fflush(file->fp);
}

int SceLibKernel_fd_truncate(SceUID fd, int64_t size)
{
    TestFile *file = file_for_fd(fd);

    if (!file)
        return -1;
    fflush(file->fp);
    return ftruncate(fileno(file->fp), (off_t)size);
}

int SceLibKernel_fd_setlock(SceUID fd, int exclusive, int wait)
{
    TestFile *file = file_for_fd(fd);
    struct flock lock;

    if (!file)
        return -1;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = exclusive < 0 ? F_UNLCK : (exclusive ? F_WRLCK : F_RDLCK);
    lock.l_whence = SEEK_SET;

    return fcntl(fileno(file->fp), wait ? F_SETLKW : F_SETLK, &lock);
}

/* ------------------------------------------------------------------------- */
/* Guest-style VFS wrapper, mirroring VitaShell's sqlite_init()               */
/* ------------------------------------------------------------------------- */

typedef struct {
    sqlite3_file base;
    int *fd;
} GuestFile;

static sqlite3_io_methods g_guest_methods;
static int (*g_guest_original_write)(sqlite3_file *, const void *, int, sqlite3_int64);
static atomic_int g_guest_writes;
static int g_guest_bad_fd;

static int guest_write(sqlite3_file *file, const void *buffer, int count, sqlite3_int64 offset)
{
    GuestFile *guest = (GuestFile *)file;

    if (!guest->fd || sceIoLseek(*guest->fd, 0, SCE_SEEK_CUR) < 0)
        g_guest_bad_fd = 1;

    atomic_fetch_add(&g_guest_writes, 1);
    return g_guest_original_write(file, buffer, count, offset);
}

static int guest_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags,
                      int *out_flags)
{
    sqlite3_vfs *original = vfs->pAppData;
    int ret = original->xOpen(original, name, file, flags, out_flags);

    if (ret == SQLITE_OK && file->pMethods) {
        memcpy(&g_guest_methods, file->pMethods, sizeof(g_guest_methods));
        g_guest_original_write = g_guest_methods.xWrite;
        g_guest_methods.xWrite = guest_write;
        file->pMethods = &g_guest_methods;
    }

    return ret;
}

static int guest_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    sqlite3_vfs *original = vfs->pAppData;

    return original->xDelete(original, name, sync_dir);
}

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("sqlite_smoke: %s (line %d)\n", message, __LINE__);                             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int count_callback(void *arg, int columns, char **values, char **names)
{
    (void)columns;
    (void)values;
    (void)names;

    (*(int *)arg)++;
    return 0;
}

int main(void)
{
    char root[] = "/tmp/v2n-sqlite-XXXXXX";
    char app_dir[1024];
    sqlite3_vfs guest_vfs;
    sqlite3_stmt *stmt;
    sqlite3 *db;
    char *error = NULL;
    int rows = 0;
    int ret;

    if (!mkdtemp(root)) {
        puts("sqlite_smoke: mkdtemp failed");
        return 1;
    }

    snprintf(app_dir, sizeof(app_dir), "%s/app0", root);
    if (host_fs_init(root, app_dir) < 0) {
        puts("sqlite_smoke: host_fs_init failed");
        return 1;
    }
    mkdir(app_dir, 0755);

    CHECK(SceSqlite_init() == 0, "could not initialize SQLite");
    CHECK(sqlite3_libversion_number() >= 3011000, "unexpected SQLite version");
    CHECK(sqlite3_threadsafe() != 0, "SQLite was built without thread safety");

    ret = sqlite3_open_v2("ux0:/smoke.db", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "psp2");
    CHECK(ret == SQLITE_OK, sqlite3_errmsg(db));

    ret = sqlite3_exec(db, "CREATE TABLE t(a INTEGER, b TEXT)", NULL, NULL, &error);
    if (ret != SQLITE_OK) {
        printf("sqlite_smoke: create table failed: %s (code %d extended %d)\n",
               error ? error : "?", sqlite3_errcode(db), sqlite3_extended_errcode(db));
        return 1;
    }
    CHECK(sqlite3_exec(db, "INSERT INTO t VALUES(1, 'one')", NULL, NULL, &error) == SQLITE_OK,
          error ? error : "first insert failed");

    CHECK(sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, ?)", -1, &stmt, NULL) == SQLITE_OK,
          "prepare insert failed");
    sqlite3_bind_int(stmt, 1, 42);
    sqlite3_bind_text(stmt, 2, "hello", -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE, "bound insert did not finish");
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK, "finalize insert failed");

    CHECK(sqlite3_prepare_v2(db, "SELECT a, b FROM t ORDER BY a", -1, &stmt, NULL) == SQLITE_OK,
          "prepare select failed");
    CHECK(sqlite3_step(stmt) == SQLITE_ROW, "select returned no rows");
    CHECK(sqlite3_column_int(stmt, 0) == 1, "first row value mismatch");
    CHECK(strcmp((const char *)sqlite3_column_text(stmt, 1), "one") == 0,
          "first row text mismatch");
    CHECK(sqlite3_step(stmt) == SQLITE_ROW, "select returned only one row");
    CHECK(sqlite3_column_int(stmt, 0) == 42, "second row value mismatch");
    CHECK(strcmp((const char *)sqlite3_column_text(stmt, 1), "hello") == 0,
          "second row text mismatch");
    CHECK(sqlite3_column_bytes(stmt, 1) == 5, "column_bytes mismatch");
    CHECK(sqlite3_step(stmt) == SQLITE_DONE, "select had extra rows");
    CHECK(sqlite3_finalize(stmt) == SQLITE_OK, "finalize select failed");

    CHECK(sqlite3_exec(db, "SELECT a FROM t", count_callback, &rows, &error) == SQLITE_OK,
          error ? error : "exec select failed");
    CHECK(rows == 2, "exec callback saw the wrong row count");
    CHECK(sqlite3_changes(db) >= 0, "sqlite3_changes failed");
    CHECK(sqlite3_last_insert_rowid(db) > 0, "insert rowid was not reported");
    CHECK(sqlite3_close(db) == SQLITE_OK, "close failed");

    /* Reopen: the second connection must observe the committed rows. */
    ret = sqlite3_open("ux0:/smoke.db", &db);
    CHECK(ret == SQLITE_OK, sqlite3_errmsg(db));
    rows = 0;
    CHECK(sqlite3_exec(db, "SELECT a FROM t", count_callback, &rows, &error) == SQLITE_OK,
          error ? error : "reopen select failed");
    CHECK(rows == 2, "the reopened database lost rows");
    CHECK(sqlite3_close(db) == SQLITE_OK, "second close failed");

    /* Guest-style wrapper: copies the default VFS and replaces xOpen/xDelete. */
    {
        sqlite3_vfs *base = sqlite3_vfs_find(NULL);

        CHECK(base != NULL, "no default VFS was registered");
        memcpy(&guest_vfs, base, sizeof(guest_vfs));
        guest_vfs.zName = "psp2_rw";
        guest_vfs.pAppData = base;
        guest_vfs.xOpen = guest_open;
        guest_vfs.xDelete = guest_delete;

        CHECK(sqlite3_vfs_register(&guest_vfs, 1) == SQLITE_OK, "guest VFS registration failed");

        rows = 0;
        ret = sqlite3_open_v2("ux0:/smoke.db", &db, SQLITE_OPEN_READWRITE, "psp2_rw");
        CHECK(ret == SQLITE_OK, sqlite3_errmsg(db));
        CHECK(sqlite3_exec(db, "INSERT INTO t VALUES(7, 'seven')", NULL, NULL, &error) == SQLITE_OK,
              error ? error : "guest VFS insert failed");
        CHECK(atomic_load(&g_guest_writes) > 0, "the guest xWrite override was never called");
        CHECK(g_guest_bad_fd == 0, "the guest file layout did not expose a valid SceUID");
        CHECK(sqlite3_close(db) == SQLITE_OK, "guest VFS close failed");

        CHECK(sqlite3_vfs_unregister(&guest_vfs) == SQLITE_OK, "guest VFS unregister failed");
    }

    CHECK(sqlite3_vfs_find("psp2") != NULL, "the psp2 VFS disappeared");

    /* Delete through the VFS and confirm the file is gone. */
    {
        sqlite3_vfs *vfs = sqlite3_vfs_find("psp2");
        char host_path[1024];

        CHECK(vfs != NULL, "no psp2 VFS");
        CHECK(vfs->xDelete(vfs, "ux0:/smoke.db", 0) == SQLITE_OK, "VFS delete failed");
        CHECK(host_fs_resolve_path("ux0:/smoke.db", host_path, sizeof(host_path)) == 0,
              "could not resolve the database path");
        CHECK(access(host_path, F_OK) != 0, "the database file still exists");
    }

    CHECK(SceSqlite_finish() == 0, "could not shut SQLite down");
    rmdir(app_dir);
    rmdir(root);

    puts("sqlite_smoke: ok");
    return 0;
}
