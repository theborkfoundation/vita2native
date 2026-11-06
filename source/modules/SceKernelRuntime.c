/*
 * SceKernelRuntime: compiler-adjacent kernel helpers.
 *
 * These exports are the pieces libc/libstdc++ and timing code reach for:
 * the complete 8/16/32/64-bit atomic family, process/system clocks, the
 * random-number helper and the OpenPSID query. Atomics use compiler
 * intrinsics, which lower to ldrex/strex sequences on ARMv7.
 */

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "host.h"
#include "module.h"

#define ATOMIC_SET(nid, name, T)                                                                   \
    EXPORT(SceLibKernel, nid, void, name, T *store, T value)                                       \
    {                                                                                              \
        __atomic_store_n(store, value, __ATOMIC_SEQ_CST);                                          \
    }

#define ATOMIC_CAS(nid, name, T)                                                                   \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value, T new_value)                             \
    {                                                                                              \
        T expected = value;                                                                        \
        __atomic_compare_exchange_n(store, &expected, new_value, 0, __ATOMIC_SEQ_CST,              \
                                    __ATOMIC_SEQ_CST);                                             \
        return expected;                                                                           \
    }

/*
 * Vita semantics: add value, but clamp the result to limit. Returns whether
 * the limit was reached. 8/16-bit values are summed at 32 bits.
 */
#define ATOMIC_ADD_UNLESS_NARROW(nid, name, T)                                                     \
    EXPORT(SceLibKernel, nid, SceBool, name, T *store, T value, T limit)                           \
    {                                                                                              \
        T old = __atomic_load_n(store, __ATOMIC_SEQ_CST);                                          \
        for (;;) {                                                                                 \
            int32_t sum = (int32_t)((uint32_t)(int32_t)old + (uint32_t)(int32_t)value);            \
            T next = sum >= (int32_t)limit ? limit : (T)sum;                                       \
            if (__atomic_compare_exchange_n(store, &old, next, 0, __ATOMIC_SEQ_CST,                \
                                            __ATOMIC_SEQ_CST))                                     \
                return sum >= (int32_t)limit;                                                      \
        }                                                                                          \
    }

#define ATOMIC_ADD_UNLESS_WIDE(nid, name, T)                                                       \
    EXPORT(SceLibKernel, nid, SceBool, name, T *store, T value, T limit)                           \
    {                                                                                              \
        T old = __atomic_load_n(store, __ATOMIC_SEQ_CST);                                          \
        for (;;) {                                                                                 \
            int64_t sum = (int64_t)((uint64_t)(int64_t)old + (uint64_t)(int64_t)value);            \
            T next = sum >= (int64_t)limit ? limit : (T)sum;                                       \
            if (__atomic_compare_exchange_n(store, &old, next, 0, __ATOMIC_SEQ_CST,                \
                                            __ATOMIC_SEQ_CST))                                     \
                return sum >= (int64_t)limit;                                                      \
        }                                                                                          \
    }

#define ATOMIC_ADD_FETCH(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_add_fetch(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_SUB_FETCH(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_sub_fetch(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_AND_FETCH(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_and_fetch(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_OR_FETCH(nid, name, T)                                                              \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_or_fetch(store, value, __ATOMIC_SEQ_CST);                                  \
    }

#define ATOMIC_XOR_FETCH(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_xor_fetch(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_CLEAR_FETCH(nid, name, T)                                                           \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_and_fetch(store, (T)~value, __ATOMIC_SEQ_CST);                             \
    }

#define ATOMIC_EXCHANGE(nid, name, T)                                                              \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_exchange_n(store, value, __ATOMIC_SEQ_CST);                                \
    }

#define ATOMIC_FETCH_ADD(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_add(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_FETCH_SUB(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_sub(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_FETCH_AND(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_and(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_FETCH_OR(nid, name, T)                                                              \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_or(store, value, __ATOMIC_SEQ_CST);                                  \
    }

#define ATOMIC_FETCH_XOR(nid, name, T)                                                             \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_xor(store, value, __ATOMIC_SEQ_CST);                                 \
    }

#define ATOMIC_FETCH_CLEAR(nid, name, T)                                                           \
    EXPORT(SceLibKernel, nid, T, name, T *store, T value)                                          \
    {                                                                                              \
        return __atomic_fetch_and(store, (T)~value, __ATOMIC_SEQ_CST);                             \
    }

