#ifndef VITA2NATIVE_HOST_PSARC_H
#define VITA2NATIVE_HOST_PSARC_H

#include <stddef.h>

/*
 * Materialize a file that the title expects to find through a Fios archive
 * mount. The Vita kernel exposes mounted PSARC contents to every file API; on
 * the host only the guest libfios2 knows about the mount, so a plain fopen()
 * (used by MonoGame's TitleContainer) would miss archived files.
 *
 * `host_path` is the already-resolved host path. When an ancestor directory
 * contains a *.psarc whose manifest holds the same relative path, the entry is
 * extracted to the host cache and its path is written to `out`.
 *
 * Returns 0 on success.
 */
int host_psarc_materialize(const char *host_path, char *out, size_t out_size);

#endif
