/*
 * Host-side PSARC fallback.
 *
 * A title can mount a PSARC with libfios2 and then read files from it with
 * ordinary libc stdio. The Vita kernel applies the Fios overlay to every file
 * API, but on the host the guest libfios2 keeps the mount to itself. This file
 * closes that gap: when fopen() cannot find a loose file, look for a *.psarc in
 * an ancestor directory, locate the same relative path in its manifest and
 * extract the entry into a cache file.
 *
 * Only the zlib PSARC variant used by the titles tested here is supported.
 */

#include "host_psarc.h"

#include <dirent.h>
#include <errno.h>
#include <miniz/miniz.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#define PSARC_MAX_CACHED 4
#define PSARC_CACHE_DIR VITA2NATIVE_ROOT_PATH "/cache/psarc"
#define PSARC_MAGIC "PSAR"

typedef struct {
    char path[1024];
    time_t mtime;
    off_t size;

    uint32_t entry_size;
    uint32_t num_entries;
    uint32_t block_size;
    uint8_t *entries;
    uint16_t *blocks;
    uint32_t num_blocks;
    char *manifest;
    uint32_t manifest_size;
} Psarc;

static Psarc g_archives[PSARC_MAX_CACHED];
static pthread_mutex_t g_psarc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t entry_field(const Psarc *ps, uint32_t index, size_t offset)
{
    const uint8_t *entry = ps->entries + (size_t)index * ps->entry_size;

    return read_be32(entry + offset);
}

/* Decompress one multi-block entry into `dest` (manifest or cached file). */
static int psarc_read_entry(FILE *file, const Psarc *ps, uint32_t index, uint8_t *dest)
{
    uint32_t usize = entry_field(ps, index, 21);
    uint32_t offset = entry_field(ps, index, 26);
    uint32_t block_index = entry_field(ps, index, 16);
    uint32_t produced = 0;
    uint32_t block = 0;
    uint8_t *compressed = NULL;
    size_t compressed_cap = 0;

    if (fseek(file, (long)offset, SEEK_SET) != 0)
        return -1;

    while (produced < usize) {
        uint32_t chunk = ps->block_size;
        uint32_t compressed_size;

        if (usize - produced < chunk)
            chunk = usize - produced;
        if (block_index + block >= ps->num_blocks)
            goto fail;
        compressed_size = ps->blocks[block_index + block];

        if (compressed_size == 0 || compressed_size == chunk) {
            if (fread(dest + produced, 1, chunk, file) != chunk)
                goto fail;
        } else {
            mz_ulong dest_len = chunk;

            if (compressed_size > compressed_cap) {
                uint8_t *grown = realloc(compressed, compressed_size);

                if (!grown)
                    goto fail;
                compressed = grown;
                compressed_cap = compressed_size;
            }
            if (fread(compressed, 1, compressed_size, file) != compressed_size)
                goto fail;
            if (mz_uncompress(dest + produced, &dest_len, compressed, compressed_size) != MZ_OK ||
                dest_len != chunk) {
                /*
                 * Some blocks are stored raw even though their size differs
                 * from the uncompressed length. Rewind and copy them.
                 */
                if (fseek(file, -(long)compressed_size, SEEK_CUR) != 0)
                    goto fail;
                if (fread(dest + produced, 1, chunk, file) != chunk)
                    goto fail;
            }
        }

        produced += chunk;
        block++;
    }

    free(compressed);
    return 0;

fail:
    free(compressed);
    return -1;
}

static void psarc_free(Psarc *ps)
{
    free(ps->entries);
    free(ps->blocks);
    free(ps->manifest);
    memset(ps, 0, sizeof(*ps));
}

