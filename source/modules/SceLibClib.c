/*
 * The sceClib family: the firmware's own libc shims.
 *
 * VitaSDK's headers still declare these, and some titles (or their middleware)
 * import them instead of linking their own memcpy/strcpy. They map directly
 * onto the host C library.
 */

#include <ctype.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "module.h"

EXPORT(SceLibKernel, 0x2F2C6046, void, sceClibAbort, void)
{
    LOG("sceClibAbort called");
    abort();
}

EXPORT(SceLibKernel, 0xD8EBBB7E, char, sceClibLookCtypeTable, char ch)
{
    return (char)isprint((unsigned char)ch);
}

EXPORT(SceLibKernel, 0x4C5471BC, int, sceClibTolower, char ch)
{
    return tolower((unsigned char)ch);
}

EXPORT(SceLibKernel, 0xB0335388, int, sceClibToupper, char ch)
{
    return toupper((unsigned char)ch);
}

EXPORT(SceLibKernel, 0xFA26BC62, int, sceClibPrintf, const char *fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = vprintf(fmt, args);
    va_end(args);

    return result;
}

EXPORT(SceLibKernel, 0x5EA3B6CE, int, sceClibVprintf, const char *fmt, va_list args)
{
    return vprintf(fmt, args);
}

EXPORT(SceLibKernel, 0x4340EF77, int, sceClibDprintf, SceUID fd, const char *fmt, ...)
{
    va_list args;
    int result;

    (void)fd;

    va_start(args, fmt);
    result = vprintf(fmt, args);
    va_end(args);

    return result;
}

EXPORT(SceLibKernel, 0xABC6893C, int, sceClibVdprintf, SceUID fd, const char *fmt, va_list args)
{
    (void)fd;

    return vprintf(fmt, args);
}

EXPORT(SceLibKernel, 0x8CBA03D5, int, sceClibSnprintf, char *dst, SceSize dst_max_size,
       const char *fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = vsnprintf(dst, dst_max_size, fmt, args);
    va_end(args);

    return result;
}

EXPORT(SceLibKernel, 0xFA6BE467, int, sceClibVsnprintf, char *dst, SceSize dst_max_size,
       const char *fmt, va_list args)
{
    return vsnprintf(dst, dst_max_size, fmt, args);
}

EXPORT(SceLibKernel, 0xC2E1A945, int, sceClibSnprintfChk, char *dst, SceSize dst_max_size,
       const char *fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = vsnprintf(dst, dst_max_size, fmt, args);
    va_end(args);

    return result;
}

EXPORT(SceLibKernel, 0xB628786B, int, sceClibVsnprintfChk, char *dst, SceSize dst_max_size,
       const char *fmt, va_list args)
{
    return vsnprintf(dst, dst_max_size, fmt, args);
}

EXPORT(SceLibKernel, 0x632980D7, void *, sceClibMemset, void *dst, int ch, SceSize len)
{
    return memset(dst, ch, len);
}

EXPORT(SceLibKernel, 0xD19B6DC5, void *, sceClibMemsetChk, void *dst, int ch, SceSize len,
       SceSize dst_size)
{
    (void)dst_size;

    return memset(dst, ch, len);
}

EXPORT(SceLibKernel, 0x14E9DBD7, void *, sceClibMemcpy, void *dst, const void *src, SceSize len)
{
    return memcpy(dst, src, len);
}

EXPORT(SceLibKernel, 0x2E3B02A1, void *, sceClibMemcpy_safe, void *dst, const void *src,
       SceSize len)
{
    return memcpy(dst, src, len);
}

EXPORT(SceLibKernel, 0x1F94EF45, void *, sceClibMemcpyChk, void *dst, const void *src, SceSize len,
       SceSize dst_size)
{
    if (len > dst_size) {
        LOG("sceClibMemcpyChk: len %u > dst_size %u", len, dst_size);
        abort();
    }

    return memcpy(dst, src, len);
}

EXPORT(SceLibKernel, 0x736753C8, void *, sceClibMemmove, void *dst, const void *src, SceSize len)
{
    return memmove(dst, src, len);
}

EXPORT(SceLibKernel, 0xA18068FC, void *, sceClibMemmoveChk, void *dst, const void *src,
       SceSize len, SceSize dst_size)
{
    if (len > dst_size) {
        LOG("sceClibMemmoveChk: len %u > dst_size %u", len, dst_size);
        abort();
    }

    return memmove(dst, src, len);
}

