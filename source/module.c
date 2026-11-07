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

static const host_var_export_t g_host_var_exports[] = {
    /* SceLibstdcxx: std::nothrow */
    { 0xf7629240, 0xbfef56b5, &g_nothrow_tag },
    /* SceLibcParam: __sce_libcparam */
    { 0x5ad9c136, 0xdf084dfa, &g_sce_libc_param },
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
 * The host mspace shim backs every session with the host heap and ignores the
 * region the guest reserved. Titles that mix the imported mspace calls with
 * their own statically linked dlmalloc (Unity does) then hand host pointers to
 * a foreign allocator, so allow handing the whole family to the guest module.
 */
static bool skip_host_mspace(uint32_t func_nid)
{
    static int enabled = -1;

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

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_NO_HOST_MSPACE") != NULL;
    if (!enabled)
        return false;

    for (size_t i = 0; i < ARRAY_SIZE(nids); i++) {
        if (nids[i] == func_nid)
            return true;
    }

    return false;
}

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer)
{
    size_t num_libs = NUM_EXPORTED_LIBS;
    const void *result = NULL;

    if (!skip_host_mspace(func_nid)) {
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
    }

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
