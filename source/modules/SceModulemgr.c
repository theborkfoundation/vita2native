/*
 * SceKernelModulemgr: load and run user modules.
 *
 * Guest modules are ordinary SCE ELF files. The existing loader already knows
 * how to map, relocate and patch their imports; this module keeps each loaded
 * image's JIT and data allocations alive and exposes the start/stop lifecycle.
 *
 * Imports are resolved against the host against the host export table. Exports
 * of loaded modules are not registered yet, so this covers the common addon
 * case (a PRX that only consumes firmware libraries).
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>

#include "fs.h"
#include "host.h"
#include "load.h"
#include "log.h"
#include "module.h"
#include "modules/SceModulemgr.h"
#include "modules/SceSysmem.h"

#define VITA_MODULE_SLOTS 32

typedef struct {
    bool used;
    SceUID uid;
    int refs;
    int state;
    Jit jit;
    void *entry;
    void *stop_entry;
    char host_path[512];
    LoadedModuleInfo loader;
} VitaModuleInfo;

static VitaModuleInfo g_modules[VITA_MODULE_SLOTS];
static pthread_mutex_t g_modules_lock = PTHREAD_MUTEX_INITIALIZER;

static bool trace_module_lookups(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_TRACE_MODULE_LOOKUPS") != NULL;

    return enabled != 0;
}

static int module_start(VitaModuleInfo *module, SceSize args, void *argp, int *status);
static void module_dump_code(const VitaModuleInfo *module);
static void module_apply_debug_patches(VitaModuleInfo *module);

static VitaModuleInfo *module_find_locked(SceUID uid)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        if (g_modules[i].used && g_modules[i].uid == uid)
            return &g_modules[i];
    }

    return NULL;
}

static VitaModuleInfo *module_alloc_slot(void)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (module->used)
            continue;

        memset(module, 0, sizeof(*module));
        module->used = true;
        module->uid = SceSysmem_get_next_uid();
        module->refs = 1;
        return module;
    }

    return NULL;
}

static void module_release_locked(VitaModuleInfo *module)
{
    if (module->loader.module_info)
        module_unregister_guest_libraries(module->loader.module_info);
    if (module->jit.rx)
        jitClose(&module->jit);
    free(module->loader.data_addr);
    memset(module, 0, sizeof(*module));
}

static int resolve_module_path(const char *path, char *out, size_t out_size)
{
    if (!path)
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;

    if (host_fs_resolve_path(path, out, out_size) < 0)
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;

    return 0;
}

static SceUID module_load(const char *path)
{
    VitaModuleInfo *module;
    char host_path[512];
    void *entry = NULL;
    int ret;

    ret = resolve_module_path(path, host_path, sizeof(host_path));
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&g_modules_lock);
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *existing = &g_modules[i];

        if (!existing->used || strcmp(existing->host_path, host_path) != 0)
            continue;

        /* The firmware keeps one image per file: loading it again only takes
           another reference. Unity re-loads its Media/Modules copies even
           though the application loader already started them. */
        existing->refs++;
        pthread_mutex_unlock(&g_modules_lock);
        LOG("Module \"%s\" is already loaded as 0x%x", path, existing->uid);
        return existing->uid;
    }

    module = module_alloc_slot();
    pthread_mutex_unlock(&g_modules_lock);

    if (!module)
        return SCE_KERNEL_ERROR_MODULEMGR_NOMEM;

    if (load_file_info(&module->jit, host_path, &entry, &module->loader) < 0) {
        pthread_mutex_lock(&g_modules_lock);
        module_release_locked(module);
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NOENT;
    }

    snprintf(module->host_path, sizeof(module->host_path), "%s", host_path);
    strncpy(module->loader.path, path, sizeof(module->loader.path) - 1);
    module->loader.path[sizeof(module->loader.path) - 1] = '\0';
    module->entry = entry;
    module->stop_entry = module->loader.module_stop;
    module->state = SCE_KERNEL_MODULE_STATE_READY;

    LOG("Loaded module \"%s\" as 0x%x (entry %p)", module->loader.name, module->uid,
        module->entry);

    module_dump_code(module);
    module_apply_debug_patches(module);

    return module->uid;
}

/* Case-insensitive substring search, for matching module names from the
   environment without depending on strcasestr(). */
