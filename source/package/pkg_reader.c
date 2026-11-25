#include "pkg_reader.h"

#include <errno.h>
#include <openssl/evp.h>
#include <string.h>
#include <sys/stat.h>

#define PKG_HEADER_SIZE 0xC0
#define PKG_EXT_HEADER_SIZE 0x40
#define PKG_MAX_ENTRIES 4096
#define PKG_IO_CHUNK (64u * 1024u)

/* Public per-firmware keys (wiki.henkaku.xyz/vita/Packages#AES_Keys), also used
   by pkg2zip and Vita3K. */
static const uint8_t pkg_vita_2[16] = {0xE3, 0x1A, 0x70, 0xC9, 0xCE, 0x1D, 0xD7, 0x2B,
                                       0xF3, 0xC0, 0x62, 0x29, 0x63, 0xF2, 0xEC, 0xCB};
static const uint8_t pkg_vita_3[16] = {0x42, 0x3A, 0xCA, 0x3A, 0x2B, 0xD5, 0x64, 0x9F,
                                       0x96, 0x86, 0xAB, 0xAD, 0x6F, 0xD8, 0x80, 0x1F};
static const uint8_t pkg_vita_4[16] = {0xAF, 0x07, 0xFD, 0x59, 0x65, 0x25, 0x27, 0xBA,
                                       0xF1, 0x33, 0x89, 0x66, 0x8B, 0x17, 0xD9, 0xEA};

struct v2n_pkg {
    FILE *file;
    uint64_t file_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t file_count;
    uint32_t content_type;
    uint32_t items_offset;
    uint8_t iv[16];
    uint8_t main_key[16];
    char content_id[0x31];
    char title_id[16];
    char title[128];
    v2n_pkg_entry *entries;
};

static uint32_t get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t get_u32le(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((p[1] << 8) | p[0]);
}

static uint64_t get_u64be(const uint8_t *p)
{
    return ((uint64_t)get_u32be(p) << 32) | get_u32be(p + 4);
}

/* The CTR counter is the 16-byte IV plus the block index, big-endian. */
static void ctr_counter(const uint8_t iv[16], uint64_t block, uint8_t out[16])
{
    uint64_t carry = block;

    for (int i = 15; i >= 0; i--) {
        carry += iv[i];
        out[i] = (uint8_t)carry;
        carry >>= 8;
    }
}

static int aes_ecb_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int ok = 0;

    if (ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_EncryptUpdate(ctx, out, &length, in, 16) == 1 &&
        length == 16)
        ok = 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static int aes_ctr_decrypt(const uint8_t key[16], const uint8_t iv[16], uint64_t block,
                           const uint8_t *in, uint8_t *out, size_t size)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    uint8_t counter[16];
    int length = 0;
    int ok = 0;

    ctr_counter(iv, block, counter);
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key, counter) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_DecryptUpdate(ctx, out, &length, in, (int)size) == 1 && (size_t)length == size)
        ok = 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static int read_at(v2n_pkg *pkg, uint64_t offset, void *buffer, size_t size)
{
    if (offset + size > pkg->file_size)
        return 0;
    if (fseeko(pkg->file, (off_t)offset, SEEK_SET) != 0)
        return 0;
    return fread(buffer, 1, size, pkg->file) == size;
}

static void set_error(char *err, size_t err_size, const char *text)
{
    if (err && err_size)
        snprintf(err, err_size, "%s", text);
}

/* Reads one TOC entry name, decrypting it in place. */
static int read_entry_name(v2n_pkg *pkg, uint32_t name_offset, uint32_t name_size, char *name,
                           size_t name_size_buffer)
{
    uint8_t *raw;
    int ok;

    if (name_size == 0 || name_size >= name_size_buffer)
        return 0;
    raw = malloc(name_size + 1);
    if (!raw)
        return 0;
    ok = read_at(pkg, pkg->data_offset + name_offset, raw, name_size) &&
         aes_ctr_decrypt(pkg->main_key, pkg->iv, name_offset / 16, raw, raw, name_size);
    if (ok) {
        memcpy(name, raw, name_size);
        name[name_size] = '\0';
    }
    free(raw);
    return ok;
}

/* Minimal PSF/param.sfo reader: finds TITLE and sets pkg->title. */
static void read_sfo_title(v2n_pkg *pkg, uint64_t sfo_offset, uint32_t sfo_size)
{
    uint8_t buffer[16 * 1024];
    uint32_t keys_offset, values_offset, count;

    /* The info block's sfo offset is absolute (pkg2zip reads it that way),
       unlike the table-of-contents offsets which are relative to data_offset. */
    if (sfo_size < 20 || sfo_size > sizeof(buffer) || !read_at(pkg, sfo_offset, buffer, sfo_size))
        return;
    if (get_u32le(buffer) != 0x46535000)
        return;

    keys_offset = get_u32le(buffer + 8);
    values_offset = get_u32le(buffer + 12);
    count = get_u32le(buffer + 16);

    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= sfo_size; i++) {
        const uint8_t *entry = buffer + 20 + i * 16;
        uint32_t key_offset = keys_offset + get_u16le(entry);
        uint32_t value_offset = values_offset + get_u32le(entry + 12);
        const char *key;
        const char *value;

        if (key_offset >= sfo_size || value_offset >= sfo_size)
            continue;
        key = (const char *)buffer + key_offset;
        value = (const char *)buffer + value_offset;
        if (strncmp(key, "TITLE", 6) == 0 && pkg->title[0] == '\0')
            snprintf(pkg->title, sizeof(pkg->title), "%s", value);
    }
}

