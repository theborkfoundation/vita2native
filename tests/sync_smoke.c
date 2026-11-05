/*
 * Exercises the synchronization primitives the guest imports: semaphores,
 * event flags and lightweight mutexes. Includes a blocking hand-off between
 * two host threads, which is the part that a stub would silently break.
 */

#include <pthread.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"

/* The module allocates UIDs this way; SceSysmem itself is not linked here. */
static _Atomic SceUID g_next_uid = 1;

SceUID SceSysmem_get_next_uid(void)
{
    return atomic_fetch_add(&g_next_uid, 1);
}

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int init_count, int max_count,
                           SceKernelSemaOptParam *option);
int sceKernelDeleteSema(SceUID uid);
int sceKernelSignalSema(SceUID uid, int signal);
int sceKernelWaitSema(SceUID uid, int signal, SceUInt *timeout);
int sceKernelPollSema(SceUID uid, int signal);
int sceKernelGetSemaInfo(SceUID uid, SceKernelSemaInfo *info);

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *opt);
int sceKernelDeleteEventFlag(SceUID uid);
int sceKernelSetEventFlag(SceUID uid, unsigned int bits);
int sceKernelClearEventFlag(SceUID uid, unsigned int bits);
int sceKernelWaitEventFlag(SceUID uid, unsigned int bits, unsigned int wait,
                           unsigned int *out_bits, SceUInt *timeout);
int sceKernelPollEventFlag(int uid, unsigned int bits, unsigned int wait,
                           unsigned int *out_bits);

int sceKernelCreateLwMutex(SceKernelLwMutexWork *work, const char *name, unsigned int attr,
                           int init_count, const SceKernelLwMutexOptParam *option);
int sceKernelLockLwMutex(SceKernelLwMutexWork *work, int count, SceUInt *timeout);
int sceKernelTryLockLwMutex(SceKernelLwMutexWork *work, int count);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *work, int count);
int sceKernelDeleteLwMutex(SceKernelLwMutexWork *work);

typedef struct {
    SceUID sema;
    atomic_int ran;
    int result;
} Handoff;

static void *handoff_worker(void *arg)
{
    Handoff *handoff = arg;

    handoff->result = sceKernelWaitSema(handoff->sema, 1, NULL);
    atomic_store(&handoff->ran, 1);
    return NULL;
}

int main(void)
{
    SceKernelSemaInfo sema_info;
    SceKernelLwMutexWork work;
    unsigned int out_bits = 0;
    Handoff handoff;
    pthread_t thread;
    SceUID sema, evf;
    int ret;

    SceKernelSync_init();

    /* --- semaphores ---------------------------------------------------- */
    sema = sceKernelCreateSema("test-sema", 0, 0, 1, NULL);
    if (sema < 0) {
        printf("sync_smoke: create sema failed: 0x%x\n", (unsigned)sema);
        return 1;
    }

    if (sceKernelPollSema(sema, 1) != SCE_KERNEL_ERROR_SEMA_ZERO) {
        puts("sync_smoke: poll on an empty semaphore did not fail");
        return 1;
    }

    handoff.sema = sema;
    atomic_init(&handoff.ran, 0);
    handoff.result = -1;

    if (pthread_create(&thread, NULL, handoff_worker, &handoff) != 0) {
        puts("sync_smoke: could not create the worker thread");
        return 1;
    }

    /* Give the worker time to block, then release it. */
    while (atomic_load(&handoff.ran) != 0)
        ;
    sceKernelSignalSema(sema, 1);
    pthread_join(thread, NULL);

    if (handoff.result != 0 || atomic_load(&handoff.ran) != 1) {
        puts("sync_smoke: blocked wait did not resume");
        return 1;
    }

    if (sceKernelGetSemaInfo(sema, &sema_info) != 0 || sema_info.currentCount != 0) {
        puts("sync_smoke: sema info mismatch");
        return 1;
    }

    if (sceKernelDeleteSema(sema) != 0) {
        puts("sync_smoke: delete sema failed");
        return 1;
    }

    /* --- event flags --------------------------------------------------- */
    evf = sceKernelCreateEventFlag("test-evf", 0, 0, NULL);
    if (evf < 0) {
        printf("sync_smoke: create evf failed: 0x%x\n", (unsigned)evf);
        return 1;
    }

    if (sceKernelSetEventFlag(evf, 0x3) != 0) {
        puts("sync_smoke: set evf failed");
        return 1;
    }

    ret = sceKernelWaitEventFlag(evf, 0x3, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, &out_bits,
                                 NULL);
    if (ret != 0 || out_bits != 0x3) {
        printf("sync_smoke: wait evf failed: 0x%x out=0x%x\n", (unsigned)ret, out_bits);
        return 1;
    }

    if (sceKernelPollEventFlag(evf, 0x3, SCE_EVENT_WAITAND, &out_bits) !=
        (int)SCE_KERNEL_ERROR_EVF_COND) {
        puts("sync_smoke: evf was not cleared by WAITCLEAR");
        return 1;
    }

    if (sceKernelDeleteEventFlag(evf) != 0) {
        puts("sync_smoke: delete evf failed");
        return 1;
    }

    /* --- lightweight mutex --------------------------------------------- */
    if (sceKernelCreateLwMutex(&work, "test-lw", 0, 0, NULL) != 0) {
        puts("sync_smoke: create lw mutex failed");
        return 1;
    }

    if (sceKernelLockLwMutex(&work, 1, NULL) != 0 ||
        sceKernelTryLockLwMutex(&work, 1) != SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN) {
        puts("sync_smoke: lw mutex ownership is wrong");
        return 1;
    }

    if (sceKernelUnlockLwMutex(&work, 1) != 0 || sceKernelDeleteLwMutex(&work) != 0) {
        puts("sync_smoke: lw mutex cleanup failed");
        return 1;
    }

    SceKernelSync_finish();

    puts("sync_smoke: ok");
    return 0;
}