#define ATOMIC_CLEAR_MASK(nid, name, T)                                                            \
    EXPORT(SceLibKernel, nid, void, name, T *store, T value)                                       \
    {                                                                                              \
        __atomic_and_fetch(store, (T)~value, __ATOMIC_SEQ_CST);                                    \
    }

/* Returns the old value. 8/16-bit variants also decrement zero (Vita quirk). */
#define ATOMIC_DEC_IF_POSITIVE_NARROW(nid, name, T)                                                \
    EXPORT(SceLibKernel, nid, T, name, T *store)                                                   \
    {                                                                                              \
        T old = __atomic_load_n(store, __ATOMIC_SEQ_CST);                                          \
        while (old >= 0) {                                                                         \
            if (__atomic_compare_exchange_n(store, &old, (T)(old - 1), 0, __ATOMIC_SEQ_CST,        \
                                            __ATOMIC_SEQ_CST))                                     \
                break;                                                                             \
        }                                                                                          \
        return old;                                                                                \
    }

#define ATOMIC_DEC_IF_POSITIVE_WIDE(nid, name, T)                                                  \
    EXPORT(SceLibKernel, nid, T, name, T *store)                                                   \
    {                                                                                              \
        T old = __atomic_load_n(store, __ATOMIC_SEQ_CST);                                          \
        while (old > 0) {                                                                          \
            if (__atomic_compare_exchange_n(store, &old, (T)(old - 1), 0, __ATOMIC_SEQ_CST,        \
                                            __ATOMIC_SEQ_CST))                                     \
                break;                                                                             \
        }                                                                                          \
        return old;                                                                                \
    }