static bool contains_ignore_case(const char *haystack, const char *needle)
{
    size_t length;

    if (!haystack || !needle)
        return false;

    length = strlen(needle);
    if (length == 0)
        return false;

    for (; *haystack; haystack++) {
        size_t i = 0;

        while (i < length && haystack[i] &&
               tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == length)
            return true;
    }

    return false;
}

static bool module_matches(const VitaModuleInfo *module, const char *name, size_t length)
{
    char needle[64];

    if (length == 0 || length >= sizeof(needle))
        return false;

    memcpy(needle, name, length);
    needle[length] = '\0';

    return contains_ignore_case(module->loader.name, needle) ||
           contains_ignore_case(module->loader.path, needle) ||
           contains_ignore_case(module->host_path, needle);
}

static void module_apply_patch_list(VitaModuleInfo *module, const char *list, bool breakpoint)
{
    const char *cursor = list;

    while (*cursor) {
        const char *end = strchr(cursor, ',');
        size_t entry_length = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *separator = memchr(cursor, ':', entry_length);

        if (!separator) {
            LOG("Module patch: expected <name>:<offset> in \"%.*s\"", (int)entry_length,
                cursor);
        } else if (module_matches(module, cursor, (size_t)(separator - cursor))) {
            const char *offset_text = separator + 1;
            char *offset_end = NULL;
            unsigned long offset = strtoul(offset_text, &offset_end, 0);

            if (module->jit.rw && offset + 2 <= module->jit.size) {
                uint8_t *target = (uint8_t *)module->jit.rw + offset;

                if (breakpoint) {
                    const uint16_t udf = 0xde00;

                    memcpy(target, &udf, sizeof(udf));
                    LOG("Module %s: planted breakpoint at image+0x%lx", module->loader.name,
                        offset);
                } else if (*offset_end == '=') {
                    unsigned long value = strtoul(offset_end + 1, NULL, 16);
                    const uint16_t halfword = (uint16_t)value;

                    memcpy(target, &halfword, sizeof(halfword));
                    LOG("Module %s: patched image+0x%lx with 0x%04lx", module->loader.name,
                        offset, value);
                }
            }
        }

        if (!end)
            break;
        cursor = end + 1;
    }
}

/*
 * Debug helpers, applied once a module is mapped:
 *   VITA2NATIVE_MODULE_BKPT=<name>:<offset>[,<name>:<offset>...]
 *   VITA2NATIVE_MODULE_PATCH=<name>:<offset>=<hex16>[,...]
 * The name is a case-insensitive substring of the module name or path and the
 * offset is relative to the start of its mapped image.
 */
static void module_apply_debug_patches(VitaModuleInfo *module)
{
    const char *breakpoints = getenv("VITA2NATIVE_MODULE_BKPT");
    const char *patches = getenv("VITA2NATIVE_MODULE_PATCH");

    if (!module->jit.rw || !module->jit.size)
        return;
    if (!breakpoints && !patches)
        return;

    if (breakpoints)
        module_apply_patch_list(module, breakpoints, true);
    if (patches)
        module_apply_patch_list(module, patches, false);

    __builtin___clear_cache((char *)module->jit.rw,
                            (char *)module->jit.rw + module->jit.size);
    if (module->jit.rx && module->jit.rx != module->jit.rw)
        __builtin___clear_cache((char *)module->jit.rx,
                                (char *)module->jit.rx + module->jit.size);
}

static void module_dump_code(const VitaModuleInfo *module)
{
    const char *dir = getenv("VITA2NATIVE_DUMP_MODULE_CODE");
    char path[PATH_MAX];
    FILE *file;

    if (!dir || !module->jit.rx || !module->jit.size)
        return;

    if (snprintf(path, sizeof(path), "%s/%03x-%s.bin", dir, module->uid,
                 module->loader.name) >= (int)sizeof(path))
        return;

    file = fopen(path, "wb");
    if (!file)
        return;

    fwrite(module->jit.rx, 1, module->jit.size, file);
    fclose(file);
    LOG("Dumped module code to %s", path);

    for (int i = 0; i < module->loader.num_segments; i++) {
        const LoadedSegment *segment = &module->loader.segments[i];

        if ((segment->perms & 1) || !segment->vaddr || !segment->memsz)
            continue;

        if (snprintf(path, sizeof(path), "%s/%03x-%s.data%d.bin", dir, module->uid,
                     module->loader.name, i) >= (int)sizeof(path))
            continue;

        file = fopen(path, "wb");
        if (!file)
            continue;

        fwrite(segment->vaddr, 1, segment->memsz, file);
        fclose(file);
        LOG("Dumped module data segment %d (%p, 0x%x) to %s", i, segment->vaddr,
            segment->memsz, path);
    }
}

