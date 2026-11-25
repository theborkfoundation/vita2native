#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the configured storage root from launcher.toml ([paths] storage),
 * or "storage" when launcher.toml is absent or omits paths.storage.
 */
const char *v2n_storage_root(void);

/* Formats "<storage_root>/<subpath>" into `out`. Returns 0 on success, -1 on truncation. */
int v2n_storage_path(char *out, size_t out_size, const char *subpath);

#ifdef __cplusplus
}
#endif

#endif