ATOMIC_ADD_FETCH(0x495C52EC, sceKernelAtomicAddAndGet16, int16_t)
ATOMIC_ADD_FETCH(0x2E84A93B, sceKernelAtomicAddAndGet32, int32_t)
ATOMIC_ADD_FETCH(0xB6CE9B9A, sceKernelAtomicAddAndGet64, int64_t)
ATOMIC_ADD_FETCH(0x8F7BD940, sceKernelAtomicAddAndGet8, int8_t)
ATOMIC_ADD_UNLESS_NARROW(0x1CF4AA4B, sceKernelAtomicAddUnless16, int16_t)
ATOMIC_ADD_UNLESS_NARROW(0x4B33FD3C, sceKernelAtomicAddUnless32, int32_t)
ATOMIC_ADD_UNLESS_WIDE(0xFFCE7438, sceKernelAtomicAddUnless64, int64_t)
ATOMIC_ADD_UNLESS_NARROW(0x7FD94393, sceKernelAtomicAddUnless8, int8_t)
ATOMIC_AND_FETCH(0xF9890F7E, sceKernelAtomicAndAndGet16, int16_t)
ATOMIC_AND_FETCH(0x6709D30C, sceKernelAtomicAndAndGet32, int32_t)
ATOMIC_AND_FETCH(0xAED2B370, sceKernelAtomicAndAndGet64, int64_t)
ATOMIC_AND_FETCH(0x86B9170F, sceKernelAtomicAndAndGet8, int8_t)
ATOMIC_CLEAR_FETCH(0xCC36B775, sceKernelAtomicClearAndGet16, int16_t)
ATOMIC_CLEAR_FETCH(0x9DAA0DE8, sceKernelAtomicClearAndGet32, int32_t)
ATOMIC_CLEAR_FETCH(0x264209F3, sceKernelAtomicClearAndGet64, int64_t)
ATOMIC_CLEAR_FETCH(0x17B40010, sceKernelAtomicClearAndGet8, int8_t)
ATOMIC_CLEAR_MASK(0x5FE7DFF8, sceKernelAtomicClearMask16, int16_t)
ATOMIC_CLEAR_MASK(0xE3DF0CB3, sceKernelAtomicClearMask32, int32_t)
ATOMIC_CLEAR_MASK(0x953D118A, sceKernelAtomicClearMask64, int64_t)
ATOMIC_CLEAR_MASK(0xD7D49E36, sceKernelAtomicClearMask8, int8_t)
ATOMIC_CAS(0xE8C01236, sceKernelAtomicCompareAndSet16, int16_t)
ATOMIC_CAS(0x1124A1D4, sceKernelAtomicCompareAndSet32, int32_t)
ATOMIC_CAS(0x1EBDFCCD, sceKernelAtomicCompareAndSet64, int64_t)
ATOMIC_CAS(0x327DB4C0, sceKernelAtomicCompareAndSet8, int8_t)
ATOMIC_DEC_IF_POSITIVE_NARROW(0x323718FB, sceKernelAtomicDecIfPositive16, int16_t)
ATOMIC_DEC_IF_POSITIVE_WIDE(0xCA3294F1, sceKernelAtomicDecIfPositive32, int32_t)
ATOMIC_DEC_IF_POSITIVE_WIDE(0x8BE2A007, sceKernelAtomicDecIfPositive64, int64_t)
ATOMIC_DEC_IF_POSITIVE_NARROW(0x9DABE6C3, sceKernelAtomicDecIfPositive8, int8_t)
ATOMIC_FETCH_ADD(0x5674DB0C, sceKernelAtomicGetAndAdd16, int16_t)
ATOMIC_FETCH_ADD(0x2611CB0B, sceKernelAtomicGetAndAdd32, int32_t)
ATOMIC_FETCH_ADD(0x63DAF37D, sceKernelAtomicGetAndAdd64, int64_t)
ATOMIC_FETCH_ADD(0x27A2AAFA, sceKernelAtomicGetAndAdd8, int8_t)
ATOMIC_FETCH_AND(0x7A0CB056, sceKernelAtomicGetAndAnd16, int16_t)
ATOMIC_FETCH_AND(0x08266595, sceKernelAtomicGetAndAnd32, int32_t)
ATOMIC_FETCH_AND(0x4828BC43, sceKernelAtomicGetAndAnd64, int64_t)
ATOMIC_FETCH_AND(0x53DCA02B, sceKernelAtomicGetAndAnd8, int8_t)
ATOMIC_FETCH_CLEAR(0x57CD7E7D, sceKernelAtomicGetAndClear16, int16_t)
ATOMIC_FETCH_CLEAR(0x0226A017, sceKernelAtomicGetAndClear32, int32_t)
ATOMIC_FETCH_CLEAR(0x436AA0B5, sceKernelAtomicGetAndClear64, int64_t)
ATOMIC_FETCH_CLEAR(0xA10873A4, sceKernelAtomicGetAndClear8, int8_t)
ATOMIC_FETCH_OR(0x31E49E73, sceKernelAtomicGetAndOr16, int16_t)
ATOMIC_FETCH_OR(0x984AD276, sceKernelAtomicGetAndOr32, int32_t)
ATOMIC_FETCH_OR(0xC39186CD, sceKernelAtomicGetAndOr64, int64_t)
ATOMIC_FETCH_OR(0x107A68DF, sceKernelAtomicGetAndOr8, int8_t)
ATOMIC_EXCHANGE(0xF7B9ADFC, sceKernelAtomicGetAndSet16, int16_t)
ATOMIC_EXCHANGE(0x9B43E667, sceKernelAtomicGetAndSet32, int32_t)
ATOMIC_EXCHANGE(0xD82B3A7A, sceKernelAtomicGetAndSet64, int64_t)
ATOMIC_EXCHANGE(0x2672EA0B, sceKernelAtomicGetAndSet8, int8_t)
ATOMIC_FETCH_SUB(0xAC51979C, sceKernelAtomicGetAndSub16, int16_t)
ATOMIC_FETCH_SUB(0x115C516F, sceKernelAtomicGetAndSub32, int32_t)
ATOMIC_FETCH_SUB(0x4AE9C8E6, sceKernelAtomicGetAndSub64, int64_t)
ATOMIC_FETCH_SUB(0xCDF5DF67, sceKernelAtomicGetAndSub8, int8_t)
ATOMIC_FETCH_XOR(0x6E2D0B9E, sceKernelAtomicGetAndXor16, int16_t)
ATOMIC_FETCH_XOR(0x38739E2F, sceKernelAtomicGetAndXor32, int32_t)
ATOMIC_FETCH_XOR(0x6A19BBE9, sceKernelAtomicGetAndXor64, int64_t)
ATOMIC_FETCH_XOR(0x7350B2DF, sceKernelAtomicGetAndXor8, int8_t)
ATOMIC_OR_FETCH(0x8E248EBD, sceKernelAtomicOrAndGet16, int16_t)
ATOMIC_OR_FETCH(0xC3B2F7F8, sceKernelAtomicOrAndGet32, int32_t)
ATOMIC_OR_FETCH(0x809BBC7D, sceKernelAtomicOrAndGet64, int64_t)
ATOMIC_OR_FETCH(0x51693931, sceKernelAtomicOrAndGet8, int8_t)
ATOMIC_SET(0x450BFECF, sceKernelAtomicSet16, int16_t)
ATOMIC_SET(0xB69DA09B, sceKernelAtomicSet32, int32_t)
ATOMIC_SET(0xC8A4339C, sceKernelAtomicSet64, int64_t)
ATOMIC_SET(0xE960FDA2, sceKernelAtomicSet8, int8_t)
ATOMIC_SUB_FETCH(0xC26BBBB1, sceKernelAtomicSubAndGet16, int16_t)
ATOMIC_SUB_FETCH(0x01C9CD92, sceKernelAtomicSubAndGet32, int32_t)
ATOMIC_SUB_FETCH(0x9BB4A94B, sceKernelAtomicSubAndGet64, int64_t)
ATOMIC_SUB_FETCH(0x99E1796E, sceKernelAtomicSubAndGet8, int8_t)
ATOMIC_XOR_FETCH(0x6F524195, sceKernelAtomicXorAndGet16, int16_t)
ATOMIC_XOR_FETCH(0x46940704, sceKernelAtomicXorAndGet32, int32_t)
ATOMIC_XOR_FETCH(0xDDC6866E, sceKernelAtomicXorAndGet64, int64_t)
ATOMIC_XOR_FETCH(0x634AF062, sceKernelAtomicXorAndGet8, int8_t)