/* Returns the UID of an already loaded module with the same name, or -1. */
static SceUID module_find_by_name_locked(const char *name, const VitaModuleInfo *self)
{
    if (!name || !name[0])
        return -1;

    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (!module->used || module == self)
            continue;
        if (strcmp(module->loader.name, name) == 0)
            return module->uid;
    }

    return -1;
}

/*
 * The firmware keeps a single instance of each user module. Applications that
 * probe several paths for the same module (Unity loads its IL2CPP assemblies
 * both from app0: and from the module search path) must not get a second copy:
 * duplicate guest export libraries would shadow each other.
 */
static SceUID module_load_unique(const char *path)
{
    SceUID uid = module_load(path);
    SceUID duplicate = -1;

    if (uid < 0)
        return uid;

    pthread_mutex_lock(&g_modules_lock);
    {
        VitaModuleInfo *module = module_find_locked(uid);

        if (module)
            duplicate = module_find_by_name_locked(module->loader.name, module);
    }
    pthread_mutex_unlock(&g_modules_lock);

    if (duplicate >= 0) {
        LOG("Module \"%s\" is already loaded as 0x%x", path, duplicate);
        sceKernelUnloadModule(uid, 0, NULL);
        return duplicate;
    }

    return uid;
}

static bool has_suprx_suffix(const char *name)
{
    size_t length = strlen(name);

    return length > 6 && strcmp(name + length - 6, ".suprx") == 0;
}

/*
 * VITA2NATIVE_SKIP_MODULES is a comma-separated list of module file names that
 * must not be loaded from the application (e.g. "libc.suprx" to use the host
 * implementation only).
 */
static bool is_skipped_module(const char *name)
{
    const char *list = getenv("VITA2NATIVE_SKIP_MODULES");
    const char *cursor;

    if (!list || !name)
        return false;

    cursor = list;
    while (*cursor) {
        const char *separator = strchr(cursor, ',');
        size_t length = separator ? (size_t)(separator - cursor) : strlen(cursor);

        if (length > 0 && strncmp(cursor, name, length) == 0 && name[length] == '\0')
            return true;

        if (!separator)
            break;
        cursor = separator + 1;
    }

    return false;
}

/*
 * Names listed in VITA2NATIVE_SKIP_MODULE_START are loaded (so their exports
 * are available) but their module_start is not called.
 */
static bool should_skip_start(const char *name)
{
    const char *list = getenv("VITA2NATIVE_SKIP_MODULE_START");
    const char *cursor;

    if (!list || !name || !name[0])
        return false;

    cursor = list;
    while (*cursor) {
        const char *separator = strchr(cursor, ',');
        size_t length = separator ? (size_t)(separator - cursor) : strlen(cursor);

        if (length > 0 && strncmp(cursor, name, length) == 0 && name[length] == '\0')
            return true;

        if (!separator)
            break;
        cursor = separator + 1;
    }

    return false;
}

static int module_name_compare(const void *a, const void *b)
{
    const char *lhs = a;
    const char *rhs = b;
    bool lhs_pthread = strncmp(lhs, "pthread", 7) == 0;
    bool rhs_pthread = strncmp(rhs, "pthread", 7) == 0;

    /* IL2CPP modules import the bundled pthread; start it first. */
    if (lhs_pthread != rhs_pthread)
        return lhs_pthread ? -1 : 1;

    return strcmp(lhs, rhs);
}

