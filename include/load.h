#ifndef LOAD_H
#define LOAD_H

#include <stdint.h>

#include "jit.h"

#define LOAD_MAX_SEGMENTS 4
/* Raw ELF program header count, used to resolve variable-import relocation
   streams (their segment indices follow the program header table). */
#define LOAD_MAX_RAW_SEGMENTS 8

typedef struct {
    uint32_t perms;
    uint32_t vita_vaddr;
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
    void *exidx_top;
    void *exidx_btm;
    void *extab_top;
    void *extab_btm;
    void *tls_init;
    uint32_t tls_filesz;
    uint32_t tls_memsz;
    LoadedSegment segments[LOAD_MAX_SEGMENTS];
    int num_segments;
    /* Per raw program header: host RX/RW base, so variable-import relocations
       can be re-applied after all modules have registered their exports. */
    uintptr_t segment_rx[LOAD_MAX_RAW_SEGMENTS];
    uintptr_t segment_rw[LOAD_MAX_RAW_SEGMENTS];
    int num_raw_segments;
} LoadedModuleInfo;

int load_file(Jit *jit, const char *filename, void **entry);
int load_file_info(Jit *jit, const char *filename, void **entry, LoadedModuleInfo *info);

/*
 * Returns the address exported for `nid` as a variable, or NULL. The main
 * executable publishes its SceProcessParam this way (NID_PROCESS_PARAM), and
 * the firmware hands that block to the process.
 */
void *load_find_export_var(LoadedModuleInfo *info, uint32_t nid);

/*
 * Re-run import resolution for an already loaded module. Useful when modules are
 * loaded in an order that does not match their dependency graph: imports that
 * were patched to error stubs resolve to the real entry point once the exporting
 * module has been registered.
 */
int load_repatch_imports(LoadedModuleInfo *info);

#endif
