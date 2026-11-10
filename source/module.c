#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "modules/SceProcessmgr.h"
#include "util.h"

#define MAX_GUEST_LIBRARIES 64
#define MAX_GUEST_DEPENDENTS 32
#define SCE_LIBC_NID 0xbe43bb07u

typedef struct {
    bool used;
    void *owner;
    uint32_t library_nid;
    uint32_t num_funcs;
    uint32_t num_vars;
    const uint32_t *nids;
    const void *const *entries;
    void *dependents[MAX_GUEST_DEPENDENTS];
    uint32_t num_dependents;
} guest_library_t;

static guest_library_t g_guest_libraries[MAX_GUEST_LIBRARIES];
static pthread_mutex_t g_guest_libraries_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_stack_chk_guard = 0x0DEADBEEu;

/*
 * Value of the SceLibcParam:__sce_libcparam variable. SceProcessmgr points this
 * at the process's libc parameters when it initialises.
 */
void *g_sce_libc_param;

/* Value of std::nothrow (an empty tag object). */
static uint32_t g_nothrow_tag;

typedef struct {
    uint32_t library_nid;
    uint32_t var_nid;
    void *address;
} host_var_export_t;

/* Cells published for SceLibc's stdio/ctype variable imports. SceLibc fills
   them at startup; the standalone module tests link without it. */
void *g_vita_libc_stdin_cell;
void *g_vita_libc_stdout_cell;
void *g_vita_libc_stderr_cell;
void *g_vita_libc_touptab_cell;
void *g_vita_libc_tolotab_cell;
void *g_vita_libc_ctype_cell;

static const host_var_export_t g_host_var_exports[] = {
    /* SceLibstdcxx: std::nothrow */
    { 0xf7629240, 0xbfef56b5, &g_nothrow_tag },
    /* SceLibcParam: __sce_libcparam */
    { 0x5ad9c136, 0xdf084dfa, &g_sce_libc_param },
    /* SceLibc: stdio and ctype globals */
    { 0xbe43bb07, 0x66b7406c, &g_vita_libc_stdin_cell },
    { 0xbe43bb07, 0x5d8c1282, &g_vita_libc_stdout_cell },
    { 0xbe43bb07, 0xddf9bb09, &g_vita_libc_stderr_cell },
    { 0xbe43bb07, 0x36878958, &g_vita_libc_touptab_cell },
    { 0xbe43bb07, 0xd662e07c, &g_vita_libc_tolotab_cell },
    { 0xbe43bb07, 0x3ce6109d, &g_vita_libc_ctype_cell },
};

static void add_dependent(guest_library_t *lib, void *importer)
{
    if (!importer || importer == lib->owner)
        return;

    for (uint32_t i = 0; i < lib->num_dependents; i++) {
        if (lib->dependents[i] == importer)
            return;
    }

    if (lib->num_dependents < MAX_GUEST_DEPENDENTS)
        lib->dependents[lib->num_dependents++] = importer;
}

void module_register_guest_library(uint32_t library_nid, uint32_t num_funcs, uint32_t num_vars,
                                   const uint32_t *nids, const void *const *entries, void *owner)
{
    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (lib->used)
            continue;

        lib->used = true;
        lib->owner = owner;
        lib->library_nid = library_nid;
        lib->num_funcs = num_funcs;
        lib->num_vars = num_vars;
        lib->nids = nids;
        lib->entries = entries;
        break;
    }
    pthread_mutex_unlock(&g_guest_libraries_lock);
}

void module_unregister_guest_libraries(void *owner)
{
    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        if (g_guest_libraries[i].used && g_guest_libraries[i].owner == owner)
            memset(&g_guest_libraries[i], 0, sizeof(g_guest_libraries[i]));
    }
    pthread_mutex_unlock(&g_guest_libraries_lock);
}

/*
 * The host mspace wrappers and a title's own dlmalloc can disagree about chunk
 * metadata (Unity links its own copy). Prefer the guest libc's implementation
 * when the title ships one; until the module is loaded, and for titles without
 * it, the host dlmalloc-backed wrappers are the fallback.
 */
static bool is_mspace_func(uint32_t func_nid)
{
    static const uint32_t nids[] = {
        0xcef7c575, /* mspace_create */
        0x30cbbc66, /* mspace_destroy */
        0xe080b96e, /* mspace_malloc */
        0x3cdfd2a3, /* mspace_free */
        0x4633134a, /* mspace_realloc */
        0x52f780dd, /* mspace_memalign */
        0x915da59e, /* mspace_reallocalign */
        0x7ad7a737, /* mspace_malloc_usable_size */
        0xf355f381, /* mspace_is_heap_empty */
    };

    for (size_t i = 0; i < ARRAY_SIZE(nids); i++) {
        if (nids[i] == func_nid)
            return true;
    }

    return false;
}

