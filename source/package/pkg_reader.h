/*
 * PlayStation Vita .pkg container reader and decryptor (C).
 *
 * Only the container layer: the AES-128-CTR/ECB pass that turns the pkg into a
 * tree of files. Those files are still PFS-encrypted for modern ("PFS style")
 * packages, which is what main.cpp hands to the psvpfstools engine.
 *
 * The layout and the three public per-firmware keys follow pkg2zip
 * (github.com/mmozeiko/pkg2zip, public domain) and Vita3K's packages/pkg.cpp.
 */
#ifndef V2N_PKG_READER_H
#define V2N_PKG_READER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v2n_pkg v2n_pkg;

typedef struct {
    char name[420];
    uint64_t size;
    uint64_t data_offset; /* relative to the pkg data area */
    int is_dir;
} v2n_pkg_entry;

/* Opens and decrypts the table of contents. NULL on failure (err is filled). */
v2n_pkg *v2n_pkg_open(const char *path, char *err, size_t err_size);
void v2n_pkg_close(v2n_pkg *pkg);

const char *v2n_pkg_content_id(const v2n_pkg *pkg);
/* Title id (e.g. PCSE00294) derived from the content id. */
const char *v2n_pkg_title_id(const v2n_pkg *pkg);
/* TITLE from param.sfo, when present. Empty string otherwise. */
const char *v2n_pkg_title(const v2n_pkg *pkg);
/* Content type from the pkg info block: 0x15 app, 0x16 dlc, 0x1a patch. */
uint32_t v2n_pkg_content_type(const v2n_pkg *pkg);

size_t v2n_pkg_entry_count(const v2n_pkg *pkg);
const v2n_pkg_entry *v2n_pkg_entry_at(const v2n_pkg *pkg, size_t index);

/* Writes the tree below dest (which is created). Returns 0 on success. */
int v2n_pkg_extract(const v2n_pkg *pkg, const char *dest, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