static int psarc_load(const char *path, Psarc *ps)
{
    struct stat st;
    uint8_t header[32];
    FILE *file;
    uint32_t toc_size;
    uint32_t table_offset;
    uint32_t table_size;

    if (stat(path, &st) != 0)
        return -1;

    file = fopen(path, "rb");
    if (!file)
        return -1;

    if (fread(header, 1, sizeof(header), file) != sizeof(header))
        goto fail;
    if (memcmp(header, PSARC_MAGIC, 4) != 0)
        goto fail;

    /* "zlib" in memory; reject the other PSARC codecs for now. */
    if (memcmp(header + 8, "zlib", 4) != 0)
        goto fail;

    toc_size = read_be32(header + 12);
    ps->entry_size = read_be32(header + 16);
    ps->num_entries = read_be32(header + 20);
    ps->block_size = read_be32(header + 24);
    if (ps->entry_size < 30 || ps->num_entries == 0 || ps->block_size == 0 ||
        ps->entry_size > 256 || ps->num_entries > 100000)
        goto fail;

    table_offset = 32u + ps->num_entries * ps->entry_size;
    if (toc_size < table_offset)
        goto fail;
    table_size = toc_size - table_offset;
    ps->num_blocks = table_size / 2u;

    ps->entries = malloc((size_t)ps->num_entries * ps->entry_size);
    ps->blocks = malloc((size_t)ps->num_blocks * sizeof(uint16_t));
    if (!ps->entries || !ps->blocks)
        goto fail;

    if (fseek(file, 32, SEEK_SET) != 0 ||
        fread(ps->entries, 1, (size_t)ps->num_entries * ps->entry_size, file) !=
            (size_t)ps->num_entries * ps->entry_size)
        goto fail;
    if (fseek(file, (long)table_offset, SEEK_SET) != 0)
        goto fail;
    for (uint32_t i = 0; i < ps->num_blocks; i++) {
        uint8_t word[2];

        if (fread(word, 1, 2, file) != 2)
            goto fail;
        ps->blocks[i] = read_be16(word);
    }

    ps->manifest_size = entry_field(ps, 0, 21);
    if (ps->manifest_size == 0 || ps->manifest_size > 8u * 1024u * 1024u)
        goto fail;
    ps->manifest = malloc(ps->manifest_size);
    if (!ps->manifest)
        goto fail;
    if (psarc_read_entry(file, ps, 0, (uint8_t *)ps->manifest) != 0)
        goto fail;

    fclose(file);
    snprintf(ps->path, sizeof(ps->path), "%s", path);
    ps->mtime = st.st_mtime;
    ps->size = st.st_size;
    return 0;

fail:
    fclose(file);
    psarc_free(ps);
    return -1;
}

static Psarc *psarc_get(const char *path)
{
    struct stat st;
    Psarc *slot = NULL;

    if (stat(path, &st) != 0)
        return NULL;

    for (int i = 0; i < PSARC_MAX_CACHED; i++) {
        Psarc *ps = &g_archives[i];

        if (ps->path[0] && strcmp(ps->path, path) == 0 && ps->mtime == st.st_mtime &&
            ps->size == st.st_size)
            return ps;
        if (!ps->path[0] && !slot)
            slot = ps;
    }

    if (!slot)
        slot = &g_archives[0];
    psarc_free(slot);
    if (psarc_load(path, slot) != 0)
        return NULL;
    return slot;
}

static int psarc_find_entry(const Psarc *ps, const char *relative, uint32_t *index)
{
    const char *line = ps->manifest;
    const char *end = ps->manifest + ps->manifest_size;
    uint32_t entry = 1;

    while (line < end && entry < ps->num_entries) {
        const char *newline = memchr(line, '\n', (size_t)(end - line));
        size_t length = newline ? (size_t)(newline - line) : (size_t)(end - line);

        if (length == strlen(relative) && strncasecmp(line, relative, length) == 0) {
            *index = entry;
            return 0;
        }
        line += length + 1;
        entry++;
    }

    return -1;
}