v2n_pkg *v2n_pkg_open(const char *path, char *err, size_t err_size)
{
    v2n_pkg *pkg = calloc(1, sizeof(*pkg));
    uint8_t header[PKG_HEADER_SIZE + PKG_EXT_HEADER_SIZE];
    uint8_t key[16];
    uint8_t *toc = NULL;
    uint32_t info_offset, info_count;
    uint32_t items_offset = 0, items_size = 0, sfo_offset = 0, sfo_size = 0;
    uint32_t key_type;
    int content_type = 0;

    if (!pkg) {
        set_error(err, err_size, "out of memory");
        return NULL;
    }
    pkg->file = fopen(path, "rb");
    if (!pkg->file) {
        set_error(err, err_size, strerror(errno));
        free(pkg);
        return NULL;
    }
    fseeko(pkg->file, 0, SEEK_END);
    pkg->file_size = (uint64_t)ftello(pkg->file);
    fseeko(pkg->file, 0, SEEK_SET);

    if (!read_at(pkg, 0, header, sizeof(header))) {
        set_error(err, err_size, "file too small for a pkg header");
        goto fail;
    }
    if (get_u32be(header) != 0x7F504B47u) {
        set_error(err, err_size, "not a pkg (bad magic)");
        goto fail;
    }

    info_offset = get_u32be(header + 0x08);
    info_count = get_u32be(header + 0x0C);
    pkg->file_count = get_u32be(header + 0x14);
    pkg->data_offset = get_u64be(header + 0x20);
    pkg->data_size = get_u64be(header + 0x28);
    memcpy(pkg->content_id, header + 0x30, 0x30);
    pkg->content_id[0x30] = '\0';
    memcpy(pkg->iv, header + 0x70, 16);

    key_type = header[0xE7] & 7;
    switch (key_type) {
    case 2:
        memcpy(key, pkg_vita_2, 16);
        break;
    case 3:
        memcpy(key, pkg_vita_3, 16);
        break;
    case 4:
        memcpy(key, pkg_vita_4, 16);
        break;
    default:
        set_error(err, err_size, "unsupported pkg key type");
        goto fail;
    }
    if (!aes_ecb_encrypt_block(key, pkg->iv, pkg->main_key)) {
        set_error(err, err_size, "AES init failed");
        goto fail;
    }

    for (uint32_t i = 0; i < info_count && i < 64; i++) {
        uint8_t block[8];
        uint8_t payload[16] = {0};
        uint32_t type;
        uint32_t size;
        size_t want;

        if (!read_at(pkg, info_offset, block, sizeof(block))) {
            set_error(err, err_size, "truncated pkg info block");
            goto fail;
        }
        type = get_u32be(block);
        size = get_u32be(block + 4);
        want = size < sizeof(payload) ? size : sizeof(payload);
        if (want && !read_at(pkg, info_offset + 8, payload, want)) {
            set_error(err, err_size, "truncated pkg info block");
            goto fail;
        }
        if (type == 2)
            content_type = (int)get_u32be(payload);
        else if (type == 13) {
            items_offset = get_u32be(payload);
            items_size = get_u32be(payload + 4);
        } else if (type == 14) {
            sfo_offset = get_u32be(payload);
            sfo_size = get_u32be(payload + 4);
        }
        info_offset += 8 + size;
    }
    (void)items_size;
    pkg->items_offset = items_offset;
    pkg->content_type = (uint32_t)content_type;

    if (pkg->file_count == 0 || pkg->file_count > PKG_MAX_ENTRIES) {
        set_error(err, err_size, "unexpected pkg entry count");
        goto fail;
    }

    toc = malloc((size_t)pkg->file_count * 32);
    if (!toc) {
        set_error(err, err_size, "out of memory");
        goto fail;
    }
    if (!read_at(pkg, pkg->data_offset + items_offset, toc, (size_t)pkg->file_count * 32) ||
        !aes_ctr_decrypt(pkg->main_key, pkg->iv, items_offset / 16, toc, toc,
                         (size_t)pkg->file_count * 32)) {
        set_error(err, err_size, "could not read the pkg table of contents");
        goto fail;
    }

    pkg->entries = calloc(pkg->file_count, sizeof(*pkg->entries));
    if (!pkg->entries) {
        set_error(err, err_size, "out of memory");
        goto fail;
    }
    for (uint32_t i = 0; i < pkg->file_count; i++) {
        const uint8_t *entry = toc + (size_t)i * 32;
        uint32_t name_offset = get_u32be(entry);
        uint32_t name_size = get_u32be(entry + 4);

        pkg->entries[i].size = get_u64be(entry + 16);
        pkg->entries[i].data_offset = get_u64be(entry + 8);
        pkg->entries[i].is_dir = entry[27] == 4 || entry[27] == 18;
        if (!read_entry_name(pkg, name_offset, name_size, pkg->entries[i].name,
                             sizeof(pkg->entries[i].name))) {
            set_error(err, err_size, "could not decrypt a file name in the pkg");
            goto fail;
        }
    }
    free(toc);

    if (sfo_size)
        read_sfo_title(pkg, sfo_offset, sfo_size);

    {
        const char *dash = strchr(pkg->content_id, '-');

        if (dash && strlen(dash + 1) >= 9)
            snprintf(pkg->title_id, sizeof(pkg->title_id), "%.9s", dash + 1);
    }
    return pkg;

fail:
    free(toc);
    v2n_pkg_close(pkg);
    return NULL;
}