EXPORT(SceLibKernel, 0x9CC2BFDF, int, sceClibMemcmp, const void *s1, const void *s2, SceSize len)
{
    return memcmp(s1, s2, len);
}

EXPORT(SceLibKernel, 0x100091AF, int, sceClibMemcmpConstTime, const void *s1, const void *s2,
       SceSize len)
{
    const uint8_t *a = s1;
    const uint8_t *b = s2;
    uint8_t diff = 0;

    for (SceSize i = 0; i < len; i++)
        diff |= a[i] ^ b[i];

    return diff != 0;
}

EXPORT(SceLibKernel, 0x407D6153, void *, sceClibMemchr, const void *src, int ch, SceSize len)
{
    return memchr(src, ch, len);
}

EXPORT(SceLibKernel, 0x614076B7, char *, sceClibStrchr, const char *s, int ch)
{
    return strchr(s, ch);
}

EXPORT(SceLibKernel, 0x6E728AAE, char *, sceClibStrrchr, const char *s, int ch)
{
    return strrchr(s, ch);
}

EXPORT(SceLibKernel, 0xE265498B, char *, sceClibStrstr, const char *s1, const char *s2)
{
    return strstr(s1, s2);
}

EXPORT(SceLibKernel, 0xA2FB4D9D, int, sceClibStrcmp, const char *s1, const char *s2)
{
    return strcmp(s1, s2);
}

EXPORT(SceLibKernel, 0x660D1F6D, int, sceClibStrncmp, const char *s1, const char *s2, SceSize len)
{
    return strncmp(s1, s2, len);
}

EXPORT(SceLibKernel, 0xB54C0BE4, int, sceClibStrncasecmp, const char *s1, const char *s2,
       SceSize len)
{
    return strncasecmp(s1, s2, len);
}

EXPORT(SceLibKernel, 0xAC595E68, SceSize, sceClibStrnlen, const char *s, SceSize max_len)
{
    return strnlen(s, max_len);
}

EXPORT(SceLibKernel, 0xC458D60A, char *, sceClibStrncpy, char *dst, const char *src, SceSize len)
{
    return strncpy(dst, src, len);
}

EXPORT(SceLibKernel, 0x9CBE1475, char *, sceClibStrncpyChk, char *dst, const char *src,
       SceSize len, SceSize dst_size)
{
    if (len > dst_size) {
        LOG("sceClibStrncpyChk: len %u > dst_size %u", len, dst_size);
        abort();
    }

    return strncpy(dst, src, len);
}

EXPORT(SceLibKernel, 0xA37E6383, char *, sceClibStrncat, char *dst, const char *src, SceSize len)
{
    return strncat(dst, src, len);
}

EXPORT(SceLibKernel, 0x489EBEDF, char *, sceClibStrncatChk, char *dst, const char *src,
       SceSize len, SceSize dst_size)
{
    (void)dst_size;

    return strncat(dst, src, len);
}

EXPORT(SceLibKernel, 0x0F28A495, char *, sceClibStrcatChk, char *dst, const char *src,
       SceSize dst_size)
{
    (void)dst_size;

    return strcat(dst, src);
}

EXPORT(SceLibKernel, 0xB9B6B05A, char *, sceClibStrcpyChk, char *dst, const char *src,
       SceSize dst_size)
{
    (void)dst_size;

    return strcpy(dst, src);
}

EXPORT(SceLibKernel, 0x2CDFCD1C, SceSize, sceClibStrlcpy, char *dst, const char *src, SceSize size)
{
    SceSize len = (SceSize)strlen(src);

    if (size > 0) {
        SceSize copy = len < size - 1 ? len : size - 1;

        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }

    return len;
}

EXPORT(SceLibKernel, 0xAC6CF5DB, SceSize, sceClibStrlcpyChk, char *dst, const char *src,
       SceSize size, SceSize dst_size)
{
    (void)dst_size;

    return sceClibStrlcpy(dst, src, size);
}

EXPORT(SceLibKernel, 0x70CBC2D5, SceSize, sceClibStrlcat, char *dst, const char *src, SceSize size)
{
    SceSize dst_len = (SceSize)strnlen(dst, size);
    SceSize src_len = (SceSize)strlen(src);

    if (dst_len == size)
        return size + src_len;

    if (src_len < size - dst_len) {
        memcpy(dst + dst_len, src, src_len + 1);
    } else {
        memcpy(dst + dst_len, src, size - dst_len - 1);
        dst[size - 1] = '\0';
    }

    return dst_len + src_len;
}