static int load_modules_in_dir(const char *subdir)
{
    char dir_path[PATH_MAX];
    char (*names)[256] = NULL;
    size_t count = 0;
    size_t capacity = 0;
    DIR *dir;
    struct dirent *entry;
    int loaded = 0;

    if (snprintf(dir_path, sizeof(dir_path), "%s/%s", host_fs_app_dir(), subdir) >=
        (int)sizeof(dir_path))
        return 0;

    dir = opendir(dir_path);
    if (!dir)
        return 0;

    while ((entry = readdir(dir))) {
        char(*grown)[256];

        if (entry->d_name[0] == '.' || !has_suprx_suffix(entry->d_name))
            continue;

        if (is_skipped_module(entry->d_name)) {
            LOG("Skipping \"%s/%s\": excluded by VITA2NATIVE_SKIP_MODULES", subdir,
                entry->d_name);
            continue;
        }

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            grown = realloc(names, capacity * sizeof(*names));
            if (!grown)
                break;
            names = grown;
        }

        snprintf(names[count++], sizeof(names[0]), "%s", entry->d_name);
    }
    closedir(dir);

    qsort(names, count, sizeof(*names), module_name_compare);

    for (size_t i = 0; i < count; i++) {
        char vita_path[512];
        SceUID uid;

        if (snprintf(vita_path, sizeof(vita_path), "app0:%s/%s", subdir, names[i]) >=
            (int)sizeof(vita_path))
            continue;

        uid = module_load_unique(vita_path);
        if (uid < 0) {
            LOG("Could not load user module \"%s\" (0x%08x)", vita_path, uid);
            continue;
        }

        loaded++;
    }

    free(names);
    return loaded;
}

int SceModulemgr_load_app_modules(void)
{
    static const char *const dirs[] = { "sce_module", "Media/Modules" };
    int loaded = 0;

    if (getenv("VITA2NATIVE_NO_MODULE_AUTOLOAD"))
        return 0;

    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++)
        loaded += load_modules_in_dir(dirs[i]);

    LOG("Loaded %d user module(s) shipped with the application", loaded);

    return loaded;
}

void SceModulemgr_repatch_modules(void)
{
    pthread_mutex_lock(&g_modules_lock);
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        if (!g_modules[i].used)
            continue;
        load_repatch_imports(&g_modules[i].loader);
    }
    pthread_mutex_unlock(&g_modules_lock);
}

int SceModulemgr_start_modules(void)
{
    int started = 0;

    pthread_mutex_lock(&g_modules_lock);
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];
        int status;

        if (!module->used || module->state != SCE_KERNEL_MODULE_STATE_READY)
            continue;

        if (should_skip_start(module->loader.name)) {
            LOG("Skipping module_start for \"%s\"", module->loader.name);
            module->state = SCE_KERNEL_MODULE_STATE_STARTED;
            continue;
        }

        module->refs++;
        pthread_mutex_unlock(&g_modules_lock);

        if (module_start(module, 0, NULL, &status) == 0)
            started++;

        pthread_mutex_lock(&g_modules_lock);
        module->refs--;
    }
    pthread_mutex_unlock(&g_modules_lock);

    return started;
}

typedef struct {
    int (*entry)(SceSize, void *);
    SceSize args;
    void *argp;
    int result;
} ModuleStartCall;

static void *module_start_thread(void *argument)
{
    ModuleStartCall *call = argument;

    call->result = call->entry(call->args, call->argp);

    return NULL;
}

static int module_start(VitaModuleInfo *module, SceSize args, void *argp, int *status)
{
    ModuleStartCall call;
    pthread_t thread;
    int ret;

    if (!module->entry)
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD_ENTRY;

    call.entry = (int (*)(SceSize, void *))module->entry;
    call.args = args;
    call.argp = argp;
    call.result = 0;

    /*
     * The firmware runs module_start on a dedicated thread, and some modules
     * (libc.suprx among them) end it with sceKernelExitThread instead of
     * returning. Join the thread so a normal exit still counts as success.
     */
    LOG("Calling module_start for \"%s\" at %p", module->loader.name, module->entry);

    if (pthread_create(&thread, NULL, module_start_thread, &call) == 0) {
        pthread_join(thread, NULL);
        ret = call.result;
    } else {
        ret = call.entry(call.args, call.argp);
    }

    LOG("module_start for \"%s\" returned 0x%x", module->loader.name, ret);

    if (status)
        *status = ret;

    if (ret < 0)
        return SCE_KERNEL_ERROR_MODULEMGR_START_FAILED;

    module->state = SCE_KERNEL_MODULE_STATE_STARTED;
    return 0;
}

