/*
 * Exercise the EABI helpers exported by SceLibKernel. The divide/modulo
 * helpers use a non-standard return convention, so on ARM they are called
 * from inline assembly and both r0/r1 (and r2/r3 for the 64-bit variants) are
 * checked.
 */

#include <stdint.h>
#include <stdio.h>

int32_t vita_aeabi_idiv(int32_t a, int32_t b);
uint32_t vita_aeabi_uidiv(uint32_t a, uint32_t b);
int64_t vita_aeabi_lmul(int64_t a, int64_t b);
int64_t vita_aeabi_llsl(int64_t value, int shift);
uint64_t vita_aeabi_llsr(uint64_t value, int shift);
int64_t vita_aeabi_lasr(int64_t value, int shift);
int vita_aeabi_lcmp(int64_t a, int64_t b);
int vita_aeabi_ulcmp(uint64_t a, uint64_t b);

#if defined(__arm__)
void aeabi_idivmod_naked(void);
void aeabi_uidivmod_naked(void);
void aeabi_ldivmod_naked(void);
void aeabi_uldivmod_naked(void);

static void call_idivmod(int32_t a, int32_t b, int32_t *q, int32_t *r)
{
    register int32_t r0 __asm__("r0") = a;
    register int32_t r1 __asm__("r1") = b;

    __asm__ volatile("bl aeabi_idivmod_naked" : "+r"(r0), "+r"(r1) : : "lr", "memory");
    *q = r0;
    *r = r1;
}

static void call_uidivmod(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r)
{
    register uint32_t r0 __asm__("r0") = a;
    register uint32_t r1 __asm__("r1") = b;

    __asm__ volatile("bl aeabi_uidivmod_naked" : "+r"(r0), "+r"(r1) : : "lr", "memory");
    *q = r0;
    *r = r1;
}

static void call_ldivmod(int64_t a, int64_t b, int64_t *q, int64_t *r)
{
    register uint32_t r0 __asm__("r0") = (uint32_t)a;
    register uint32_t r1 __asm__("r1") = (uint32_t)((uint64_t)a >> 32);
    register uint32_t r2 __asm__("r2") = (uint32_t)b;
    register uint32_t r3 __asm__("r3") = (uint32_t)((uint64_t)b >> 32);

    __asm__ volatile("bl aeabi_ldivmod_naked"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                     :
                     : "lr", "memory");
    *q = (int64_t)(((uint64_t)r1 << 32) | r0);
    *r = (int64_t)(((uint64_t)r3 << 32) | r2);
}

static void call_uldivmod(uint64_t a, uint64_t b, uint64_t *q, uint64_t *r)
{
    register uint32_t r0 __asm__("r0") = (uint32_t)a;
    register uint32_t r1 __asm__("r1") = (uint32_t)(a >> 32);
    register uint32_t r2 __asm__("r2") = (uint32_t)b;
    register uint32_t r3 __asm__("r3") = (uint32_t)(b >> 32);

    __asm__ volatile("bl aeabi_uldivmod_naked"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                     :
                     : "lr", "memory");
    *q = ((uint64_t)r1 << 32) | r0;
    *r = ((uint64_t)r3 << 32) | r2;
}
#endif

int main(void)
{
    if (vita_aeabi_idiv(7, 2) != 3 || vita_aeabi_idiv(-7, 2) != -3 ||
        vita_aeabi_uidiv(7, 2) != 3) {
        puts("aeabi_smoke: division failed");
        return 1;
    }
    if (vita_aeabi_idiv(1, 0) != 0 || vita_aeabi_uidiv(1, 0) != 0) {
        puts("aeabi_smoke: division by zero failed");
        return 1;
    }
    if (vita_aeabi_lmul(0x100000000ll, 3) != 0x300000000ll) {
        puts("aeabi_smoke: 64-bit multiply failed");
        return 1;
    }
    if (vita_aeabi_llsl(1, 40) != 0x10000000000ll || vita_aeabi_llsr(0x10000000000ull, 40) != 1 ||
        vita_aeabi_lasr(-16, 2) != -4 || vita_aeabi_lasr(-1, 64) != -1) {
        puts("aeabi_smoke: shifts failed");
        return 1;
    }
    if (vita_aeabi_lcmp(-1, 1) != -1 || vita_aeabi_lcmp(1, 1) != 0 ||
        vita_aeabi_ulcmp(1, UINT64_MAX) != -1) {
        puts("aeabi_smoke: comparison failed");
        return 1;
    }

#if defined(__arm__)
    {
        int32_t q, r;
        uint32_t uq, ur;
        int64_t lq, lr;
        uint64_t ulq, ulr;

        call_idivmod(7, 2, &q, &r);
        if (q != 3 || r != 1) {
            printf("aeabi_smoke: idivmod returned q=%d r=%d\n", q, r);
            return 1;
        }
        call_idivmod(-7, 2, &q, &r);
        if (q != -3 || r != -1) {
            printf("aeabi_smoke: signed idivmod returned q=%d r=%d\n", q, r);
            return 1;
        }
        call_uidivmod(7, 2, &uq, &ur);
        if (uq != 3 || ur != 1) {
            puts("aeabi_smoke: uidivmod failed");
            return 1;
        }
        call_ldivmod(0x100000001ll, 2, &lq, &lr);
        if (lq != 0x80000000ll || lr != 1) {
            printf("aeabi_smoke: ldivmod returned q=%lld r=%lld\n", (long long)lq,
                   (long long)lr);
            return 1;
        }
        call_uldivmod(0xFFFFFFFFFFFFFFFFull, 0x100000000ull, &ulq, &ulr);
        if (ulq != 0xFFFFFFFFull || ulr != 0xFFFFFFFFull) {
            puts("aeabi_smoke: uldivmod failed");
            return 1;
        }
    }
#endif

    puts("aeabi_smoke: ok");
    return 0;
}
