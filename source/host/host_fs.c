#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static char g_root[PATH_MAX];
static char g_app_dir[PATH_MAX];

static int copy_string(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (src_len + 1 > dst_size)
        return -1;

    memcpy(dst, src, src_len);
    dst[src_len] = '\0';

    return 0;
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "vita2native: could not create '%s': %s\n", path, strerror(errno));
}

static int path_has_parent_component(const char *path)
{
    const char *p = path;

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
            return 1;
    }

    return 0;
}

int host_fs_init(const char *root, const char *app_dir)
{
    char path[PATH_MAX];
    size_t root_len;
    size_t app_len;

    if (!root || !root[0])
        root = "vita2native";

    root_len = strlen(root);
    if (copy_string(g_root, sizeof(g_root), root, root_len) < 0)
        return -1;

    app_len = app_dir ? strlen(app_dir) : 0;
    if (copy_string(g_app_dir, sizeof(g_app_dir), app_len ? app_dir : ".", app_len ? app_len : 1) < 0)
        return -1;

    make_dir(g_root);

    if (snprintf(path, sizeof(path), "%s/ux0", g_root) >= (int)sizeof(path))
        return -1;
    make_dir(path);

    if (snprintf(path, sizeof(path), "%s/savedata", g_root) >= (int)sizeof(path))
        return -1;
    make_dir(path);

    if (snprintf(path, sizeof(path), "%s/system", g_root) >= (int)sizeof(path))
        return -1;
    make_dir(path);

    return 0;
}

const char *host_fs_root(void)
{
    return g_root;
}

const char *host_fs_app_dir(void)
{
    return g_app_dir;
}

int host_fs_resolve_path(const char *vita_path, char *out, size_t out_size)
{
    const char *colon;
    const char *slash;
    const char *rest;
    const char *base;
    size_t device_len;
    int needed;

    if (!vita_path || !out || out_size == 0)
        return -1;

    colon = strchr(vita_path, ':');
    slash = strchr(vita_path, '/');

    if (colon && (!slash || colon < slash)) {
        device_len = (size_t)(colon - vita_path);
        rest = colon + 1;

        if (device_len == 3 && strncmp(vita_path, "ux0", 3) == 0) {
            needed = snprintf(out, out_size, "%s/ux0", g_root);
            if (needed < 0 || (size_t)needed >= out_size)
                return -1;
            base = out;
        } else if (device_len == 4 && strncmp(vita_path, "app0", 4) == 0) {
            base = g_app_dir;
        } else if (device_len == 9 && strncmp(vita_path, "savedata0", 9) == 0) {
            needed = snprintf(out, out_size, "%s/savedata", g_root);
            if (needed < 0 || (size_t)needed >= out_size)
                return -1;
            base = out;
        } else if ((device_len == 3 && strncmp(vita_path, "os0", 3) == 0) ||
                   (device_len == 3 && strncmp(vita_path, "vs0", 3) == 0)) {
            needed = snprintf(out, out_size, "%s/system", g_root);
            if (needed < 0 || (size_t)needed >= out_size)
                return -1;
            base = out;
        } else {
            base = g_root;
        }

        /*
         * For ux0/savedata/os0/vs0 we already wrote the base into `out`.
         * Reconstruct the final path without using `out` as both source and
         * destination.
         */
        if (base == out) {
            size_t base_len = strlen(out);
            if (path_has_parent_component(rest))
                return -1;
            needed = snprintf(out + base_len, out_size - base_len, "%s%s", rest[0] == '/' ? "" : "/",
                              rest);
            if (needed < 0 || (size_t)needed >= out_size - base_len)
                return -1;
            return 0;
        }
    } else {
        base = g_root;
        rest = vita_path;
    }

    if (path_has_parent_component(rest))
        return -1;

    needed = snprintf(out, out_size, "%s%s%s", base, rest[0] == '/' ? "" : "/", rest);
    if (needed < 0 || (size_t)needed >= out_size)
        return -1;

    return 0;
}
