#ifndef VITA2NATIVE_FS_H
#define VITA2NATIVE_FS_H

#include <stddef.h>

/*
 * `app_dir` is the host directory backing app0: (either the directory that
 * contains the executable, or the extracted VPK).
 */
int host_fs_init(const char *root, const char *app_dir);
const char *host_fs_root(void);
const char *host_fs_app_dir(void);

/*
 * savedata0: is scoped to the running title, the way the Vita scopes it: each
 * application gets its own directory and cannot address another one's saves.
 * Defaults to the app directory's name, sanitised; call before the guest starts
 * when a different identity is wanted.
 */
void host_fs_set_title_id(const char *title_id);
const char *host_fs_title_id(void);

/*
 * Resolve a Vita path (e.g. "ux0:/data/foo") into a host path rooted at the
 * configured root. Returns 0 on success, -1 on invalid/escaping paths.
 */
int host_fs_resolve_path(const char *vita_path, char *out, size_t out_size);

#endif
