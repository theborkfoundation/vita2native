#ifndef VPK_H
#define VPK_H

#include <stdbool.h>

/*
 * A VPK is a ZIP archive with eboot.bin plus the game's assets. The executable
 * is loaded from memory by load_vpk(); assets are extracted into app0: so the
 * existing file I/O layer can serve them unchanged.
 */

bool vpk_is_archive(const char *path);

/*
 * Extract all safe entries of `vpk_path` into `dest_dir`.
 * Extraction is skipped when the archive's size and mtime match the stamp
 * written by a previous successful run.
 *
 * Returns 0 on success, -1 on failure.
 */
int vpk_extract(const char *vpk_path, const char *dest_dir);

#endif
