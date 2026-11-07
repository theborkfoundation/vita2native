/*
 * SceLibc, SceLibm and SceLibstdcxx.
 *
 * These are ordinary C library entry points. Guest code runs natively on the
 * host CPU, so they map straight onto the host C library: a guest `malloc`
 * returns a host pointer, a guest `FILE *` is a host `FILE *`, and so on.
 *
 * This replaces the `libc.suprx` shipped in some VPKs. That module is a system
 * library: its initialisation expects the Vita kernel to have set up its heap
 * and TLS and it crashes before publishing anything usable.
 *
 * The exported wrappers are prefixed with `vita_` so they never shadow the host
 * C library symbols that they call into.
 */

#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <psp2/types.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "fs.h"
#include "log.h"
#include "module.h"

#define SCE_LIBC_NID 0xbe43bb07
#define SCE_LIBM_NID 0xcdae3c7d
#define SCE_LIBSTDCXX_NID 0xf7629240

/* ------------------------------------------------------------------------- */
/* Path translation                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Guest paths use the Vita namespace ("ux0:/data/x", "app0:/..."). Translate
 * them to the host directory that backs the matching device.
 */
static const char *host_path(const char *path, char *buffer, size_t size)
{
    if (!path)
        return path;

    if (host_fs_resolve_path(path, buffer, size) == 0)
        return buffer;

    return path;
}

/* ------------------------------------------------------------------------- */
/* errno, TLS                                                                */
/* ------------------------------------------------------------------------- */

static int g_errno_storage;

EXPORT(SceLibc, 0x9e248b76, int *, _sceLibcErrnoLoc, void)
{
    return &g_errno_storage;
}

#define MAX_TLS_MODULES 32
#define TLS_BLOCK_SIZE 4096

static uint8_t *g_tls_blocks[MAX_TLS_MODULES];

static uint8_t *tls_block(uint32_t module)
{
    if (module >= MAX_TLS_MODULES)
        return NULL;

    if (!g_tls_blocks[module]) {
        g_tls_blocks[module] = calloc(1, TLS_BLOCK_SIZE);
        if (!g_tls_blocks[module])
            return NULL;
    }

    return g_tls_blocks[module];
}

typedef struct {
    uint32_t module;
    uint32_t offset;
} SceTlsIndex;

EXPORT(SceLibc, 0xbc529b7b, void *, vita_tls_get_addr, SceTlsIndex * index)
{
    uint8_t *block;

    if (!index)
        return NULL;

    /*
     * Guest modules index their TLS area with whatever identifier the loader
     * handed out. Titles link against a fixed table, so the index can exceed
     * our slot count; fold it instead of returning NULL (callers dereference
     * the result unconditionally).
     */
    block = tls_block(index->module % MAX_TLS_MODULES);
    if (!block)
        return NULL;

    return block + (index->offset % TLS_BLOCK_SIZE);
}

EXPORT(SceLibc, 0x54f87eac, int, _sceLdTlsRegisterModuleInfo, void *info)
{
    (void)info;

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Strings and memory                                                        */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibc, 0x7205bfdb, void *, vita_memcpy, void *dst, const void *src, size_t size)
{
    return memcpy(dst, src, size);
}

EXPORT(SceLibc, 0xaf5c218d, void *, vita_memmove, void *dst, const void *src, size_t size)
{
    return memmove(dst, src, size);
}

EXPORT(SceLibc, 0x6dc1f0d8, void *, vita_memset, void *dst, int value, size_t size)
{
    return memset(dst, value, size);
}

EXPORT(SceLibc, 0x7747f6d7, int, vita_memcmp, const void *lhs, const void *rhs, size_t size)
{
    return memcmp(lhs, rhs, size);
}

EXPORT(SceLibc, 0x2f3e5b16, void *, vita_memchr, const void *block, int value, size_t size)
{
    return memchr(block, value, size);
}

EXPORT(SceLibc, 0x8aecc873, size_t, vita_strlen, const char *s)
{
    return strlen(s);
}

EXPORT(SceLibc, 0x85b924b7, char *, vita_strcpy, char *dst, const char *src)
{
    return strcpy(dst, src);
}

