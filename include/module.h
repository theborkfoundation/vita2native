#ifndef MODULE_H
#define MODULE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Float ABI for exports that pass or return float by value. PS Vita binaries
 * use the base AAPCS convention (softfp: floats travel in core registers),
 * while the host is built hard-float. Every ARM export that takes or returns a
 * float/double by value must use this attribute.
 */
#if defined(__arm__)
#define GUEST_SOFTFP_ABI __attribute__((pcs("aapcs")))
#else
#define GUEST_SOFTFP_ABI
#endif

typedef struct {
    const char *name;
    uint32_t nid;
    const uint32_t *func_nidtable_start;
    const uint32_t *func_nidtable_stop;
    const void **func_table_start;
    const void **func_table_stop;
} library_t;

extern const library_t __start_exported_libraries[];
extern const library_t __stop_exported_libraries[];

#define DECLARE_LIBRARY(_name, _nid)                                                               \
    extern const uint32_t __start_##_name##_func_nidtable[];                                       \
    extern const uint32_t __stop_##_name##_func_nidtable[];                                        \
    extern const void *__start_##_name##_func_table[];                                             \
    extern const void *__stop_##_name##_func_table[];                                              \
    static const library_t _name##_library                                                         \
        __attribute__((used, section("exported_libraries"))) = {                                   \
            .name                = #_name,                                                         \
            .nid                 = _nid,                                                           \
            .func_nidtable_start = __start_##_name##_func_nidtable,                                \
            .func_nidtable_stop  = __stop_##_name##_func_nidtable,                                 \
            .func_table_start    = __start_##_name##_func_table,                                   \
            .func_table_stop     = __stop_##_name##_func_table                                     \
        }

#define EXPORT_FUNC(library, nid, func)                                                            \
    static const uint32_t library##_##func##_##nid##_nid                                           \
        __attribute__((used, section(#library "_func_nidtable"))) = nid;                           \
    static const void *const library##_##func##_##nid##_func                                       \
        __attribute__((used, section(#library "_func_table"))) = &func;

#define EXPORT(library, nid, type, name, ...)                                                      \
    type name(__VA_ARGS__);                                                                        \
    EXPORT_FUNC(library, nid, name)                                                               \
    type name(__VA_ARGS__)

/* Same as EXPORT, but for entry points whose float/double arguments and
   results travel in core registers (the guest uses the softfp ABI while the
   host is built hard-float). */
#define EXPORT_SOFTFP(library, nid, type, name, ...)                                                \
    static type name(__VA_ARGS__) GUEST_SOFTFP_ABI;                                                \
    EXPORT_FUNC(library, nid, name)                                                               \
    static type name(__VA_ARGS__)

#define NUM_EXPORTED_LIBS                                                                          \
    (((uintptr_t)&__stop_exported_libraries - (uintptr_t)&__start_exported_libraries) /            \
     sizeof(library_t))
#define NUM_FUNCS(lib)                                                                             \
    (((uintptr_t)lib->func_table_stop - (uintptr_t)lib->func_table_start) / sizeof(void *))

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);
void *module_get_var_export(uint32_t lib_nid, uint32_t var_nid, void *importer);

bool module_guest_library_has_dependents(const void *owner);
void module_remove_importer(void *importer);

void module_register_guest_library(uint32_t library_nid, uint32_t num_funcs, uint32_t num_vars,
                                   const uint32_t *nids, const void *const *entries, void *owner);
void module_unregister_guest_libraries(void *owner);

#endif