/*
 * Exercises the synchronization primitives the guest imports: semaphores,
 * event flags and lightweight mutexes. Includes a blocking hand-off between
 * two host threads, which is the part that a stub would silently break.
 */

#include <pthread.h>
#include <stdbool.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"

/* The module allocates UIDs this way; SceSysmem itself is not linked here. */
static _Atomic SceUID g_next_uid = 1;

SceUID SceSysmem_get_next_uid(void)
{
    return atomic_fetch_add(&g_next_uid, 1);
}

/* Owner tracking is best-effort; the thread manager is not linked here. */
VitaThreadInfo *SceKernelThreadMgr_get_thread_info(void)
{
    return NULL;
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

typedef struct {
    SceUID sema;
    atomic_int done;
    int result;
} WaitProbe;

static void *wait_worker(void *arg)
{
    WaitProbe *probe = arg;

    probe->result = sceKernelWaitSema(probe->sema, 1, NULL);
    atomic_store(&probe->done, 1);
    return NULL;
}

/* Blocks until the plugin reports a waiter on the semaphore. */
static bool wait_for_waiter(SceUID sema)
{
    for (int i = 0; i < 200; i++) {
        SceKernelSemaInfo info;
        struct timespec delay = {0, 5 * 1000 * 1000};

        if (sceKernelGetSemaInfo(sema, &info) == 0 && info.numWaitThreads > 0)
            return true;
        nanosleep(&delay, NULL);
    }
    return false;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ull + (uint64_t)now.tv_nsec / 1000000ull;
}

/* Signals a semaphore once, midway through someone else's wait: enough to wake
   the waiter, not enough to satisfy it. */
static void *half_signal_worker(void *arg)
{
    struct timespec delay = {0, 100 * 1000 * 1000};
    SceUID sema = *(SceUID *)arg;

    nanosleep(&delay, NULL);
    sceKernelSignalSema(sema, 1);
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

    /* --- a bounded wait keeps its deadline ----------------------------- */
    /*
     * The wait needs two counts and gets one after 100 ms. Recomputing the
     * deadline on every wake (the old behaviour) made a 200 ms wait return at
     * ~300 ms; the deadline is now fixed when the wait begins.
     */
    {
        SceUID deadline_sema = sceKernelCreateSema("test-sema-deadline", 0, 0, 2, NULL);
        uint64_t started;
        uint64_t elapsed;
        SceUInt timeout = 200000;
        pthread_t signaller;
        int waited;

        if (deadline_sema < 0) {
            puts("sync_smoke: create deadline sema failed");
            return 1;
        }

        if (pthread_create(&signaller, NULL, half_signal_worker, &deadline_sema) != 0) {
            puts("sync_smoke: could not create the signaller thread");
            return 1;
        }

        started = monotonic_ms();
        waited = sceKernelWaitSema(deadline_sema, 2, &timeout);
        elapsed = monotonic_ms() - started;
        pthread_join(signaller, NULL);

        if (waited != SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
            printf("sync_smoke: insufficient signal returned 0x%x, expected timeout\n",
                   (unsigned)waited);
            return 1;
        }
        if (elapsed < 150 || elapsed > 280) {
            printf("sync_smoke: 200 ms wait returned after %llu ms\n",
                   (unsigned long long)elapsed);
            return 1;
        }

        sceKernelDeleteSema(deadline_sema);
    }

    /* --- cancelling a waiter reports the cancellation ------------------- */
    {
        SceUID cancel_sema = sceKernelCreateSema("test-sema-cancel", 0, 0, 1, NULL);
        WaitProbe probe;
        pthread_t worker;
        int waiters = 0;

        probe.sema = cancel_sema;
        atomic_init(&probe.done, 0);
        probe.result = -1;

        if (cancel_sema < 0 ||
            pthread_create(&worker, NULL, wait_worker, &probe) != 0) {
            puts("sync_smoke: could not set up the cancel case");
            return 1;
        }
        if (!wait_for_waiter(cancel_sema)) {
            puts("sync_smoke: the cancel waiter never blocked");
            return 1;
        }

        if (sceKernelCancelSema(cancel_sema, -1, &waiters) != 0 || waiters != 1) {
            puts("sync_smoke: CancelSema did not report the blocked waiter");
            return 1;
        }
        pthread_join(worker, NULL);
        if (probe.result != SCE_KERNEL_ERROR_WAIT_CANCEL) {
            printf("sync_smoke: cancelled wait returned 0x%x\n", (unsigned)probe.result);
            return 1;
        }

        /* A later waiter is independent of that cancellation. */
        if (sceKernelSignalSema(cancel_sema, 1) != 0 ||
            sceKernelWaitSema(cancel_sema, 1, NULL) != 0) {
            puts("sync_smoke: a fresh wait after cancellation failed");
            return 1;
        }
        sceKernelDeleteSema(cancel_sema);
    }

    /* --- deleting a semaphore wakes its waiters with WAIT_DELETE -------- */
    {
        SceUID delete_sema = sceKernelCreateSema("test-sema-delete", 0, 0, 1, NULL);
        WaitProbe probe;
        pthread_t worker;

        probe.sema = delete_sema;
        atomic_init(&probe.done, 0);
        probe.result = -1;

        if (delete_sema < 0 || pthread_create(&worker, NULL, wait_worker, &probe) != 0) {
            puts("sync_smoke: could not set up the delete case");
            return 1;
        }
        if (!wait_for_waiter(delete_sema)) {
            puts("sync_smoke: the delete waiter never blocked");
            return 1;
        }

        if (sceKernelDeleteSema(delete_sema) != 0) {
            puts("sync_smoke: deleting a semaphore with a waiter failed");
            return 1;
        }
        pthread_join(worker, NULL);
        if (probe.result != SCE_KERNEL_ERROR_WAIT_DELETE) {
            printf("sync_smoke: a deleted semaphore's waiter returned 0x%x\n",
                   (unsigned)probe.result);
            return 1;
        }
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