EXPORT(SceLibc, 0x9f87712d, char *, vita_strncpy, char *dst, const char *src, size_t size)
{
    return strncpy(dst, src, size);
}

EXPORT(SceLibc, 0x1434fa46, char *, vita_strcat, char *dst, const char *src)
{
    return strcat(dst, src);
}

EXPORT(SceLibc, 0xfba69bc2, char *, vita_strncat, char *dst, const char *src, size_t size)
{
    return strncat(dst, src, size);
}

EXPORT(SceLibc, 0x1b58fa3b, int, vita_strcmp, const char *lhs, const char *rhs)
{
    return strcmp(lhs, rhs);
}

EXPORT(SceLibc, 0xe4299dcb, int, vita_strncmp, const char *lhs, const char *rhs, size_t size)
{
    return strncmp(lhs, rhs, size);
}

EXPORT(SceLibc, 0x184c4b07, int, vita_strcasecmp, const char *lhs, const char *rhs)
{
    return strcasecmp(lhs, rhs);
}

EXPORT(SceLibc, 0xaf1ca2f1, int, vita_strncasecmp, const char *lhs, const char *rhs, size_t size)
{
    return strncasecmp(lhs, rhs, size);
}

EXPORT(SceLibc, 0xb9336e16, char *, vita_strchr, const char *s, int c)
{
    return strchr(s, c);
}

EXPORT(SceLibc, 0xcefdd143, char *, vita_strrchr, const char *s, int c)
{
    return strrchr(s, c);
}

EXPORT(SceLibc, 0x0d5200cb, char *, vita_strstr, const char *haystack, const char *needle)
{
    return strstr(haystack, needle);
}

EXPORT(SceLibc, 0x68c307b6, char *, vita_strpbrk, const char *s, const char *accept)
{
    return strpbrk(s, accept);
}

EXPORT(SceLibc, 0x0e29d27a, size_t, vita_strcspn, const char *s, const char *reject)
{
    return strcspn(s, reject);
}

EXPORT(SceLibc, 0x0289b8b3, char *, vita_strtok, char *s, const char *delim)
{
    return strtok(s, delim);
}

EXPORT(SceLibc, 0xff6f77c7, char *, vita_strdup, const char *s)
{
    return strdup(s);
}

EXPORT(SceLibc, 0x181827ed, long, vita_strtol, const char *s, char **end, int base)
{
    return strtol(s, end, base);
}

EXPORT(SceLibc, 0x1e9d6335, char *, vita_strerror, int error)
{
    return strerror(error);
}

EXPORT(SceLibc, 0x8e5a06c5, int, vita_abs, int value)
{
    return value < 0 ? -value : value;
}

EXPORT(SceLibc, 0xe9f823c0, div_t, vita_div, int numerator, int denominator)
{
    return div(numerator, denominator);
}

/* ------------------------------------------------------------------------- */
/* C++ runtime support                                                       */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibc, 0xbf94193b, double, _Stod, const char *s, char **end)
{
    return strtod(s, end);
}

