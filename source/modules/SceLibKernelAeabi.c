/*
 * ARM EABI runtime helpers exported by SceLibKernel.
 *
 * Most helpers map onto a normal AAPCS signature. The divide/modulo helpers
 * are special: they return the quotient in r0:r1 and the remainder in r2:r3,
 * which cannot be expressed as a C return type, so they are small assembly
 * wrappers around C helpers.
 */

#include <limits.h>
#include <stdint.h>

#include "module.h"

int32_t vita_aeabi_idiv(int32_t a, int32_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0x38D62D60, vita_aeabi_idiv);

uint32_t vita_aeabi_uidiv(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0xFB311F87, vita_aeabi_uidiv);

int64_t vita_aeabi_lmul(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a * (uint64_t)b);
}
EXPORT_FUNC(SceLibKernel, 0x141BC4CE, vita_aeabi_lmul);

int64_t vita_aeabi_llsl(int64_t value, int shift)
{
    if (shift < 0 || shift >= 64)
        return 0;
    return (int64_t)((uint64_t)value << shift);
}
EXPORT_FUNC(SceLibKernel, 0xA5DB3A86, vita_aeabi_llsl);

uint64_t vita_aeabi_llsr(uint64_t value, int shift)
{
    if (shift < 0 || shift >= 64)
        return 0;
    return value >> shift;
}
EXPORT_FUNC(SceLibKernel, 0xCBDA815C, vita_aeabi_llsr);

int64_t vita_aeabi_lasr(int64_t value, int shift)
{
    if (shift < 0 || shift >= 64)
        return value < 0 ? -1 : 0;
    return value >> shift;
}
EXPORT_FUNC(SceLibKernel, 0x21FF67B9, vita_aeabi_lasr);

int vita_aeabi_lcmp(int64_t a, int64_t b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}
EXPORT_FUNC(SceLibKernel, 0x0D4F0635, vita_aeabi_lcmp);

int vita_aeabi_ulcmp(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}
EXPORT_FUNC(SceLibKernel, 0x317B3774, vita_aeabi_ulcmp);