static uint64_t runtime_now_us(void)
{
    return host_monotonic_ns() / 1000ull;
}

EXPORT(SceLibKernel, 0x4C4672BF, int, sceKernelGetProcessTime, SceKernelSysClock *clock)
{
    if (!clock)
        return -1;

    *clock = runtime_now_us();
    return 0;
}

EXPORT(SceLibKernel, 0xB110C123, SceUInt64, sceKernelGetProcessTimeWide, void)
{
    return runtime_now_us();
}

EXPORT(SceLibKernel, 0x8AF15B5F, int, sceKernelGetSystemTime, SceKernelSysClock *clock)
{
    if (!clock)
        return -1;

    *clock = runtime_now_us();
    return 0;
}

EXPORT(SceThreadmgr, 0xB70EBAE9, int, _sceKernelGetSystemTime, SceKernelSysClock *clock)
{
    return sceKernelGetSystemTime(clock);
}

EXPORT(SceThreadmgr, 0xF4EE4FA9, SceInt64, sceKernelGetSystemTimeWide, void)
{
    return (SceInt64)runtime_now_us();
}

EXPORT(SceLibKernel, 0xB2700165, int, sceKernelGetRandomNumber, void *output, SceSize size)
{
    static uint32_t state = 0x9e3779b9u;
    uint8_t *bytes = output;

    if (!bytes && size)
        return -1;

    for (SceSize i = 0; i < size; i++) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = (uint8_t)(state >> 24);
    }

    return 0;
}

/* VitaSDK's SceLibRng imports the same function under its own library. */
EXPORT(SceLibRng, 0xB2700165, int, sceKernelGetRandomNumberRng, void *output, SceSize size)
{
    return sceKernelGetRandomNumber(output, size);
}

EXPORT(SceLibKernel, 0x261E2C34, int, sceKernelGetOpenPsId, SceKernelOpenPsId *id)
{
    if (!id)
        return -1;

    for (int i = 0; i < (int)sizeof(id->id); i++)
        id->id[i] = (char)(0x10 + i);

    return 0;
}

/* Compatibility NIDs used by older VitaSDK stubs. */
DECLARE_LIBRARY(SceThreadmgrCoredumpTime, 0x00000052);
DECLARE_LIBRARY(SceLibRng, 0xf9ac7cf8);