EXPORT(SceLibc, 0xe71c5cde, unsigned long, _Stoul, const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

EXPORT(SceLibc, 0x6794b3c6, unsigned long long, _Stoull, const char *s, char **end, int base)
{
    return strtoull(s, end, base);
}

#define MAX_ATEXIT 64

typedef struct {
    void (*function)(void *);
    void *argument;
    void *dso;
} SceAtExitEntry;

static SceAtExitEntry g_atexit[MAX_ATEXIT];
static int g_atexit_count;

static void run_atexit(void)
{
    while (g_atexit_count > 0) {
        SceAtExitEntry entry = g_atexit[--g_atexit_count];

        if (entry.function)
            entry.function(entry.argument);
    }
}

static int register_atexit(void (*function)(void *), void *argument, void *dso)
{
    if (g_atexit_count == 0)
        atexit(run_atexit);

    if (g_atexit_count >= MAX_ATEXIT)
        return -1;

    g_atexit[g_atexit_count].function = function;
    g_atexit[g_atexit_count].argument = argument;
    g_atexit[g_atexit_count].dso = dso;
    g_atexit_count++;

    return 0;
}

EXPORT(SceLibc, 0x33b83b70, int, vita_cxa_atexit, void (*function)(void *), void *argument,
       void *dso)
{
    return register_atexit(function, argument, dso);
}

EXPORT(SceLibc, 0xedc939e1, int, vita_aeabi_atexit, void *object, void (*function)(void *),
       void *dso)
{
    return register_atexit(function, object, dso);
}

EXPORT(SceLibc, 0xba2ef93e, int, vita_at_quick_exit, void (*function)(void))
{
    return atexit(function);
}

EXPORT(SceLibc, 0xbfe02b3a, void, vita_cxa_set_dso_handle_main, void *handle)
{
    (void)handle;
}

EXPORT(SceLibc, 0xd0310e31, int, vita_cxa_guard_acquire, int *guard)
{
    if (!guard)
        return 0;

    if (__atomic_load_n(guard, __ATOMIC_ACQUIRE) & 1)
        return 0;

    __atomic_store_n(guard, 1, __ATOMIC_RELEASE);

    return 1;
}

EXPORT(SceLibc, 0x4ed1056f, void, vita_cxa_guard_release, int *guard)
{
    if (guard)
        __atomic_store_n(guard, 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------------- */
/* Heap                                                                      */
/* ------------------------------------------------------------------------- */

static bool trace_alloc(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_TRACE_ALLOC") != NULL;

    return enabled != 0;
}

EXPORT(SceLibc, 0x775a0cb2, void *, vita_malloc, size_t size)
{
    void *block = malloc(size);

    if (trace_alloc())
        LOG("malloc(%u) -> %p", (unsigned)size, block);

    return block;
}

EXPORT(SceLibc, 0xe7ec3d0b, void *, vita_calloc, size_t count, size_t size)
{
    void *block = calloc(count, size);

    if (trace_alloc())
        LOG("calloc(%u, %u) -> %p", (unsigned)count, (unsigned)size, block);

    return block;
}

EXPORT(SceLibc, 0x006b54ba, void *, vita_realloc, void *block, size_t size)
{
    void *result = realloc(block, size);

    if (trace_alloc())
        LOG("realloc(%p, %u) -> %p", block, (unsigned)size, result);

    return result;
}

EXPORT(SceLibc, 0x5b9bb802, void, vita_free, void *block)
{
    if (trace_alloc())
        LOG("free(%p)", block);

    free(block);
}

EXPORT(SceLibc, 0xa9363e6b, void *, vita_memalign, size_t alignment, size_t size)
{
    void *block = NULL;

    if (alignment < sizeof(void *))
        alignment = sizeof(void *);

    if (posix_memalign(&block, alignment, size) != 0)
        block = NULL;

    if (trace_alloc())
        LOG("memalign(%u, %u) -> %p", (unsigned)alignment, (unsigned)size, block);

    return block;
}

/*
 * newlib's mspace allocator. The guest passes the base and size of a region it
 * reserved, and expects the handle to hold newlib's `malloc_state`. Unity both
 * imports these entry points and links its own copy of dlmalloc, so the handle
 * must be large enough (and zeroed) for either implementation to use.
 */
typedef struct {
    int magic;
    uint8_t state[0xffc];
} SceMSpace;

#define MSPACE_MAGIC 0x4d535043

EXPORT(SceLibc, 0xcef7c575, SceMSpace *, mspace_create, void *base, size_t size)
{
    SceMSpace *mspace;

    mspace = calloc(1, sizeof(*mspace));
    if (mspace)
        mspace->magic = MSPACE_MAGIC;

    if (trace_alloc())
        LOG("mspace_create(base=%p, size=0x%x) -> %p", base, (unsigned)size, (void *)mspace);

    return mspace;
}

EXPORT(SceLibc, 0x30cbbc66, void, mspace_destroy, SceMSpace *mspace)
{
    free(mspace);
}

EXPORT(SceLibc, 0xe080b96e, void *, mspace_malloc, SceMSpace *mspace, size_t size)
{
    void *block;

    (void)mspace;
    block = malloc(size);
    if (trace_alloc()) {
        static int dumped;

        if (!dumped) {
            const uint32_t *w = (const uint32_t *)mspace;

            dumped = 1;
            LOG("mspace %p state: %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                (void *)mspace, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
        LOG("mspace_malloc(%p, %u) -> %p", (void *)mspace, (unsigned)size, block);
    }
    return block;
}

EXPORT(SceLibc, 0x3cdfd2a3, void, mspace_free, SceMSpace *mspace, void *block)
{
    (void)mspace;
    if (trace_alloc())
        LOG("mspace_free(%p, %p)", (void *)mspace, block);
    free(block);
}

EXPORT(SceLibc, 0x4633134a, void *, mspace_realloc, SceMSpace *mspace, void *block, size_t size)
{
    void *result;

    (void)mspace;
    result = realloc(block, size);
    if (trace_alloc())
        LOG("mspace_realloc(%p, %p, %u) -> %p", (void *)mspace, block, (unsigned)size, result);
    return result;
}

EXPORT(SceLibc, 0x52f780dd, void *, mspace_memalign, SceMSpace *mspace, size_t alignment,
       size_t size)
{
    (void)mspace;
    return vita_memalign(alignment, size);
}

EXPORT(SceLibc, 0x915da59e, void *, mspace_reallocalign, SceMSpace *mspace, void *block,
       size_t alignment, size_t size)
{
    void *fresh;

    (void)mspace;

    if (alignment == 0)
        return realloc(block, size);

    fresh = vita_memalign(alignment, size);
    if (!fresh)
        return NULL;

    if (block) {
        size_t old = malloc_usable_size(block);

        memcpy(fresh, block, old < size ? old : size);
        free(block);
    }

    return fresh;
}

EXPORT(SceLibc, 0x7ad7a737, size_t, mspace_malloc_usable_size, SceMSpace *mspace, void *block)
{
    (void)mspace;
    return block ? malloc_usable_size(block) : 0;
}

EXPORT(SceLibc, 0xf355f381, int, mspace_is_heap_empty, SceMSpace *mspace)
{
    (void)mspace;
    return 1;
}

EXPORT(SceLibc, 0x8d2a14c4, void, mspace_malloc_stats, SceMSpace *mspace)
{
    (void)mspace;
}

EXPORT(SceLibc, 0x2ff5d5bb, size_t, mspace_malloc_stats_fast, SceMSpace *mspace)
{
    (void)mspace;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* stdio                                                                     */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibc, 0xfffbe239, FILE *, vita_fopen, const char *path, const char *mode)
{
    char buffer[512];

    return fopen(host_path(path, buffer, sizeof(buffer)), mode);
}

EXPORT(SceLibc, 0xb2f318fe, FILE *, vita_fdopen, int fd, const char *mode)
{
    return fdopen(fd, mode);
}

EXPORT(SceLibc, 0xec97321c, int, vita_fclose, FILE *stream)
{
    return fclose(stream);
}

EXPORT(SceLibc, 0x5aad2996, int, vita_fflush, FILE *stream)
{
    return fflush(stream);
}

EXPORT(SceLibc, 0xb31c73a9, size_t, vita_fread, void *buffer, size_t size, size_t count,
       FILE *stream)
{
    return fread(buffer, size, count, stream);
}

EXPORT(SceLibc, 0x8bcdcc4e, size_t, vita_fwrite, const void *buffer, size_t size, size_t count,
       FILE *stream)
{
    return fwrite(buffer, size, count, stream);
}

EXPORT(SceLibc, 0xc3a7cde1, int, vita_fseek, FILE *stream, long offset, int whence)
{
    return fseek(stream, offset, whence);
}

EXPORT(SceLibc, 0x41c2af95, long, vita_ftell, FILE *stream)
{
    return ftell(stream);
}

EXPORT(SceLibc, 0xfec1502e, int, vita_fileno, FILE *stream)
{
    return fileno(stream);
}

EXPORT(SceLibc, 0x4bd5212e, int, vita_getc, FILE *stream)
{
    return getc(stream);
}

EXPORT(SceLibc, 0x7e6a6108, int, vita_fputc, int c, FILE *stream)
{
    return fputc(c, stream);
}

EXPORT(SceLibc, 0x59c3e171, int, vita_puts, const char *s)
{
    return puts(s);
}

EXPORT(SceLibc, 0xc8ff13e5, int, vita_fputs, const char *s, FILE *stream)
{
    return fputs(s, stream);
}

EXPORT(SceLibc, 0x72ba4468, void, vita_clearerr, FILE *stream)
{
    clearerr(stream);
}

EXPORT(SceLibc, 0xb724bfc1, int, vita_ferror, FILE *stream)
{
    return ferror(stream);
}

EXPORT(SceLibc, 0x40293b75, int, vita_remove, const char *path)
{
    char buffer[512];

    return remove(host_path(path, buffer, sizeof(buffer)));
}

EXPORT(SceLibc, 0x4696e7be, void, vita_perror, const char *s)
{
    perror(s);
}

EXPORT(SceLibc, 0x9a004680, int, vita_printf, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vprintf(format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xe7b5e23e, int, vita_vprintf, const char *format, va_list args)
{
    return vprintf(format, args);
}

EXPORT(SceLibc, 0xe0c79764, int, vita_fprintf, FILE *stream, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vfprintf(stream, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xf7915685, int, vita_vfprintf, FILE *stream, const char *format, va_list args)
{
    return vfprintf(stream, format, args);
}

EXPORT(SceLibc, 0x7449b359, int, vita_sprintf, char *buffer, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsprintf(buffer, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xa1bff606, int, vita_snprintf, char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xfe83f2e4, int, vita_vsnprintf, char *buffer, size_t size, const char *format,
       va_list args)
{
    return vsnprintf(buffer, size, format, args);
}

EXPORT(SceLibc, 0x4ab952f5, int, vita_snprintf_s, char *buffer, size_t size, const char *format,
       ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xec585241, int, vita_sscanf, const char *s, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vsscanf(s, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0x5d8c1282, FILE *, vita_stdout, void)
{
    return stdout;
}

EXPORT(SceLibc, 0xddf9bb09, FILE *, vita_stderr, void)
{
    return stderr;
}

EXPORT(SceLibc, 0x66b7406c, FILE *, vita_stdin, void)
{
    return stdin;
}

/* ctype tables, published as pointers the way newlib does. */
static unsigned char g_touptab[257];
static unsigned char g_tolotab[257];
static unsigned char g_ctype[257];

EXPORT(SceLibc, 0x36878958, unsigned char *, _Touptab, void)
{
    if (!g_touptab[1]) {
        for (int i = 0; i < 257; i++)
            g_touptab[i] = (unsigned char)i;
    }

    return g_touptab;
}

EXPORT(SceLibc, 0xd662e07c, unsigned char *, _Tolotab, void)
{
    if (!g_tolotab['a']) {
        for (int i = 0; i < 257; i++)
            g_tolotab[i] = (unsigned char)(i >= 'A' && i <= 'Z' ? i + 'a' - 'A' : i);
    }

    return g_tolotab;
}

EXPORT(SceLibc, 0x3ce6109d, unsigned char *, _Ctype, void)
{
    if (!g_ctype[1]) {
        for (int i = 0; i < 257; i++) {
            unsigned char flags = 0;

            if ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z'))
                flags |= 0x01 | 0x02; /* _U | _L */
            if (i >= '0' && i <= '9')
                flags |= 0x04;
            if (i == ' ' || (i >= 0x09 && i <= 0x0d))
                flags |= 0x08;
            if (i >= 0x20 && i < 0x7f)
                flags |= 0x40; /* printable */

            g_ctype[i] = flags;
        }
    }

    return g_ctype;
}

/* ------------------------------------------------------------------------- */
/* Miscellaneous                                                             */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibc, 0x3aad41b0, void, vita_srand, unsigned int seed)
{
    srand(seed);
}

EXPORT(SceLibc, 0xc0883865, int, vita_rand, void)
{
    return rand();
}

EXPORT(SceLibc, 0xdae8d60f, time_t, vita_time, time_t *result)
{
    return time(result);
}

EXPORT(SceLibc, 0xc94ae948, char *, vita_asctime, const struct tm *tm)
{
    return asctime(tm);
}

EXPORT(SceLibc, 0xf283cfe3, struct tm *, vita_gmtime, const time_t *timer)
{
    return gmtime(timer);
}

EXPORT(SceLibc, 0xf4a2e0bf, struct tm *, vita_localtime, const time_t *timer)
{
    return localtime(timer);
}

EXPORT(SceLibc, 0xa7cbe4a6, void, vita_qsort, void *base, size_t count, size_t size,
       int (*compare)(const void *, const void *))
{
    qsort(base, count, size, compare);
}

EXPORT(SceLibc, 0x826bbbaf, void, vita_exit, int status)
{
    exit(status);
}

/* ------------------------------------------------------------------------- */
/* setjmp / longjmp                                                          */
/* ------------------------------------------------------------------------- */

/*
 * The guest jmp_buf is newlib's: eight callee-saved core registers, sp and lr.
 * Save and restore them directly so a longjmp lands back in the guest (Thumb)
 * caller without going through the import stub.
 *
 * Host functions are entered in ARM state (the stub uses `bx` to an even
 * address), so these are plain ARM functions.
 */
__attribute__((naked)) static void vita_setjmp_body(void)
{
#if defined(__arm__)
    __asm__ volatile("stmia r0!, {r4-r11, sp, lr}\n\t"
                     "mov r0, #0\n\t"
                     "bx lr\n\t");
#endif
}

__attribute__((naked)) static void vita_longjmp_body(void)
{
#if defined(__arm__)
    __asm__ volatile("ldmia r0!, {r4-r11, sp, lr}\n\t"
                     "cmp r1, #0\n\t"
                     "moveq r0, #1\n\t"
                     "movne r0, r1\n\t"
                     "bx lr\n\t");
#endif
}

EXPORT_FUNC(SCE_LIBC, 0x50b326ce, vita_setjmp_body)
EXPORT_FUNC(SCE_LIBC, 0x2d81c8c8, vita_longjmp_body)

/* ------------------------------------------------------------------------- */
/* SceLibm                                                                   */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibm, 0x22bb8237, double, vita_floor, double x) { return floor(x); }
EXPORT(SceLibm, 0xcd7c05bd, float, vita_floorf, float x) { return floorf(x); }
EXPORT(SceLibm, 0x63f05bd6, double, vita_ceil, double x) { return ceil(x); }
EXPORT(SceLibm, 0x6bbfec89, float, vita_ceilf, float x) { return ceilf(x); }
EXPORT(SceLibm, 0x943f218f, double, vita_rint, double x) { return rint(x); }
EXPORT(SceLibm, 0xaaf31896, float, vita_roundf, float x) { return roundf(x); }
EXPORT(SceLibm, 0x090b899f, float, vita_truncf, float x) { return truncf(x); }
EXPORT(SceLibm, 0x75348906, float, vita_fabsf, float x) { return fabsf(x); }
EXPORT(SceLibm, 0xda227fcc, double, vita_sqrt, double x) { return sqrt(x); }
EXPORT(SceLibm, 0xba3f6937, float, vita_sqrtf, float x) { return sqrtf(x); }
EXPORT(SceLibm, 0x640db443, double, vita_pow, double x, double y) { return pow(x, y); }
EXPORT(SceLibm, 0x6dea815a, float, vita_powf, float x, float y) { return powf(x, y); }
EXPORT(SceLibm, 0xeb027358, double, vita_exp, double x) { return exp(x); }
EXPORT(SceLibm, 0x56473bc7, float, vita_expf, float x) { return expf(x); }
EXPORT(SceLibm, 0x79415bd3, float, vita_exp2f, float x) { return exp2f(x); }
EXPORT(SceLibm, 0x0056061b, double, vita_ldexp, double x, int exponent) { return ldexp(x, exponent); }
EXPORT(SceLibm, 0x0e61e016, float, vita_ldexpf, float x, int exponent) { return ldexpf(x, exponent); }
EXPORT(SceLibm, 0x59197427, double, vita_frexp, double x, int *exponent) { return frexp(x, exponent); }
EXPORT(SceLibm, 0x1167b5d2, double, vita_modf, double x, double *integral) { return modf(x, integral); }
EXPORT(SceLibm, 0x5d7a7eb2, float, vita_modff, float x, float *integral) { return modff(x, integral); }
EXPORT(SceLibm, 0x798587e4, double, vita_fmod, double x, double y) { return fmod(x, y); }
EXPORT(SceLibm, 0x1cd8f88e, float, vita_fmodf, float x, float y) { return fmodf(x, y); }
EXPORT(SceLibm, 0x5bae40b0, double, vita_tan, double x) { return tan(x); }
EXPORT(SceLibm, 0xa98e941b, float, vita_tanf, float x) { return tanf(x); }
EXPORT(SceLibm, 0x516d9970, double, vita_atan, double x) { return atan(x); }
EXPORT(SceLibm, 0xd78fc94e, float, vita_atanf, float x) { return atanf(x); }
EXPORT(SceLibm, 0xc9be3f05, double, vita_atan2, double y, double x) { return atan2(y, x); }
EXPORT(SceLibm, 0x4e09dd53, float, vita_atan2f, float y, float x) { return atan2f(y, x); }
EXPORT(SceLibm, 0xd72b5acb, double, vita_acos, double x) { return acos(x); }
EXPORT(SceLibm, 0x27eab8c1, float, vita_acosf, float x) { return acosf(x); }
EXPORT(SceLibm, 0x4016b2e6, double, vita_asin, double x) { return asin(x); }
EXPORT(SceLibm, 0x3a3e5424, float, vita_asinf, float x) { return asinf(x); }
EXPORT(SceLibm, 0x7004fa75, float, vita_fmaxf, float x, float y) { return fmaxf(x, y); }
EXPORT(SceLibm, 0x7673cc1e, float, vita_fminf, float x, float y) { return fminf(x, y); }

/* newlib's internal entry points. */
EXPORT(SceLibm, 0x67e99979, double, vita_log, double x) { return log(x); }
EXPORT(SceLibm, 0xd5bd8d5c, float, vita_logf, float x) { return logf(x); }

EXPORT(SceLibm, 0x603caa93, double, vita_sinx, double x, int *sign)
{
    if (sign)
        *sign = 1;

    return sin(x);
}

EXPORT(SceLibm, 0x93fc85ed, float, vita_fsinx, float x, int *sign)
{
    if (sign)
        *sign = 1;

    return sinf(x);
}

EXPORT(SceLibm, 0x27a55170, int, vita_dtest, double *value)
{
    if (isnan(*value))
        return 1;
    if (isinf(*value))
        return 2;
    if (*value == 0.0)
        return 3;
    if (fpclassify(*value) == FP_SUBNORMAL)
        return 4;

    return 0;
}

EXPORT(SceLibm, 0xc5b9c8d8, int, vita_fdtest, float *value)
{
    if (isnan(*value))
        return 1;
    if (isinf(*value))
        return 2;
    if (*value == 0.0f)
        return 3;
    if (fpclassify(*value) == FP_SUBNORMAL)
        return 4;

    return 0;
}

/* ------------------------------------------------------------------------- */
/* SceLibstdcxx                                                              */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibstdcxx, 0x0466c022, const char *, vita_syserror_map, int error)
{
    return strerror(error);
}

EXPORT(SceLibstdcxx, 0x0fc144b2, void, vita_xbad_alloc, void)
{
    LOG("C++ runtime: std::bad_alloc");
    abort();
}

EXPORT(SceLibstdcxx, 0x2f6a7265, void, vita_xlength_error, const char *message)
{
    LOG("C++ runtime: std::length_error(%s)", message ? message : "");
    abort();
}

EXPORT(SceLibstdcxx, 0x553e56bd, void, vita_xout_of_range, const char *message)
{
    LOG("C++ runtime: std::out_of_range(%s)", message ? message : "");
    abort();
}

EXPORT(SceLibstdcxx, 0xc9de8208, void, vita_ios_base_clear, void *self, int state, int throw)
{
    (void)self;
    (void)state;
    (void)throw;
}

/* ------------------------------------------------------------------------- */
/* Library declarations                                                      */
/* ------------------------------------------------------------------------- */

DECLARE_LIBRARY(SceLibc, SCE_LIBC_NID);
DECLARE_LIBRARY(SceLibm, SCE_LIBM_NID);
DECLARE_LIBRARY(SceLibstdcxx, SCE_LIBSTDCXX_NID);
