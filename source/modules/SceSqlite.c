/*
 * SceSqlite: the Vita exposes the stock SQLite C API under its own library.
 *
 * A full host-backed implementation requires vendoring SQLite and emulating
 * Sony's "psp2" VFS. Until then, the entry points used during startup report a
 * clean failure (or a valid empty VFS descriptor) so callers can fall back
 * instead of treating an unresolved-import error as a pointer.
 */

#include <stdint.h>
#include <stddef.h>

#include "module.h"

#define SQLITE_OK       0
#define SQLITE_ERROR    1
#define SQLITE_CANTOPEN 14

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct sqlite3_vfs sqlite3_vfs;

/* Large enough for any 3.60 sqlite3_vfs layout the guest may memcpy. */
static unsigned char g_fake_vfs[128];

EXPORT(SceSqlite, 0x2034E5B7, int, sceSqliteConfigMallocMethods, const void *methods)
{
    (void)methods;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0xC6DD8C3, sqlite3_vfs *, sqlite3_vfs_find, const char *name)
{
    (void)name;
    return (sqlite3_vfs *)g_fake_vfs;
}

EXPORT(SceSqlite, 0x65F53B9C, int, sqlite3_vfs_register, sqlite3_vfs *vfs, int make_default)
{
    (void)vfs;
    (void)make_default;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0x69CF4171, int, sqlite3_vfs_unregister, sqlite3_vfs *vfs)
{
    (void)vfs;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0xA1E98A41, int, sqlite3_open_v2, const char *filename, sqlite3 **db,
       int flags, const char *vfs)
{
    (void)filename;
    (void)flags;
    (void)vfs;
    if (db)
        *db = NULL;
    return SQLITE_CANTOPEN;
}

EXPORT(SceSqlite, 0xBC4BDCF4, int, sqlite3_prepare_v2, sqlite3 *db, const char *sql, int bytes,
       sqlite3_stmt **stmt, const char **tail)
{
    (void)db;
    (void)sql;
    (void)bytes;
    if (stmt)
        *stmt = NULL;
    if (tail)
        *tail = sql;
    return SQLITE_ERROR;
}

EXPORT(SceSqlite, 0xCA8755B7, int, sqlite3_step, sqlite3_stmt *stmt)
{
    (void)stmt;
    return SQLITE_ERROR;
}

EXPORT(SceSqlite, 0xB656B7E2, int, sqlite3_finalize, sqlite3_stmt *stmt)
{
    (void)stmt;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0x7DF94B79, int, sqlite3_close, sqlite3 *db)
{
    (void)db;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0x2371E86A, int, sqlite3_exec, sqlite3 *db, const char *sql, void *callback,
       void *arg, char **errmsg)
{
    (void)db;
    (void)sql;
    (void)callback;
    (void)arg;
    if (errmsg)
        *errmsg = NULL;
    return SQLITE_ERROR;
}

EXPORT(SceSqlite, 0x3F225D62, int, sqlite3_bind_blob, sqlite3_stmt *stmt, int index,
       const void *data, int bytes, void (*destructor)(void *))
{
    (void)stmt;
    (void)index;
    (void)data;
    (void)bytes;
    (void)destructor;
    return SQLITE_OK;
}

EXPORT(SceSqlite, 0xFE237ED7, const void *, sqlite3_column_blob, sqlite3_stmt *stmt, int column)
{
    (void)stmt;
    (void)column;
    return NULL;
}

EXPORT(SceSqlite, 0x36013FE4, int, sqlite3_column_bytes, sqlite3_stmt *stmt, int column)
{
    (void)stmt;
    (void)column;
    return 0;
}

/* Unimplemented entry points report SQLITE_ERROR rather than a bogus success. */
static int64_t sqlite_unimplemented(void)
{
    return SQLITE_ERROR;
}

#include "SceSqlite_stubs.inc"

DECLARE_LIBRARY(SceSqlite, 0x67F02E1D);
