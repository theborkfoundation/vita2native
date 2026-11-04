#include "vpk.h"

#include <errno.h>
#include <limits.h>
#include <miniz/miniz.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define VPK_STAMP_NAME ".vpk-stamp"

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "vita2native: could not create '%s': %s\n", path, strerror(errno));
}

/* Create every parent directory of `path` (which is a file path). */
static int make_parent_dirs(const char *path)
{
    char scratch[PATH_MAX];
    size_t len = strlen(path);

    if (len + 1 > sizeof(scratch))
        return -1;

    memcpy(scratch, path, len + 1);

    for (char *p = scratch + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(scratch, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }

    return 0;
}

/*
 * Reject absolute paths, drive-like prefixes, backslashes and any ".."
 * component. ZIP entries are untrusted input: without this a crafted archive
 * could write outside app0:.
 */
static bool entry_name_is_safe(const char *name)
{
    const char *p = name;

    if (!name[0] || name[0] == '/')
        return false;

    if (strchr(name, '\\') || strchr(name, ':'))
        return false;

    while (*p) {
        const char *start;
        size_t len;

        while (*p == '/')
            p++;
        start = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - start);

        if (len == 2 && start[0] == '.' && start[1] == '.')
            return false;
    }

    return true;
}

static int write_stamp(const char *dest_dir, const struct stat *st)
{
    char path[PATH_MAX];
    FILE *fp;

    if (snprintf(path, sizeof(path), "%s/%s", dest_dir, VPK_STAMP_NAME) >= (int)sizeof(path))
        return -1;

    fp = fopen(path, "w");
    if (!fp)
        return -1;

    fprintf(fp, "%lld %lld\n", (long long)st->st_size, (long long)st->st_mtime);
    fclose(fp);

    return 0;
}

static bool stamp_matches(const char *dest_dir, const struct stat *st)
{
    char path[PATH_MAX];
    char expected[64];
    char line[64];
    FILE *fp;
    bool match = false;

    if (snprintf(path, sizeof(path), "%s/%s", dest_dir, VPK_STAMP_NAME) >= (int)sizeof(path))
        return false;

    fp = fopen(path, "r");
    if (!fp)
        return false;

    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(expected, sizeof(expected), "%lld %lld",
                 (long long)st->st_size, (long long)st->st_mtime);
        match = strcmp(line, expected) == 0;
    }

    fclose(fp);
    return match;
}

bool vpk_is_archive(const char *path)
{
    unsigned char magic[4];
    FILE *fp;
    size_t read;

    if (!path)
        return false;

    fp = fopen(path, "rb");
    if (!fp)
        return false;

    read = fread(magic, 1, sizeof(magic), fp);
    fclose(fp);

    return read == sizeof(magic) && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 &&
           magic[3] == 0x04;
}

int vpk_extract(const char *vpk_path, const char *dest_dir)
{
    mz_zip_archive zip;
    struct stat st;
    int ret = -1;

    if (stat(vpk_path, &st) != 0)
        return -1;

    if (stamp_matches(dest_dir, &st))
        return 0;

    fprintf(stderr, "vita2native: extracting VPK assets to '%s'\n", dest_dir);

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, vpk_path, 0)) {
        fprintf(stderr, "vita2native: could not open VPK '%s'\n", vpk_path);
        return -1;
    }

    mz_uint num_files = mz_zip_reader_get_num_files(&zip);

    make_dir(dest_dir);

    for (mz_uint i = 0; i < num_files; i++) {
        char name[PATH_MAX];
        char out_path[PATH_MAX];

        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;

        if (!mz_zip_reader_get_filename(&zip, i, name, sizeof(name))) {
            fprintf(stderr, "vita2native: invalid VPK entry %u\n", (unsigned)i);
            goto done;
        }

        if (!entry_name_is_safe(name)) {
            fprintf(stderr, "vita2native: skipping unsafe VPK entry '%s'\n", name);
            continue;
        }

        if (snprintf(out_path, sizeof(out_path), "%s/%s", dest_dir, name) >= (int)sizeof(out_path))
            goto done;

        if (make_parent_dirs(out_path) < 0) {
            fprintf(stderr, "vita2native: could not create directories for '%s'\n", out_path);
            goto done;
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, out_path, 0)) {
            fprintf(stderr, "vita2native: could not extract '%s'\n", name);
            goto done;
        }
    }

    write_stamp(dest_dir, &st);
    ret = 0;

done:
    mz_zip_reader_end(&zip);
    return ret;
}