static int module_stop(VitaModuleInfo *module, SceSize args, void *argp, int *status)
{
    int ret;

    if (module->stop_entry) {
        ret = ((int (*)(SceSize, void *))module->stop_entry)(args, argp);
        if (status)
            *status = ret;
        if (ret < 0)
            return SCE_KERNEL_ERROR_MODULEMGR_STOP_FAIL;
    } else if (status) {
        *status = 0;
    }

    module->state = SCE_KERNEL_MODULE_STATE_ENDED;
    return 0;
}

static int module_get_info(VitaModuleInfo *module, SceKernelModuleInfo *info)
{
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->modid = module->uid;
    info->modattr = module->loader.attributes;
    info->modver[0] = 1;
    info->modver[1] = 1;
    strncpy(info->module_name, module->loader.name, sizeof(info->module_name) - 1);
    info->start_entry = module->entry;
    info->stop_entry = module->stop_entry;
    info->exidx_top = module->loader.exidx_top;
    info->exidx_btm = module->loader.exidx_btm;
    info->extab_top = module->loader.extab_top;
    info->extab_btm = module->loader.extab_btm;
    info->tlsInit = module->loader.tls_init;
    info->tlsInitSize = module->loader.tls_filesz;
    info->tlsAreaSize = module->loader.tls_memsz;
    strncpy(info->path, module->loader.path, sizeof(info->path) - 1);
    info->state = (SceUInt)module->state;

    for (int i = 0; i < module->loader.num_segments && i < 4; i++) {
        info->segments[i].size = sizeof(info->segments[i]);
        info->segments[i].perms = module->loader.segments[i].perms;
        info->segments[i].vaddr = module->loader.segments[i].vaddr;
        info->segments[i].memsz = module->loader.segments[i].memsz;
        info->segments[i].filesz = module->loader.segments[i].filesz;
    }

    return 0;
}

EXPORT(SceLibKernel, 0xBBE82155, SceUID, sceKernelLoadModule, const char *path, int flags,
       SceKernelLMOption *option)
{
    (void)flags;
    (void)option;
    if (trace_module_lookups())
        LOG("LoadModule(\"%s\")", path ? path : "(null)");
    return module_load(path);
}

EXPORT(SceModulemgr, 0xB4C5EF9E, SceUID, _sceKernelLoadModule, const char *path, SceUInt32 flags,
       const SceKernelLoadModuleOption *option)
{
    (void)flags;
    (void)option;
    return module_load(path);
}

EXPORT(SceLibKernel, 0x702425D5, int, sceKernelStartModule, SceUID modid, SceSize args, void *argp,
       int flags, void *option, int *status)
{
    VitaModuleInfo *module;
    int ret;

    (void)flags;
    (void)option;

    if (trace_module_lookups())
        LOG("StartModule(0x%x)", modid);

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }
    if (module->state == SCE_KERNEL_MODULE_STATE_STARTED) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_STARTED;
    }
    module->refs++;
    pthread_mutex_unlock(&g_modules_lock);

    ret = module_start(module, args, argp, status);

    pthread_mutex_lock(&g_modules_lock);
    module->refs--;
    pthread_mutex_unlock(&g_modules_lock);
    return ret;
}

EXPORT(SceModulemgr, 0x72CD301F, int, _sceKernelStartModule, SceUID modid, SceSize args,
       void *argp, SceUInt32 flags, const void *option, int *status)
{
    return sceKernelStartModule(modid, args, argp, flags, (void *)option, status);
}

EXPORT(SceLibKernel, 0x2DCC4AFA, SceUID, sceKernelLoadStartModule, const char *path, SceSize args,
       void *argp, int flags, SceKernelLMOption *option, int *status)
{
    SceUID modid;
    int ret;

    if (trace_module_lookups())
        LOG("LoadStartModule(\"%s\")", path ? path : "(null)");

    modid = module_load(path);

    if (modid < 0)
        return modid;

    pthread_mutex_lock(&g_modules_lock);
    {
        VitaModuleInfo *already = module_find_locked(modid);

        if (already && already->state == SCE_KERNEL_MODULE_STATE_STARTED) {
            pthread_mutex_unlock(&g_modules_lock);
            LOG("LoadStartModule: 0x%x is already started", modid);
            if (status)
                *status = 0;
            return modid;
        }
    }
    pthread_mutex_unlock(&g_modules_lock);

    ret = sceKernelStartModule(modid, args, argp, flags, option, status);
    if (ret < 0) {
        sceKernelUnloadModule(modid, 0, NULL);
        return ret;
    }

    return modid;
}

