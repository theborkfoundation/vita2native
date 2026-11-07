#ifndef LOAD_H
#define LOAD_H

#include <stdint.h>

#include "jit.h"

#define LOAD_MAX_SEGMENTS 4

typedef struct {
    uint32_t perms;
    void *vaddr;
    uint32_t memsz;
    uint32_t filesz;
} LoadedSegment;

typedef struct {
    char name[32];
    char path[256];
    uint16_t attributes;
    void *entry;
    void *module_stop;
    void *module_info;
    void *module_segment;
    void *code_rx;
    void *code_rw;
    void *data_addr;
    LoadedSegment segments[LOAD_MAX_SEGMENTS];
    int num_segments;
} LoadedModuleInfo;

int load_file(Jit *jit, const char *filename, void **entry);
int load_file_info(Jit *jit, const char *filename, void **entry, LoadedModuleInfo *info);

/*
 * Re-run import resolution for an already loaded module. Useful when modules are
 * loaded in an order that does not match their dependency graph: imports that
 * were patched to error stubs resolve to the real entry point once the exporting
 * module has been registered.
 */
int load_repatch_imports(LoadedModuleInfo *info);

#endif