static int prefer_guest_exports(uint32_t lib_nid)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_PREFER_GUEST_EXPORTS") != NULL;

    if (enabled)
        return lib_nid != SCE_LIBC_NID;

    /* SceLibc stays on the host: Sony's libc module_start expects a real
       Vita kernel and aborts before publishing anything usable here. For Fios
       the situation is the opposite: the application's libfios2.suprx
       implements the full 168-function async API on top of the kernel calls we
       provide, while the host only stands in for a handful of sync entry
       points. Use the module when the title ships it. */
    return lib_nid == 0x3ad4d630;
}

static const void *guest_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer)
{
    const void *result = NULL;

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (!lib->used || lib->library_nid != lib_nid)
            continue;

        for (uint32_t j = 0; j < lib->num_funcs; j++) {
            if (lib->nids[j] == func_nid) {
                result = lib->entries[j];
                add_dependent(lib, importer);
                goto done;
            }
        }
    }

done:
    pthread_mutex_unlock(&g_guest_libraries_lock);
    return result;
}

static const void *host_func_export(uint32_t lib_nid, uint32_t func_nid)
{
    size_t num_libs = NUM_EXPORTED_LIBS;

    for (size_t i = 0; i < num_libs; i++) {
        if (__start_exported_libraries[i].nid == lib_nid) {
            const library_t *const lib = &__start_exported_libraries[i];
            size_t num_funcs = NUM_FUNCS(lib);

            for (size_t j = 0; j < num_funcs; j++) {
                if (lib->func_nidtable_start[j] == func_nid)
                    return lib->func_table_start[j];
            }
        }
    }

    return NULL;
}

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer)
{
    const void *result;

    if (prefer_guest_exports(lib_nid)) {
        result = guest_func_export(lib_nid, func_nid, importer);
        if (result)
            return result;
    }

    if (lib_nid == SCE_LIBC_NID && is_mspace_func(func_nid)) {
        result = guest_func_export(lib_nid, func_nid, importer);
        if (result)
            return result;
    }

    result = host_func_export(lib_nid, func_nid);
    if (result)
        return result;

    if (!prefer_guest_exports(lib_nid))
        return guest_func_export(lib_nid, func_nid, importer);

    return NULL;
}

static void *guest_var_export(uint32_t lib_nid, uint32_t var_nid, void *importer)
{
    void *result = NULL;

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (!lib->used || lib->library_nid != lib_nid)
            continue;

        for (uint32_t j = 0; j < lib->num_vars; j++) {
            uint32_t index = lib->num_funcs + j;

            if (lib->nids[index] == var_nid) {
                result = (void *)lib->entries[index];
                add_dependent(lib, importer);
                goto done;
            }
        }
    }

done:
    pthread_mutex_unlock(&g_guest_libraries_lock);
    return result;
}

void *module_get_var_export(uint32_t lib_nid, uint32_t var_nid, void *importer)
{
    void *result = NULL;

    /* The firmware exports its stack guard as a variable as well as a function. */
    if (lib_nid == 0xCAE9ACE6 && var_nid == 0x93B8AA67)
        return &g_stack_chk_guard;

    for (size_t i = 0; i < ARRAY_SIZE(g_host_var_exports); i++) {
        if (g_host_var_exports[i].library_nid == lib_nid &&
            g_host_var_exports[i].var_nid == var_nid) {
            return g_host_var_exports[i].address;
        }
    }

    return guest_var_export(lib_nid, var_nid, importer);
}

bool module_guest_library_has_dependents(const void *owner)
{
    bool result = false;

    if (!owner)
        return false;

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (lib->used && lib->owner == owner && lib->num_dependents > 0) {
            result = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_guest_libraries_lock);

    return result;
}

void module_remove_importer(void *importer)
{
    if (!importer)
        return;

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (!lib->used)
            continue;

        for (uint32_t j = 0; j < lib->num_dependents;) {
            if (lib->dependents[j] == importer) {
                lib->dependents[j] = lib->dependents[--lib->num_dependents];
                lib->dependents[lib->num_dependents] = NULL;
            } else {
                j++;
            }
        }
    }
    pthread_mutex_unlock(&g_guest_libraries_lock);
}