static uint32_t psarc_hash(const char *path, time_t mtime, off_t size, uint32_t offset,
                           uint32_t usize)
{
    uint32_t hash = 2166136261u;
    const char *p;

    for (p = path; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    hash ^= (uint32_t)mtime;
    hash *= 16777619u;
    hash ^= (uint32_t)size;
    hash *= 16777619u;
    hash ^= offset;
    hash *= 16777619u;
    hash ^= usize;
    return hash;
}

static int cache_file(const Psarc *ps, uint32_t index, char *out, size_t out_size)
{
    char temp[1200];
    uint32_t offset = entry_field(ps, index, 26);
    uint32_t usize = entry_field(ps, index, 21);
    uint32_t hash = psarc_hash(ps->path, ps->mtime, ps->size, offset, usize);
    struct stat st;
    FILE *archive;
    FILE *cache;
    uint8_t *data;

    if (snprintf(out, out_size, PSARC_CACHE_DIR "/%08x_%08x.bin", hash, offset) >=
        (int)out_size)
        return -1;

    if (stat(out, &st) == 0 && st.st_size == (off_t)usize)
        return 0;

    mkdir(VITA2NATIVE_ROOT_PATH "/cache", 0755);
    mkdir(PSARC_CACHE_DIR, 0755);

    data = malloc(usize);
    if (!data)
        return -1;

    archive = fopen(ps->path, "rb");
    if (!archive) {
        free(data);
        return -1;
    }
    if (psarc_read_entry(archive, ps, index, data) != 0) {
        fclose(archive);
        free(data);
        return -1;
    }
    fclose(archive);

    if (snprintf(temp, sizeof(temp), "%s.tmp", out) >= (int)sizeof(temp)) {
        free(data);
        return -1;
    }
    cache = fopen(temp, "wb");
    if (!cache) {
        free(data);
        return -1;
    }
    if (fwrite(data, 1, usize, cache) != usize) {
        fclose(cache);
        remove(temp);
        free(data);
        return -1;
    }
    fclose(cache);
    free(data);

    if (rename(temp, out) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}

static int try_archive(const char *archive_path, const char *host_path, const char *dir,
                       char *out, size_t out_size)
{
    Psarc *ps = psarc_get(archive_path);
    const char *relative;
    uint32_t index;

    if (!ps)
        return -1;
    relative = host_path + strlen(dir);
    if (*relative == '/')
        relative++;
    if (!*relative)
        return -1;
    if (psarc_find_entry(ps, relative, &index) != 0)
        return -1;
    if (cache_file(ps, index, out, out_size) != 0)
        return -1;
    return 0;
}

static int scan_directory(const char *dir, const char *host_path, char *out, size_t out_size)
{
    DIR *dirp = opendir(dir);
    struct dirent *entry;

    if (!dirp)
        return -1;
    while ((entry = readdir(dirp)) != NULL) {
        size_t length = strlen(entry->d_name);
        char archive_path[1200];

        if (length < 6 || strcasecmp(entry->d_name + length - 6, ".psarc") != 0)
            continue;
        if (snprintf(archive_path, sizeof(archive_path), "%s/%s", dir, entry->d_name) >=
            (int)sizeof(archive_path))
            continue;
        if (try_archive(archive_path, host_path, dir, out, out_size) == 0) {
            closedir(dirp);
            return 0;
        }
    }
    closedir(dirp);
    return -1;
}

int host_psarc_materialize(const char *host_path, char *out, size_t out_size)
{
    char dir[1024];
    int result = -1;

    if (!host_path || !out || out_size == 0)
        return -1;
    if (snprintf(dir, sizeof(dir), "%s", host_path) >= (int)sizeof(dir))
        return -1;
    {
        char *slash = strrchr(dir, '/');

        if (!slash)
            return -1;
        *slash = '\0';
    }

    pthread_mutex_lock(&g_psarc_lock);
    for (;;) {
        if (scan_directory(dir, host_path, out, out_size) == 0) {
            result = 0;
            break;
        }
        {
            char *slash = strrchr(dir, '/');

            if (!slash || slash == dir)
                break;
            *slash = '\0';
        }
    }
    pthread_mutex_unlock(&g_psarc_lock);
    return result;
}