EXPORT(SceModulemgr, 0x60647592, SceUID, _sceKernelLoadStartModule, const char *path,
       SceSize args, const void *argp, SceUInt32 flags)
{
    return sceKernelLoadStartModule(path, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x3B2CBA09, int, sceKernelStopModule, SceUID modid, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    VitaModuleInfo *module;
    int ret;

    (void)flags;
    (void)option;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }
    module->refs++;
    pthread_mutex_unlock(&g_modules_lock);

    ret = module_stop(module, args, argp, status);

    pthread_mutex_lock(&g_modules_lock);
    module->refs--;
    pthread_mutex_unlock(&g_modules_lock);
    return ret;
}

EXPORT(SceModulemgr, 0x086867A8, int, _sceKernelStopModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelStopModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x1987920E, int, sceKernelUnloadModule, SceUID modid, int flags,
       SceKernelULMOption *option)
{
    VitaModuleInfo *module;

    (void)flags;
    (void)option;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    if (!module) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    }

    if (module->refs > 1) {
        module->refs--;
        pthread_mutex_unlock(&g_modules_lock);
        return 0;
    }

    if (module_guest_library_has_dependents(module->loader.module_info)) {
        pthread_mutex_unlock(&g_modules_lock);
        return SCE_KERNEL_ERROR_MODULEMGR_IN_USE;
    }

    module_remove_importer(module->loader.module_info);
    module_release_locked(module);
    pthread_mutex_unlock(&g_modules_lock);

    return 0;
}

EXPORT(SceModulemgr, 0x8E4A7716, int, _sceKernelUnloadModule, SceUID modid, SceUInt32 flags,
       const SceKernelUnloadModuleOption *option)
{
    (void)option;
    return sceKernelUnloadModule(modid, flags, NULL);
}

EXPORT(SceLibKernel, 0x2415F8A4, int, sceKernelStopUnloadModule, SceUID modid, SceSize args,
       void *argp, int flags, SceKernelULMOption *option, int *status)
{
    int ret = sceKernelStopModule(modid, args, argp, flags, option, status);

    if (ret < 0)
        return ret;

    return sceKernelUnloadModule(modid, 0, NULL);
}

EXPORT(SceModulemgr, 0x86EAEA0A, int, _sceKernelStopUnloadModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelStopUnloadModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x4F2D8B15, SceUID, sceKernelOpenModule, const char *path, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    (void)args;
    (void)argp;
    (void)flags;
    (void)option;
    (void)status;

    return module_load(path);
}

EXPORT(SceModulemgr, 0x9D674F45, SceUID, _sceKernelOpenModule, const char *path, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelOpenModule(path, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceLibKernel, 0x657FA50E, int, sceKernelCloseModule, SceUID modid, SceSize args,
       void *argp, int flags, void *option, int *status)
{
    (void)args;
    (void)argp;
    (void)flags;
    (void)option;
    (void)status;

    return sceKernelUnloadModule(modid, 0, NULL);
}

EXPORT(SceModulemgr, 0x849E78BE, int, _sceKernelCloseModule, SceUID modid, SceSize args,
       const void *argp, SceUInt32 flags)
{
    return sceKernelCloseModule(modid, args, (void *)argp, flags, NULL, NULL);
}

EXPORT(SceModulemgr, 0x36585DAF, int, sceKernelGetModuleInfo, SceUID modid,
       SceKernelModuleInfo *info)
{
    VitaModuleInfo *module;
    int ret;

    pthread_mutex_lock(&g_modules_lock);
    module = module_find_locked(modid);
    ret = module ? module_get_info(module, info) : SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
    if (trace_module_lookups()) {
        static int traced;

        if (traced++ < 32)
            LOG("GetModuleInfo(0x%x) -> 0x%x (%s)", modid, ret,
                module ? module->loader.name : "unknown");
    }
    pthread_mutex_unlock(&g_modules_lock);

    return ret;
}

EXPORT(SceLibKernel, 0xD11A5103, int, sceKernelGetModuleInfoByAddr, void *addr,
       SceKernelModuleInfo *info)
{
    int ret = SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;

    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (!module->used)
            continue;
        if (addr >= module->loader.code_rx &&
            (uintptr_t)addr < (uintptr_t)module->loader.code_rx +
                                 (uintptr_t)module->jit.size) {
            ret = module_get_info(module, info);

            if (trace_module_lookups()) {
                static int traced;

                if (traced++ < 32)
                    LOG("GetModuleInfoByAddr(%p) -> %s exidx %p-%p extab %p-%p", addr,
                        module->loader.name, info ? info->exidx_top : NULL,
                        info ? info->exidx_btm : NULL, info ? info->extab_top : NULL,
                        info ? info->extab_btm : NULL);
            }

            return ret;
        }
    }

    if (trace_module_lookups()) {
        static int traced;

        if (traced++ < 32)
            LOG("GetModuleInfoByAddr(%p) -> no module", addr);
    }

    return ret;
}