void v2n_pkg_close(v2n_pkg *pkg)
{
    if (!pkg)
        return;
    if (pkg->file)
        fclose(pkg->file);
    free(pkg->entries);
    free(pkg);
}

const char *v2n_pkg_content_id(const v2n_pkg *pkg)
{
    return pkg->content_id;
}

const char *v2n_pkg_title_id(const v2n_pkg *pkg)
{
    return pkg->title_id;
}

const char *v2n_pkg_title(const v2n_pkg *pkg)
{
    return pkg->title;
}

uint32_t v2n_pkg_content_type(const v2n_pkg *pkg)
{
    return pkg->content_type;
}

size_t v2n_pkg_entry_count(const v2n_pkg *pkg)
{
    return pkg->file_count;
}

const v2n_pkg_entry *v2n_pkg_entry_at(const v2n_pkg *pkg, size_t index)
{
    return index < pkg->file_count ? &pkg->entries[index] : NULL;
}

static int mkdir_p(const char *path)
{
    char buffer[1024];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(buffer))
        return -1;
    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(buffer, 0777) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    if (mkdir(buffer, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int safe_name(const char *name)
{
    if (name[0] == '/' || strstr(name, "../") || strcmp(name, "..") == 0)
        return 0;
    return 1;
}

int v2n_pkg_extract(const v2n_pkg *pkg, const char *dest, char *err, size_t err_size)
{
    uint8_t *buffer = malloc(PKG_IO_CHUNK);
    v2n_pkg *pkg_mut = (v2n_pkg *)pkg;
    int result = -1;

    if (!buffer) {
        set_error(err, err_size, "out of memory");
        return -1;
    }
    if (mkdir_p(dest) != 0) {
        set_error(err, err_size, "could not create the destination directory");
        goto done;
    }

    for (size_t i = 0; i < pkg->file_count; i++) {
        uint64_t data_offset = pkg->entries[i].data_offset;
        uint64_t data_size = pkg->entries[i].size;
        char path[1024];

        if (!safe_name(pkg->entries[i].name)) {
            set_error(err, err_size, "refusing to extract a path with '..'");
            goto done;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dest, pkg->entries[i].name) >= (int)sizeof(path)) {
            set_error(err, err_size, "path too long");
            goto done;
        }

        if (pkg->entries[i].is_dir) {
            if (mkdir_p(path) != 0) {
                set_error(err, err_size, "could not create a directory");
                goto done;
            }
            continue;
        }

        /* Make sure the parent exists; the pkg may not list it. */
        {
            char parent[1024];
            char *slash;

            snprintf(parent, sizeof(parent), "%s", path);
            slash = strrchr(parent, '/');
            if (slash) {
                *slash = '\0';
                if (parent[0] && mkdir_p(parent) != 0) {
                    set_error(err, err_size, "could not create a directory");
                    goto done;
                }
            }
        }

        FILE *out = fopen(path, "wb");
        uint64_t written = 0;

        if (!out) {
            set_error(err, err_size, "could not create an output file");
            goto done;
        }
        while (written < data_size) {
            size_t chunk = (size_t)((data_size - written) < PKG_IO_CHUNK ? (data_size - written)
                                                                        : PKG_IO_CHUNK);
            if (!read_at(pkg_mut, pkg->data_offset + data_offset + written, buffer, chunk) ||
                !aes_ctr_decrypt(pkg->main_key, pkg->iv, (data_offset + written) / 16, buffer,
                                 buffer, chunk) ||
                fwrite(buffer, 1, chunk, out) != chunk) {
                fclose(out);
                set_error(err, err_size, "could not decrypt an entry");
                goto done;
            }
            written += chunk;
        }
        fclose(out);
    }
    result = 0;

done:
    free(buffer);
    return result;
}
