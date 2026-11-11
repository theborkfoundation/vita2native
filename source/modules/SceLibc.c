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
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <psp2/types.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fs.h"
#include "host_psarc.h"
#include "log.h"
#include "module.h"
#include "mspace.h"

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

/* One TLS area per guest thread. Sharing a single block across threads makes
   every thread read the same __thread variables, which breaks titles that keep
   per-thread state there (IL2CPP's "is this thread attached?" flag, for one). */
static _Thread_local uint8_t *t_tls_blocks[MAX_TLS_MODULES];

static uint8_t *tls_block(uint32_t module)
{
    if (module >= MAX_TLS_MODULES)
        return NULL;

    if (!t_tls_blocks[module]) {
        t_tls_blocks[module] = calloc(1, TLS_BLOCK_SIZE);
        if (!t_tls_blocks[module])
            return NULL;
    }

    return t_tls_blocks[module];
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

/*
 * C++ allocation operators. Native C++ titles import these from SceLibstdcxx;
 * leaving them unresolved makes every `new` return NULL. Pair them with the
 * same host allocator as malloc/free.
 */
EXPORT(SceLibstdcxx, 0xf99ed5ac, void *, _Znwj, size_t size)
{
    return malloc(size ? size : 1);
}

EXPORT(SceLibstdcxx, 0xe7fb2bf4, void *, _Znaj, size_t size)
{
    return malloc(size ? size : 1);
}

EXPORT(SceLibstdcxx, 0x72293931, void, _ZdlPv, void *block)
{
    free(block);
}

EXPORT(SceLibstdcxx, 0x91b0dc47, void, _ZdaPv, void *block)
{
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
 * newlib's mspace allocator, backed by the vendored dlmalloc implementation.
 * The handle owns exactly the region the guest reserved, and the chunk layout
 * matches what titles that link their own dlmalloc (Unity's IL2CPP runtime)
 * expect, so imported and inline calls operate on the same metadata.
 *
 * The wrapper functions are prefixed because dlmalloc defines the plain
 * `mspace_*` names itself.
 */
typedef void *SceMSpace;

EXPORT(SceLibc, 0xcef7c575, SceMSpace, sce_mspace_create, void *base, size_t size)
{
    SceMSpace mspace = vita_mspace_create(base, size);

    if (trace_alloc())
        LOG("mspace_create(base=%p, size=0x%x) -> %p", base, (unsigned)size, mspace);

    return mspace;
}

EXPORT(SceLibc, 0x30cbbc66, void, sce_mspace_destroy, SceMSpace mspace)
{
    vita_mspace_destroy(mspace);
}

EXPORT(SceLibc, 0xe080b96e, void *, sce_mspace_malloc, SceMSpace mspace, size_t size)
{
    void *block = vita_mspace_malloc(mspace, size);

    if (trace_alloc())
        LOG("mspace_malloc(%p, %u) -> %p", mspace, (unsigned)size, block);
    return block;
}

EXPORT(SceLibc, 0x3cdfd2a3, void, sce_mspace_free, SceMSpace mspace, void *block)
{
    if (trace_alloc())
        LOG("mspace_free(%p, %p)", mspace, block);
    vita_mspace_free(mspace, block);
}

EXPORT(SceLibc, 0x4633134a, void *, sce_mspace_realloc, SceMSpace mspace, void *block,
       size_t size)
{
    void *result = vita_mspace_realloc(mspace, block, size);

    if (trace_alloc())
        LOG("mspace_realloc(%p, %p, %u) -> %p", mspace, block, (unsigned)size, result);
    return result;
}

EXPORT(SceLibc, 0x52f780dd, void *, sce_mspace_memalign, SceMSpace mspace, size_t alignment,
       size_t size)
{
    void *block = vita_mspace_memalign(mspace, alignment, size);

    if (trace_alloc())
        LOG("mspace_memalign(%p, %u, %u) -> %p", mspace, (unsigned)alignment, (unsigned)size,
            block);
    return block;
}

EXPORT(SceLibc, 0x915da59e, void *, sce_mspace_reallocalign, SceMSpace mspace, void *block,
       size_t size, size_t alignment)
{
    void *fresh = vita_mspace_reallocalign(mspace, block, size, alignment);

    if (trace_alloc())
        LOG("mspace_reallocalign(%p, %p, %u, %u) -> %p", mspace, block, (unsigned)size,
            (unsigned)alignment, fresh);
    return fresh;
}

/* Sony's entry point is the one-argument dlmalloc form:
   mspace_malloc_usable_size(void *block). Unity calls it with the payload in
   r0; the second register is whatever the caller left there. */
EXPORT(SceLibc, 0x7ad7a737, size_t, sce_mspace_malloc_usable_size, void *block)
{
    size_t size = vita_mspace_usable_size(NULL, block);

    if (trace_alloc())
        LOG("mspace_usable_size(block=%p) -> %u", block, (unsigned)size);
    return size;
}

EXPORT(SceLibc, 0xf355f381, int, sce_mspace_is_heap_empty, SceMSpace mspace)
{
    return vita_mspace_is_empty(mspace);
}

EXPORT(SceLibc, 0x8d2a14c4, void, sce_mspace_malloc_stats, SceMSpace mspace)
{
    (void)mspace;
}

EXPORT(SceLibc, 0x2ff5d5bb, size_t, sce_mspace_malloc_stats_fast, SceMSpace mspace)
{
    return vita_mspace_footprint(mspace);
}

/* ------------------------------------------------------------------------- */
/* stdio                                                                     */
/* ------------------------------------------------------------------------- */

static int trace_fileio(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_TRACE_FILEIO") != NULL;
    return enabled;
}

/*
 * Guest modules sometimes keep their own newlib `FILE` structures (the libc
 * module reassigns `_Stderr` during startup, for instance). Those must never
 * reach the host stdio functions, so track every stream this module opened and
 * route anything else to the host standard streams.
 */
#define VITA_HOST_FILES 256

static FILE *g_vita_host_files[VITA_HOST_FILES];
static pthread_mutex_t g_vita_host_files_lock = PTHREAD_MUTEX_INITIALIZER;

static bool vita_is_host_file(FILE *fp)
{
    bool found = false;

    if (!fp)
        return false;
    if (fp == stdin || fp == stdout || fp == stderr)
        return true;

    pthread_mutex_lock(&g_vita_host_files_lock);
    for (int i = 0; i < VITA_HOST_FILES; i++) {
        if (g_vita_host_files[i] == fp) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_vita_host_files_lock);

    return found;
}

static void vita_track_host_file(FILE *fp)
{
    if (!fp)
        return;

    pthread_mutex_lock(&g_vita_host_files_lock);
    for (int i = 0; i < VITA_HOST_FILES; i++) {
        if (!g_vita_host_files[i]) {
            g_vita_host_files[i] = fp;
            break;
        }
    }
    pthread_mutex_unlock(&g_vita_host_files_lock);
}

static void vita_untrack_host_file(FILE *fp)
{
    pthread_mutex_lock(&g_vita_host_files_lock);
    for (int i = 0; i < VITA_HOST_FILES; i++) {
        if (g_vita_host_files[i] == fp) {
            g_vita_host_files[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_vita_host_files_lock);
}

static void vita_guest_stream_write(const void *data, size_t size)
{
    if (size)
        fwrite(data, 1, size, stderr);
}

EXPORT(SceLibc, 0xfffbe239, FILE *, vita_fopen, const char *path, const char *mode)
{
    char buffer[512];
    const char *host_name = host_path(path, buffer, sizeof(buffer));
    FILE *fp = fopen(host_name, mode);

    if (!fp && path && mode && mode[0] == 'r' && !strchr(mode, '+')) {
        char psarc_path[PATH_MAX];

        if (host_psarc_materialize(host_name, psarc_path, sizeof(psarc_path)) == 0)
            fp = fopen(psarc_path, mode);
    }

    vita_track_host_file(fp);

    if (trace_fileio())
        LOG("fopen(\"%s\", \"%s\") -> %p ra=%p", path ? path : "(null)",
            mode ? mode : "(null)", (void *)fp, __builtin_return_address(0));
    return fp;
}

EXPORT(SceLibc, 0xb2f318fe, FILE *, vita_fdopen, int fd, const char *mode)
{
    FILE *fp = fdopen(fd, mode);

    vita_track_host_file(fp);
    return fp;
}

EXPORT(SceLibc, 0xec97321c, int, vita_fclose, FILE *stream)
{
    if (trace_fileio())
        LOG("fclose(%p)", (void *)stream);
    if (!vita_is_host_file(stream))
        return 0;

    vita_untrack_host_file(stream);
    return fclose(stream);
}

EXPORT(SceLibc, 0x5aad2996, int, vita_fflush, FILE *stream)
{
    if (!stream)
        return fflush(NULL);
    if (!vita_is_host_file(stream))
        return 0;
    return fflush(stream);
}

EXPORT(SceLibc, 0xb31c73a9, size_t, vita_fread, void *buffer, size_t size, size_t count,
       FILE *stream)
{
    if (trace_fileio())
        LOG("fread(%p, %u, %u, %p)", buffer, (unsigned)size, (unsigned)count, (void *)stream);
    if (!vita_is_host_file(stream))
        return 0;
    return fread(buffer, size, count, stream);
}

EXPORT(SceLibc, 0x8bcdcc4e, size_t, vita_fwrite, const void *buffer, size_t size, size_t count,
       FILE *stream)
{
    if (trace_fileio())
        LOG("fwrite(%p, %u, %u, %p)", buffer, (unsigned)size, (unsigned)count, (void *)stream);
    if (!vita_is_host_file(stream)) {
        vita_guest_stream_write(buffer, size * count);
        return count;
    }
    return fwrite(buffer, size, count, stream);
}

EXPORT(SceLibc, 0xc3a7cde1, int, vita_fseek, FILE *stream, long offset, int whence)
{
    if (!vita_is_host_file(stream))
        return -1;
    return fseek(stream, offset, whence);
}

EXPORT(SceLibc, 0x41c2af95, long, vita_ftell, FILE *stream)
{
    if (!vita_is_host_file(stream))
        return -1;
    return ftell(stream);
}

EXPORT(SceLibc, 0xfec1502e, int, vita_fileno, FILE *stream)
{
    if (!vita_is_host_file(stream))
        return 2;
    return fileno(stream);
}

EXPORT(SceLibc, 0x4bd5212e, int, vita_getc, FILE *stream)
{
    if (!vita_is_host_file(stream))
        return -1;
    return getc(stream);
}

EXPORT(SceLibc, 0x7e6a6108, int, vita_fputc, int c, FILE *stream)
{
    if (!vita_is_host_file(stream)) {
        unsigned char ch = (unsigned char)c;

        vita_guest_stream_write(&ch, 1);
        return c;
    }
    return fputc(c, stream);
}

EXPORT(SceLibc, 0x59c3e171, int, vita_puts, const char *s)
{
    return puts(s);
}

EXPORT(SceLibc, 0xc8ff13e5, int, vita_fputs, const char *s, FILE *stream)
{
    if (trace_fileio())
        LOG("fputs(\"%s\", %p)", s ? s : "(null)", (void *)stream);
    if (!vita_is_host_file(stream)) {
        if (s)
            vita_guest_stream_write(s, strlen(s));
        return 0;
    }
    return fputs(s, stream);
}

EXPORT(SceLibc, 0x72ba4468, void, vita_clearerr, FILE *stream)
{
    if (vita_is_host_file(stream))
        clearerr(stream);
}

EXPORT(SceLibc, 0xb724bfc1, int, vita_ferror, FILE *stream)
{
    if (!vita_is_host_file(stream))
        return 0;
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

/*
 * Vita wide strings are UTF-16, but the host printf reads %ls as a 32-bit
 * wchar_t string. The two halves of a UTF-16 code unit then form an invalid
 * character, the conversion fails and nothing is written: Fort Defense builds
 * its file names with sprintf("%ls") and ended up opening empty paths.
 *
 * Format the arguments here instead: every conversion is handed to the host
 * printf on its own (so widths, precisions and floating point keep their exact
 * behaviour) and only %ls/%lc go through the UTF-16 to UTF-8 conversion.
 */
struct vita_format_out {
    char *buffer;
    size_t size;   /* capacity including the terminating NUL; SIZE_MAX = sprintf */
    size_t length; /* bytes the complete output needs */
};

static void format_append(struct vita_format_out *out, const char *text, size_t length)
{
    if (length == 0)
        return;

    if (out->size > 0 && out->length < out->size - 1) {
        size_t room = out->size - 1 - out->length;
        size_t count = length < room ? length : room;

        memcpy(out->buffer + out->length, text, count);
    }

    out->length += length;
}

static void format_emit(struct vita_format_out *out, const char *spec, ...)
{
    char stack[256];
    va_list args;
    int needed;

    va_start(args, spec);
    needed = vsnprintf(stack, sizeof(stack), spec, args);
    va_end(args);

    if (needed < 0)
        return;

    if ((size_t)needed < sizeof(stack)) {
        format_append(out, stack, (size_t)needed);
        return;
    }

    {
        char *heap = malloc((size_t)needed + 1);

        if (!heap) {
            format_append(out, stack, sizeof(stack) - 1);
            return;
        }

        va_start(args, spec);
        vsnprintf(heap, (size_t)needed + 1, spec, args);
        va_end(args);
        format_append(out, heap, (size_t)needed);
        free(heap);
    }
}

/* Returns the UTF-8 length and, when `dst` is given, writes the text (without
   the NUL) as long as it fits. */
static size_t utf16_to_utf8(char *dst, size_t dst_size, const uint16_t *src)
{
    size_t length = 0;

    for (; *src; src++) {
        uint32_t code = *src;
        char encoded[4];
        size_t count;

        if (code >= 0xd800 && code <= 0xdbff && src[1] >= 0xdc00 && src[1] <= 0xdfff) {
            code = 0x10000u + ((code - 0xd800u) << 10) + (src[1] - 0xdc00u);
            src++;
        }

        if (code < 0x80) {
            encoded[0] = (char)code;
            count = 1;
        } else if (code < 0x800) {
            encoded[0] = (char)(0xc0 | (code >> 6));
            encoded[1] = (char)(0x80 | (code & 0x3f));
            count = 2;
        } else if (code < 0x10000) {
            encoded[0] = (char)(0xe0 | (code >> 12));
            encoded[1] = (char)(0x80 | ((code >> 6) & 0x3f));
            encoded[2] = (char)(0x80 | (code & 0x3f));
            count = 3;
        } else {
            encoded[0] = (char)(0xf0 | (code >> 18));
            encoded[1] = (char)(0x80 | ((code >> 12) & 0x3f));
            encoded[2] = (char)(0x80 | ((code >> 6) & 0x3f));
            encoded[3] = (char)(0x80 | (code & 0x3f));
            count = 4;
        }

        if (dst && length + count <= dst_size)
            memcpy(dst + length, encoded, count);
        length += count;
    }

    return length;
}

static void format_wide_string(struct vita_format_out *out, const char *prefix,
                               const char *precision_text, const uint16_t *text)
{
    char spec[64];

    if (!text) {
        snprintf(spec, sizeof(spec), "%s%ss", prefix, precision_text);
        format_emit(out, spec, "(null)");
        return;
    }

    {
        size_t length = utf16_to_utf8(NULL, 0, text);
        char stack[256];
        char *buffer = length < sizeof(stack) ? stack : malloc(length + 1);

        if (!buffer)
            return;

        utf16_to_utf8(buffer, length + 1, text);
        buffer[length] = '\0';

        /* %ls counts the precision in bytes and never splits a character. */
        if (precision_text[0]) {
            long precision = strtol(precision_text + 1, NULL, 10);

            if (precision >= 0 && (size_t)precision < length) {
                length = (size_t)precision;
                while (length > 0 && ((unsigned char)buffer[length] & 0xc0) == 0x80)
                    length--;
                buffer[length] = '\0';
            }
        }

        snprintf(spec, sizeof(spec), "%ss", prefix);
        format_emit(out, spec, buffer);

        if (buffer != stack)
            free(buffer);
    }
}

typedef enum {
    VITA_LEN_NONE,
    VITA_LEN_HH,
    VITA_LEN_H,
    VITA_LEN_L,
    VITA_LEN_LL,
    VITA_LEN_J,
    VITA_LEN_Z,
    VITA_LEN_T,
    VITA_LEN_BIG_L,
} vita_format_length;

static int vita_format(char *buffer, size_t size, const char *format, va_list args)
{
    struct vita_format_out out = { buffer, size, 0 };
    const char *p = format;
    va_list ap;

    if (!format) {
        format = "(null)";
        p = format;
    }

    va_copy(ap, args);

    while (*p) {
        char prefix[48];
        size_t prefix_length = 0;
        char precision_text[16] = "";
        vita_format_length length = VITA_LEN_NONE;
        char spec[64];
        char conversion;

        if (*p != '%') {
            const char *start = p;

            while (*p && *p != '%')
                p++;
            format_append(&out, start, (size_t)(p - start));
            continue;
        }

        p++;
        if (*p == '%') {
            p++;
            format_append(&out, "%", 1);
            continue;
        }

        prefix[prefix_length++] = '%';
        while (*p && strchr("-+ #0'", *p) && prefix_length < sizeof(prefix) - 1)
            prefix[prefix_length++] = *p++;

        if (*p == '*') {
            int width = va_arg(ap, int);

            p++;
            if (width < 0) {
                if (prefix_length < sizeof(prefix) - 1)
                    prefix[prefix_length++] = '-';
                width = -width;
            }
            prefix_length += (size_t)snprintf(prefix + prefix_length,
                                              sizeof(prefix) - prefix_length, "%d", width);
            if (prefix_length >= sizeof(prefix) - 1)
                prefix_length = sizeof(prefix) - 1;
        } else {
            while (*p >= '0' && *p <= '9' && prefix_length < sizeof(prefix) - 1)
                prefix[prefix_length++] = *p++;
        }
        prefix[prefix_length] = '\0';

        if (*p == '.') {
            p++;
            if (*p == '*') {
                int precision = va_arg(ap, int);

                p++;
                if (precision >= 0)
                    snprintf(precision_text, sizeof(precision_text), ".%d", precision);
            } else {
                char *text = precision_text;

                *text++ = '.';
                while (*p >= '0' && *p <= '9' &&
                       text < precision_text + sizeof(precision_text) - 1)
                    *text++ = *p++;
                *text = '\0';
            }
        }

        if (p[0] == 'h' && p[1] == 'h') {
            length = VITA_LEN_HH;
            p += 2;
        } else if (*p == 'h') {
            length = VITA_LEN_H;
            p++;
        } else if (p[0] == 'l' && p[1] == 'l') {
            length = VITA_LEN_LL;
            p += 2;
        } else if (*p == 'l') {
            length = VITA_LEN_L;
            p++;
        } else if (*p == 'j') {
            length = VITA_LEN_J;
            p++;
        } else if (*p == 'z') {
            length = VITA_LEN_Z;
            p++;
        } else if (*p == 't') {
            length = VITA_LEN_T;
            p++;
        } else if (*p == 'L' || *p == 'q') {
            length = *p == 'L' ? VITA_LEN_BIG_L : VITA_LEN_LL;
            p++;
        }

        if (!*p)
            break;

        conversion = *p++;

        switch (conversion) {
        case 'd':
        case 'i':
            switch (length) {
            case VITA_LEN_HH:
                snprintf(spec, sizeof(spec), "%s%s%sd", prefix, precision_text, "hh");
                format_emit(&out, spec, (int)(signed char)va_arg(ap, int));
                break;
            case VITA_LEN_H:
                snprintf(spec, sizeof(spec), "%s%shd", prefix, precision_text);
                format_emit(&out, spec, (int)(short)va_arg(ap, int));
                break;
            case VITA_LEN_L:
                snprintf(spec, sizeof(spec), "%s%sld", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, long));
                break;
            case VITA_LEN_LL:
                snprintf(spec, sizeof(spec), "%s%slld", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, long long));
                break;
            case VITA_LEN_J:
                snprintf(spec, sizeof(spec), "%s%sjd", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, intmax_t));
                break;
            case VITA_LEN_Z:
                snprintf(spec, sizeof(spec), "%s%szd", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, ssize_t));
                break;
            case VITA_LEN_T:
                snprintf(spec, sizeof(spec), "%s%std", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, ptrdiff_t));
                break;
            default:
                snprintf(spec, sizeof(spec), "%s%sd", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, int));
                break;
            }
            break;

        case 'o':
        case 'u':
        case 'x':
        case 'X':
            switch (length) {
            case VITA_LEN_HH:
                snprintf(spec, sizeof(spec), "%s%shh%c", prefix, precision_text, conversion);
                format_emit(&out, spec, (unsigned int)(unsigned char)va_arg(ap, unsigned int));
                break;
            case VITA_LEN_H:
                snprintf(spec, sizeof(spec), "%s%sh%c", prefix, precision_text, conversion);
                format_emit(&out, spec, (unsigned int)(unsigned short)va_arg(ap, unsigned int));
                break;
            case VITA_LEN_L:
                snprintf(spec, sizeof(spec), "%s%sl%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, unsigned long));
                break;
            case VITA_LEN_LL:
                snprintf(spec, sizeof(spec), "%s%sll%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, unsigned long long));
                break;
            case VITA_LEN_J:
                snprintf(spec, sizeof(spec), "%s%sj%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, uintmax_t));
                break;
            case VITA_LEN_Z:
                snprintf(spec, sizeof(spec), "%s%sz%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, size_t));
                break;
            case VITA_LEN_T:
                snprintf(spec, sizeof(spec), "%s%st%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, ptrdiff_t));
                break;
            default:
                snprintf(spec, sizeof(spec), "%s%s%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, unsigned int));
                break;
            }
            break;

        case 'c':
            if (length == VITA_LEN_L) {
                uint16_t units[2];
                char narrow[8];
                size_t narrow_length;

                units[0] = (uint16_t)va_arg(ap, int);
                units[1] = 0;
                narrow_length = utf16_to_utf8(narrow, sizeof(narrow) - 1, units);
                narrow[narrow_length] = '\0';
                snprintf(spec, sizeof(spec), "%ss", prefix);
                format_emit(&out, spec, narrow);
            } else {
                snprintf(spec, sizeof(spec), "%s%sc", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, int));
            }
            break;

        case 's':
            if (length == VITA_LEN_L)
                format_wide_string(&out, prefix, precision_text, va_arg(ap, const uint16_t *));
            else {
                snprintf(spec, sizeof(spec), "%s%ss", prefix, precision_text);
                format_emit(&out, spec, va_arg(ap, const char *));
            }
            break;

        case 'p':
            snprintf(spec, sizeof(spec), "%s%sp", prefix, precision_text);
            format_emit(&out, spec, va_arg(ap, void *));
            break;

        case 'n':
            switch (length) {
            case VITA_LEN_HH:
                *va_arg(ap, signed char *) = (signed char)out.length;
                break;
            case VITA_LEN_H:
                *va_arg(ap, short *) = (short)out.length;
                break;
            case VITA_LEN_L:
                *va_arg(ap, long *) = (long)out.length;
                break;
            case VITA_LEN_LL:
                *va_arg(ap, long long *) = (long long)out.length;
                break;
            case VITA_LEN_J:
                *va_arg(ap, intmax_t *) = (intmax_t)out.length;
                break;
            case VITA_LEN_Z:
                *va_arg(ap, ssize_t *) = (ssize_t)out.length;
                break;
            default:
                *va_arg(ap, int *) = (int)out.length;
                break;
            }
            break;

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            if (length == VITA_LEN_BIG_L) {
                snprintf(spec, sizeof(spec), "%s%sL%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, long double));
            } else {
                snprintf(spec, sizeof(spec), "%s%s%c", prefix, precision_text, conversion);
                format_emit(&out, spec, va_arg(ap, double));
            }
            break;

        default:
            /* Unknown conversion: emit it verbatim rather than dropping text. */
            snprintf(spec, sizeof(spec), "%s%s%c", prefix, precision_text, conversion);
            format_append(&out, spec, strlen(spec));
            break;
        }
    }

    va_end(ap);

    if (out.size > 0) {
        size_t end = out.length < out.size - 1 ? out.length : out.size - 1;

        out.buffer[end] = '\0';
    }

    return (int)out.length;
}

static int vita_stream_write(FILE *stream, const char *text, size_t length)
{
    if (vita_is_host_file(stream)) {
        if (length == 0)
            return 0;
        return (int)fwrite(text, 1, length, stream);
    }

    vita_guest_stream_write(text, length);
    return (int)length;
}

static int vita_format_stream(FILE *stream, const char *format, va_list args)
{
    char stack[512];
    char *buffer = stack;
    va_list copy;
    int needed;
    int written;

    va_copy(copy, args);
    needed = vita_format(stack, sizeof(stack), format, copy);
    va_end(copy);

    if (needed < 0)
        return needed;

    if ((size_t)needed >= sizeof(stack)) {
        buffer = malloc((size_t)needed + 1);
        if (!buffer)
            return -1;

        va_copy(copy, args);
        vita_format(buffer, (size_t)needed + 1, format, copy);
        va_end(copy);
    }

    written = vita_stream_write(stream, buffer, (size_t)needed);

    if (buffer != stack)
        free(buffer);

    return written;
}

EXPORT(SceLibc, 0x9a004680, int, vita_printf, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vita_format_stream(stdout, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xe7b5e23e, int, vita_vprintf, const char *format, va_list args)
{
    return vita_format_stream(stdout, format, args);
}

EXPORT(SceLibc, 0xe0c79764, int, vita_fprintf, FILE *stream, const char *format, ...)
{
    va_list args;
    int result;

    if (trace_fileio())
        LOG("fprintf(%p, \"%s\")", (void *)stream, format ? format : "(null)");

    va_start(args, format);
    result = vita_format_stream(stream, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xf7915685, int, vita_vfprintf, FILE *stream, const char *format, va_list args)
{
    return vita_format_stream(stream, format, args);
}

EXPORT(SceLibc, 0x7449b359, int, vita_sprintf, char *buffer, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vita_format(buffer, SIZE_MAX, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xa1bff606, int, vita_snprintf, char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vita_format(buffer, size, format, args);
    va_end(args);

    return result;
}

EXPORT(SceLibc, 0xfe83f2e4, int, vita_vsnprintf, char *buffer, size_t size, const char *format,
       va_list args)
{
    return vita_format(buffer, size, format, args);
}

EXPORT(SceLibc, 0x4ab952f5, int, vita_snprintf_s, char *buffer, size_t size, const char *format,
       ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = vita_format(buffer, size, format, args);
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
    if (trace_fileio())
        LOG("stdout() -> %p", (void *)stdout);
    return stdout;
}

EXPORT(SceLibc, 0xddf9bb09, FILE *, vita_stderr, void)
{
    if (trace_fileio())
        LOG("stderr() -> %p", (void *)stderr);
    return stderr;
}

EXPORT(SceLibc, 0x66b7406c, FILE *, vita_stdin, void)
{
    if (trace_fileio())
        LOG("stdin() -> %p", (void *)stdin);
    return stdin;
}

/*
 * Sony's libc publishes _Ctype, _Touptab and _Tolotab as *pointer variables*
 * that point at 16-bit tables indexed by the raw (signed) char. The guest code
 * loads the pointer and then indexes it with `ldrsh [ptr + c * 2]`, so both the
 * cell and the table must be ready before any guest code runs. The entries are
 * the ones libc.suprx ships: isdigit tests 0x20, isalpha 0x212, isspace 0x144,
 * ispunct 0x8, isprint 0x23e, isgraph 0x23a, iscntrl 0x80 and isxdigit 0x404.
 * The tables are padded below index 0 so that negative chars stay in bounds.
 */
#define LIBC_CTYPE_PAD 128

static const uint16_t k_ctype[256] = {
    0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080,
    0x080, 0x4c0, 0x0c0, 0x0c0, 0x0c0, 0x0c0, 0x080, 0x080,
    0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080,
    0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080, 0x080,
    0x004, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008,
    0x008, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008,
    0x021, 0x021, 0x021, 0x021, 0x021, 0x021, 0x021, 0x021,
    0x021, 0x021, 0x008, 0x008, 0x008, 0x008, 0x008, 0x008,
    0x008, 0x003, 0x003, 0x003, 0x003, 0x003, 0x003, 0x002,
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002,
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002,
    0x002, 0x002, 0x002, 0x008, 0x008, 0x008, 0x008, 0x008,
    0x008, 0x011, 0x011, 0x011, 0x011, 0x011, 0x011, 0x010,
    0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010,
    0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010,
    0x010, 0x010, 0x010, 0x008, 0x008, 0x008, 0x008, 0x080,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
};

static uint16_t g_ctype[LIBC_CTYPE_PAD + 256];
static uint16_t g_touptab[LIBC_CTYPE_PAD + 256];
static uint16_t g_tolotab[LIBC_CTYPE_PAD + 256];

/*
 * The stdio and ctype entry points are exported both as variables and as
 * accessor functions depending on the toolchain. Guest modules import the
 * variables, so publish cells holding the host objects. They must be ready
 * before any guest code runs.
 */
extern void *g_vita_libc_stdin_cell;
extern void *g_vita_libc_stdout_cell;
extern void *g_vita_libc_stderr_cell;
extern void *g_vita_libc_touptab_cell;
extern void *g_vita_libc_tolotab_cell;
extern void *g_vita_libc_ctype_cell;

static void init_libc_var_cells(void) __attribute__((constructor));
static void init_libc_var_cells(void)
{
    for (int i = 0; i < 256; i++) {
        /* Negative chars index the pad; keep them in range and map them to
           themselves, which is what the firmware tables do for 0x80..0xff. */
        g_ctype[LIBC_CTYPE_PAD + i] = k_ctype[i];
        g_touptab[LIBC_CTYPE_PAD + i] =
            (uint16_t)(i >= 'a' && i <= 'z' ? i - 'a' + 'A' : i);
        g_tolotab[LIBC_CTYPE_PAD + i] =
            (uint16_t)(i >= 'A' && i <= 'Z' ? i - 'A' + 'a' : i);
    }
    for (int i = 0; i < LIBC_CTYPE_PAD; i++) {
        g_ctype[i] = 0;
        g_touptab[i] = (uint16_t)(i - LIBC_CTYPE_PAD);
        g_tolotab[i] = (uint16_t)(i - LIBC_CTYPE_PAD);
    }

    g_vita_libc_stdin_cell = stdin;
    g_vita_libc_stdout_cell = stdout;
    g_vita_libc_stderr_cell = stderr;
    g_vita_libc_touptab_cell = &g_touptab[LIBC_CTYPE_PAD];
    g_vita_libc_tolotab_cell = &g_tolotab[LIBC_CTYPE_PAD];
    g_vita_libc_ctype_cell = &g_ctype[LIBC_CTYPE_PAD];
}

EXPORT(SceLibc, 0x36878958, unsigned short *, _Touptab, void)
{
    return &g_touptab[LIBC_CTYPE_PAD];
}

EXPORT(SceLibc, 0xd662e07c, unsigned short *, _Tolotab, void)
{
    return &g_tolotab[LIBC_CTYPE_PAD];
}

EXPORT(SceLibc, 0x3ce6109d, unsigned short *, _Ctype, void)
{
    return &g_ctype[LIBC_CTYPE_PAD];
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

EXPORT(SceLibc, 0xc082ca03, clock_t, vita_clock, void)
{
    return clock();
}

EXPORT(SceLibc, 0x20fe0fff, void, vita_abort, void)
{
    LOG("abort() called");
    fflush(NULL);

    /* Let the crash handler print the guest stack and produce a core. */
    raise(SIGABRT);
    _exit(128 + SIGABRT);
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

EXPORT_FUNC(SceLibc, 0x50b326ce, vita_setjmp_body)
EXPORT_FUNC(SceLibc, 0x2d81c8c8, vita_longjmp_body)

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

/* Sony's newlib folds sin and cos into these: bit 0 selects cosine and bit 1
   negates the result, with the angle in s0/d0. The wrappers call
   _fsinx(x, 0) / _fsinx(x, 1) from sincos. */
EXPORT(SceLibm, 0x603caa93, double, vita_sinx, double x, int flags)
{
    double result = (flags & 1) ? cos(x) : sin(x);

    return (flags & 2) ? -result : result;
}

EXPORT(SceLibm, 0x93fc85ed, float, vita_fsinx, float x, int flags)
{
    float result = (flags & 1) ? cosf(x) : sinf(x);

    return (flags & 2) ? -result : result;
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