EXPORT(SceModulemgr, 0xF5798C7C, SceUID, sceKernelGetModuleIdByAddr, void *addr)
{
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        VitaModuleInfo *module = &g_modules[i];

        if (!module->used)
            continue;
        if (addr >= module->loader.code_rx &&
            (uintptr_t)addr < (uintptr_t)module->loader.code_rx +
                                 (uintptr_t)module->jit.size) {
            if (trace_module_lookups()) {
                static int traced;

                if (traced++ < 32)
                    LOG("GetModuleIdByAddr(%p) -> 0x%x (%s)", addr, module->uid,
                        module->loader.name);
            }

            return module->uid;
        }
    }

    if (trace_module_lookups()) {
        static int traced;

        if (traced++ < 32)
            LOG("GetModuleIdByAddr(%p) -> no module", addr);
    }

    return SCE_KERNEL_ERROR_MODULEMGR_NO_MOD;
}

EXPORT(SceModulemgr, 0x2EF2581F, int, sceKernelGetModuleList, SceUInt8 type, SceUID *uids,
       SceSize *num)
{
    SceSize capacity;
    SceSize count = 0;

    (void)type;

    if (!num)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    capacity = *num;

    pthread_mutex_lock(&g_modules_lock);
    for (int i = 0; i < VITA_MODULE_SLOTS; i++) {
        if (!g_modules[i].used)
            continue;
        if (uids && count < capacity)
            uids[count] = g_modules[i].uid;
        count++;
    }
    pthread_mutex_unlock(&g_modules_lock);

    *num = count;
    if (uids && count > capacity)
        return SCE_KERNEL_ERROR_MODULEMGR_NOMEM;

    return 0;
}

EXPORT(SceModulemgr, 0x85E6D2BB, int, sceKernelIsCalledFromSysModule, void *lr)
{
    (void)lr;
    return 0;
}

EXPORT(SceModulemgr, 0x4397FC4E, SceUInt32, sceKernelGetAllowedSdkVersionOnSystem, void)
{
    return 0x03570011;
}

EXPORT(SceModulemgr, 0x5182E212, int, sceKernelGetSystemSwVersion, SceKernelSystemSwVersion *version)
{
    if (!version)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(version, 0, sizeof(*version));
    version->size = sizeof(*version);
    strncpy(version->versionString, "3.60", sizeof(version->versionString) - 1);
    version->version = 0x03600000;

    return 0;
}

EXPORT(SceModulemgr, 0x6CED1F63, int, sceKernelInhibitLoadingModule, SceUInt16 module_id)
{
    (void)module_id;
    return 0;
}

/*
 * The loader does not retain its export-library table after relocating a
 * module, so there is nothing reliable to report here. Fail like the firmware
 * does for an unknown library instead of fabricating a descriptor.
 */
EXPORT(SceModulemgr, 0xEAEB1312, int, sceKernelGetLibraryInfoByNID, SceUID modid,
       SceNID libnid, SceKernelLibraryInfo *info)
{
    (void)modid;
    (void)libnid;
    (void)info;

    return SCE_KERNEL_ERROR_MODULEMGR_NOENT;
}

DECLARE_LIBRARY(SceModulemgr, 0xeaed1616);
