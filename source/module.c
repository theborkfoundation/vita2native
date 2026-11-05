#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"

#define MAX_GUEST_LIBRARIES 64

typedef struct {
    bool used;
    void *owner;
    uint32_t library_nid;
    uint32_t num_funcs;
    uint32_t num_vars;
    const uint32_t *nids;
    const void *const *entries;
} guest_library_t;

static guest_library_t g_guest_libraries[MAX_GUEST_LIBRARIES];
static pthread_mutex_t g_guest_libraries_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_stack_chk_guard = 0x0DEADBEEu;

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

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid)
{
    size_t num_libs = NUM_EXPORTED_LIBS;
    const void *result = NULL;

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

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (!lib->used || lib->library_nid != lib_nid)
            continue;

        for (uint32_t j = 0; j < lib->num_funcs; j++) {
            if (lib->nids[j] == func_nid) {
                result = lib->entries[j];
                goto done;
            }
        }
    }

done:
    pthread_mutex_unlock(&g_guest_libraries_lock);
    return result;
}

void *module_get_var_export(uint32_t lib_nid, uint32_t var_nid)
{
    void *result = NULL;

    /* The firmware exports its stack guard as a variable as well as a function. */
    if (lib_nid == 0xCAE9ACE6 && var_nid == 0x93B8AA67)
        return &g_stack_chk_guard;

    pthread_mutex_lock(&g_guest_libraries_lock);
    for (int i = 0; i < MAX_GUEST_LIBRARIES; i++) {
        guest_library_t *lib = &g_guest_libraries[i];

        if (!lib->used || lib->library_nid != lib_nid)
            continue;

        for (uint32_t j = 0; j < lib->num_vars; j++) {
            uint32_t index = lib->num_funcs + j;

            if (lib->nids[index] == var_nid) {
                result = (void *)lib->entries[index];
                goto done;
            }
        }
    }

done:
    pthread_mutex_unlock(&g_guest_libraries_lock);
    return result;
}