EXPORT(SceLibKernel, 0x4CCB60A9, SceSize, sceClibStrlcatChk, char *dst, const char *src,
       SceSize size, SceSize dst_size)
{
    (void)dst_size;

    return sceClibStrlcat(dst, src, size);
}

EXPORT(SceLibKernel, 0x2E581B88, long long, sceClibStrtoll, const char *s, char **end, int base)
{
    return strtoll(s, end, base);
}

/* mspace is a dlmalloc-style pool; the host allocator is used instead. */
typedef struct {
    int unused;
} SceClibMspaceImpl;

EXPORT(SceLibKernel, 0x3B9E301A, SceClibMspace, sceClibMspaceCreate, void *memblock, SceSize size)
{
    SceClibMspaceImpl *mspace = calloc(1, sizeof(*mspace));

    (void)memblock;
    (void)size;

    return mspace;
}

EXPORT(SceLibKernel, 0xAE1A21EC, void, sceClibMspaceDestroy, SceClibMspace mspace)
{
    free(mspace);
}

EXPORT(SceLibKernel, 0x86EF7680, void *, sceClibMspaceMalloc, SceClibMspace mspace, SceSize size)
{
    (void)mspace;

    return malloc(size);
}

EXPORT(SceLibKernel, 0x678374AD, void *, sceClibMspaceCalloc, SceClibMspace mspace, SceSize num,
       SceSize size)
{
    (void)mspace;

    return calloc(num, size);
}

EXPORT(SceLibKernel, 0x774891D6, void *, sceClibMspaceRealloc, SceClibMspace mspace, void *ptr,
       SceSize size)
{
    (void)mspace;

    return realloc(ptr, size);
}

EXPORT(SceLibKernel, 0x586AC68D, void *, sceClibMspaceReallocalign, SceClibMspace mspace,
       void *ptr, SceSize size, SceSize alignment)
{
    (void)mspace;
    (void)alignment;

    return realloc(ptr, size);
}

EXPORT(SceLibKernel, 0x3C847D57, void *, sceClibMspaceMemalign, SceClibMspace mspace,
       SceSize alignment, SceSize size)
{
    void *pointer = NULL;

    (void)mspace;

    if (posix_memalign(&pointer, alignment, size) != 0)
        return NULL;

    return pointer;
}

EXPORT(SceLibKernel, 0x9C56B4D1, void, sceClibMspaceFree, SceClibMspace mspace, void *ptr)
{
    (void)mspace;

    free(ptr);
}

EXPORT(SceLibKernel, 0x46A02279, SceSize, sceClibMspaceMallocUsableSize, void *ptr)
{
    (void)ptr;

    return 0;
}

EXPORT(SceLibKernel, 0xD1D59701, SceBool, sceClibMspaceIsHeapEmpty, SceClibMspace mspace)
{
    (void)mspace;

    return SCE_TRUE;
}

EXPORT(SceLibKernel, 0x8CC1D38E, void, sceClibMspaceMallocStats, SceClibMspace mspace,
       SceClibMspaceStats *stats)
{
    (void)mspace;

    if (stats)
        memset(stats, 0, sizeof(*stats));
}

EXPORT(SceLibKernel, 0x738E0322, void, sceClibMspaceMallocStatsFast, SceClibMspace mspace,
       SceClibMspaceStats *stats)
{
    sceClibMspaceMallocStats(mspace, stats);
}

EXPORT(SceLibKernel, 0x37691BF8, void, __stack_chk_fail, void)
{
    LOG("__stack_chk_fail called (guest stack canary mismatch)");
    abort();
}

EXPORT(SceLibKernel, 0x4458BCF3, uint32_t, SceKernelStackChkGuard, void)
{
    return 0x0DEADBEEu;
}

EXPORT(SceLibKernel, 0xEC287338, void, sceKernelStackChkFail, void)
{
    LOG("sceKernelStackChkFail called (guest stack canary mismatch)");
    abort();
}

EXPORT(SceLibKernel, 0x1EA375E6, uint32_t, sceLibSspStackChkGuard, void)
{
    return 0x0DEADBEEu;
}

EXPORT(SceLibKernel, 0x39AD080B, void, sceLibSspStackChkFail, void)
{
    LOG("sceLibSspStackChkFail called (guest stack canary mismatch)");
    abort();
}

EXPORT(SceLibKernel, 0x4373B548, int, __sce_aeabi_idiv0, void)
{
    return 0;
}

EXPORT(SceLibKernel, 0xFB235848, long long, __sce_aeabi_ldiv0, void)
{
    return 0;
}