static __attribute__((used, noinline)) void aeabi_idivmod_helper(int32_t a, int32_t b,
                                                                 int32_t *quotient,
                                                                 int32_t *remainder)
{
    if (!b) {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    if (a == INT32_MIN && b == -1) {
        *quotient = INT32_MIN;
        *remainder = 0;
        return;
    }
    *quotient = a / b;
    *remainder = a % b;
}

static __attribute__((used, noinline)) void aeabi_uidivmod_helper(uint32_t a, uint32_t b,
                                                                  uint32_t *quotient,
                                                                  uint32_t *remainder)
{
    if (!b) {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    *quotient = a / b;
    *remainder = a % b;
}

static __attribute__((used, noinline)) void aeabi_ldivmod_helper(int64_t a, int64_t b,
                                                                 int64_t *quotient,
                                                                 int64_t *remainder)
{
    if (!b) {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    if (a == INT64_MIN && b == -1) {
        *quotient = INT64_MIN;
        *remainder = 0;
        return;
    }
    *quotient = a / b;
    *remainder = a % b;
}

static __attribute__((used, noinline)) void aeabi_uldivmod_helper(uint64_t a, uint64_t b,
                                                                  uint64_t *quotient,
                                                                  uint64_t *remainder)
{
    if (!b) {
        *quotient = 0;
        *remainder = 0;
        return;
    }
    *quotient = a / b;
    *remainder = a % b;
}

#if defined(__arm__)

__attribute__((naked)) void aeabi_idivmod_naked(void)
{
    __asm__ volatile("push {r4, lr}\n"
                     "sub  sp, sp, #8\n"
                     "add  r2, sp, #0\n"
                     "add  r3, sp, #4\n"
                     "bl   aeabi_idivmod_helper\n"
                     "ldr  r0, [sp, #0]\n"
                     "ldr  r1, [sp, #4]\n"
                     "add  sp, sp, #8\n"
                     "pop  {r4, pc}\n");
}
EXPORT_FUNC(SceLibKernel, 0x67104054, aeabi_idivmod_naked);

__attribute__((naked)) void aeabi_uidivmod_naked(void)
{
    __asm__ volatile("push {r4, lr}\n"
                     "sub  sp, sp, #8\n"
                     "add  r2, sp, #0\n"
                     "add  r3, sp, #4\n"
                     "bl   aeabi_uidivmod_helper\n"
                     "ldr  r0, [sp, #0]\n"
                     "ldr  r1, [sp, #4]\n"
                     "add  sp, sp, #8\n"
                     "pop  {r4, pc}\n");
}
EXPORT_FUNC(SceLibKernel, 0xC33391D1, aeabi_uidivmod_naked);

__attribute__((naked)) void aeabi_ldivmod_naked(void)
{
    __asm__ volatile("push {r4, lr}\n"
                     "sub  sp, sp, #24\n"
                     "add  r4, sp, #8\n"
                     "str  r4, [sp, #0]\n"
                     "add  r4, sp, #16\n"
                     "str  r4, [sp, #4]\n"
                     "bl   aeabi_ldivmod_helper\n"
                     "ldr  r0, [sp, #8]\n"
                     "ldr  r1, [sp, #12]\n"
                     "ldr  r2, [sp, #16]\n"
                     "ldr  r3, [sp, #20]\n"
                     "add  sp, sp, #24\n"
                     "pop  {r4, pc}\n");
}
EXPORT_FUNC(SceLibKernel, 0x6CBB0E84, aeabi_ldivmod_naked);

__attribute__((naked)) void aeabi_uldivmod_naked(void)
{
    __asm__ volatile("push {r4, lr}\n"
                     "sub  sp, sp, #24\n"
                     "add  r4, sp, #8\n"
                     "str  r4, [sp, #0]\n"
                     "add  r4, sp, #16\n"
                     "str  r4, [sp, #4]\n"
                     "bl   aeabi_uldivmod_helper\n"
                     "ldr  r0, [sp, #8]\n"
                     "ldr  r1, [sp, #12]\n"
                     "ldr  r2, [sp, #16]\n"
                     "ldr  r3, [sp, #20]\n"
                     "add  sp, sp, #24\n"
                     "pop  {r4, pc}\n");
}
EXPORT_FUNC(SceLibKernel, 0xCDF7708E, aeabi_uldivmod_naked);

#else

/* Host test builds are not ARM; provide ordinary C entry points. */
int32_t aeabi_idivmod_naked(int32_t a, int32_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0x67104054, aeabi_idivmod_naked);

uint32_t aeabi_uidivmod_naked(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0xC33391D1, aeabi_uidivmod_naked);

int64_t aeabi_ldivmod_naked(int64_t a, int64_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0x6CBB0E84, aeabi_ldivmod_naked);

uint64_t aeabi_uldivmod_naked(uint64_t a, uint64_t b)
{
    return b ? a / b : 0;
}
EXPORT_FUNC(SceLibKernel, 0xCDF7708E, aeabi_uldivmod_naked);

#endif

static int64_t aeabi_f2lz_softfp(float value) GUEST_SOFTFP_ABI;
static int64_t aeabi_f2lz_softfp(float value)
{
    return (int64_t)value;
}
EXPORT_FUNC(SceLibKernel, 0xAA1F1B50, aeabi_f2lz_softfp);

static uint64_t aeabi_f2ulz_softfp(float value) GUEST_SOFTFP_ABI;
static uint64_t aeabi_f2ulz_softfp(float value)
{
    return (uint64_t)value;
}
EXPORT_FUNC(SceLibKernel, 0x609CA961, aeabi_f2ulz_softfp);

static int64_t aeabi_d2lz_softfp(double value) GUEST_SOFTFP_ABI;
static int64_t aeabi_d2lz_softfp(double value)
{
    return (int64_t)value;
}
EXPORT_FUNC(SceLibKernel, 0x5024AB91, aeabi_d2lz_softfp);

static uint64_t aeabi_d2ulz_softfp(double value) GUEST_SOFTFP_ABI;
static uint64_t aeabi_d2ulz_softfp(double value)
{
    return (uint64_t)value;
}
EXPORT_FUNC(SceLibKernel, 0x6BB838EF, aeabi_d2ulz_softfp);

DECLARE_LIBRARY(SceLibKernel, 0xCAE9ACE6);
