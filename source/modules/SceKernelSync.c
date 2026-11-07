/*
 * SceThreadmgr synchronization primitives: semaphores, event flags and
 * lightweight mutexes.
 *
 * These are exported under the same library names as the thread functions in
 * SceKernelThreadMgr.c; the linker merges both files' export tables.
 *
 * The user-visible entry points live in SceLibKernel, the rest in SceThreadmgr,
 * matching the firmware. Waiting uses host condition variables, so a blocked
 * guest thread really blocks and costs nothing.
 */

#include <errno.h>
#include <m-dict.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2common/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceKernelSync.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"
#include "util.h"

#define SCE_KERNEL_ERROR_EVF_MULTI 0x800280E4

/* ------------------------------------------------------------- semaphores */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    int count;
    int init_count;
    int max_count;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaSemaInfo;

DICT_DEF2(vita_sema_dict, SceUID, M_DEFAULT_OPLIST, VitaSemaInfo *, M_POD_OPLIST)
static vita_sema_dict_t g_semas;
static RwLock g_semas_lock;

static VitaSemaInfo *sema_lookup(SceUID uid)
{
    VitaSemaInfo *sema;

    VitaSemaInfo **slot;

    rwlockReadLock(&g_semas_lock);
    slot = vita_sema_dict_get(g_semas, uid);
    sema = slot ? *slot : NULL;
    rwlockReadUnlock(&g_semas_lock);

    return sema;
}

static void deadline_from_us(struct timespec *ts, SceUInt microseconds)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(microseconds / 1000000u);
    ts->tv_nsec += (long)(microseconds % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

/* Returns 0, or a negative SCE error. `timeout` is in microseconds. */
static int sema_wait(VitaSemaInfo *sema, int need, const SceUInt *timeout)
{
    int result = 0;

    if (need <= 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    while (sema->count < need) {
        sema->waiting++;

        if (timeout) {
            struct timespec ts;

            deadline_from_us(&ts, *timeout);
            result = pthread_cond_timedwait(&sema->cond, &sema->mutex, &ts) == ETIMEDOUT
                         ? SCE_KERNEL_ERROR_WAIT_TIMEOUT
                         : 0;
        } else {
            pthread_cond_wait(&sema->cond, &sema->mutex);
            result = 0;
        }

        sema->waiting--;
        if (result != 0)
            break;
    }

    if (result == 0)
        sema->count -= need;

    pthread_mutex_unlock(&sema->mutex);
    return result;
}

EXPORT(SceLibKernel, 0x1BD67366, SceUID, sceKernelCreateSema, const char *name, SceUInt attr,
       int init_count, int max_count, SceKernelSemaOptParam *option)
{
    VitaSemaInfo *sema;

    (void)option;

    if (init_count < 0 || max_count <= 0 || init_count > max_count)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    sema = calloc(1, sizeof(*sema));
    if (!sema)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    sema->uid = SceSysmem_get_next_uid();
    sema->attr = attr;
    sema->count = init_count;
    sema->init_count = init_count;
    sema->max_count = max_count;
    if (name)
        strncpy(sema->name, name, sizeof(sema->name) - 1);

    pthread_mutex_init(&sema->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sema->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_semas_lock);
    vita_sema_dict_set_at(g_semas, sema->uid, sema);
    rwlockWriteUnlock(&g_semas_lock);

    return sema->uid;
}

EXPORT(SceThreadmgr, 0x0D76458E, SceUID, _sceKernelCreateSema, const char *name, SceUInt attr,
       int init_count, int max_count, SceKernelSemaOptParam *option)
{
    return sceKernelCreateSema(name, attr, init_count, max_count, option);
}

EXPORT(SceThreadmgr, 0xBD06F27C, SceUID, _sceKernelCreateSema_16XX, const char *name, SceUInt attr,
       int init_count, int max_count, SceKernelSemaOptParam *option)
{
    return sceKernelCreateSema(name, attr, init_count, max_count, option);
}

EXPORT(SceLibKernel, 0x297AA2AE, SceUID, sceKernelCreateSema_16XX, const char *name, SceUInt attr,
       int init_count, int max_count, SceKernelSemaOptParam *option)
{
    return sceKernelCreateSema(name, attr, init_count, max_count, option);
}

EXPORT(SceThreadmgr, 0xDB32948A, int, sceKernelDeleteSema, SceUID uid)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;

    rwlockWriteLock(&g_semas_lock);
    vita_sema_dict_erase(g_semas, uid);
    rwlockWriteUnlock(&g_semas_lock);

    pthread_mutex_lock(&sema->mutex);
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    pthread_cond_destroy(&sema->cond);
    pthread_mutex_destroy(&sema->mutex);
    free(sema);

    return 0;
}

EXPORT(SceThreadmgr, 0xE6B761D1, int, sceKernelSignalSema, SceUID uid, int signal)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (signal < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (sema->count + signal > sema->max_count) {
        pthread_mutex_unlock(&sema->mutex);
        return SCE_KERNEL_ERROR_SEMA_OVF;
    }
    sema->count += signal;
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x0C7B834B, int, sceKernelWaitSema, SceUID uid, int signal,
       SceUInt *timeout)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;

    return sema_wait(sema, signal, timeout);
}

EXPORT(SceLibKernel, 0x174692B4, int, sceKernelWaitSemaCB, SceUID uid, int signal,
       SceUInt *timeout)
{
    return sceKernelWaitSema(uid, signal, timeout);
}

EXPORT(SceThreadmgr, 0x45389B6B, int, _sceKernelWaitSema, SceUID uid, int signal,
       SceUInt *timeout)
{
    return sceKernelWaitSema(uid, signal, timeout);
}

EXPORT(SceThreadmgr, 0xF8E06784, int, _sceKernelWaitSemaCB, SceUID uid, int signal,
       SceUInt *timeout)
{
    return sceKernelWaitSemaCB(uid, signal, timeout);
}

EXPORT(SceThreadmgr, 0x866EF048, int, sceKernelPollSema, SceUID uid, int signal)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (signal <= 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (sema->count < signal) {
        pthread_mutex_unlock(&sema->mutex);
        return SCE_KERNEL_ERROR_SEMA_ZERO;
    }
    sema->count -= signal;
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x66D6BF05, int, sceKernelCancelSema, SceUID uid, int set_count,
       int *num_wait_threads)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (set_count < 0 || set_count > sema->max_count)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (num_wait_threads)
        *num_wait_threads = sema->waiting;
    sema->count = set_count;
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x1CAF805D, int, _sceKernelCancelSema, SceUID uid, int set_count,
       int *num_wait_threads)
{
    return sceKernelCancelSema(uid, set_count, num_wait_threads);
}

EXPORT(SceLibKernel, 0x595D3FA6, int, sceKernelGetSemaInfo, SceUID uid, SceKernelSemaInfo *info)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->semaId = sema->uid;
    memcpy(info->name, sema->name, sizeof(info->name));
    info->attr = sema->attr;
    info->initCount = sema->init_count;
    info->maxCount = sema->max_count;

    pthread_mutex_lock(&sema->mutex);
    info->currentCount = sema->count;
    info->numWaitThreads = sema->waiting;
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x0402C633, int, _sceKernelGetSemaInfo, SceUID uid,
       SceKernelSemaInfo *info)
{
    return sceKernelGetSemaInfo(uid, info);
}

EXPORT(SceThreadmgr, 0xCBE235C7, SceUID, sceKernelOpenSema, const char *name)
{
    VitaSemaInfo *result = NULL;
    vita_sema_dict_it_t it;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_semas_lock);
    for (vita_sema_dict_it(it, g_semas); !vita_sema_dict_end_p(it); vita_sema_dict_next(it)) {
        VitaSemaInfo *sema = vita_sema_dict_ref(it)->value;

        if (strncmp(sema->name, name, sizeof(sema->name)) == 0) {
            result = sema;
            break;
        }
    }
    rwlockReadUnlock(&g_semas_lock);

    return result ? result->uid : SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
}

EXPORT(SceThreadmgr, 0xA2D81F9E, int, sceKernelCloseSema, SceUID uid)
{
    return sema_lookup(uid) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
}

/* ------------------------------------------------------------ event flags */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    uint32_t init_pattern;
    uint32_t pattern;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaEventFlagInfo;

DICT_DEF2(vita_event_flag_dict, SceUID, M_DEFAULT_OPLIST, VitaEventFlagInfo *, M_POD_OPLIST)
static vita_event_flag_dict_t g_event_flags;
static RwLock g_event_flags_lock;

static VitaEventFlagInfo *event_flag_lookup(SceUID uid)
{
    VitaEventFlagInfo *evf;

    VitaEventFlagInfo **slot;

    rwlockReadLock(&g_event_flags_lock);
    slot = vita_event_flag_dict_get(g_event_flags, uid);
    evf = slot ? *slot : NULL;
    rwlockReadUnlock(&g_event_flags_lock);

    return evf;
}

static bool event_flag_matches(const VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait)
{
    if (wait & SCE_EVENT_WAITOR)
        return (evf->pattern & bits) != 0;

    return (evf->pattern & bits) == bits;
}

static void event_flag_consume(VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait)
{
    if (wait & SCE_EVENT_WAITCLEAR)
        evf->pattern = 0;
    else if (wait & SCE_EVENT_WAITCLEAR_PAT)
        evf->pattern &= ~bits;
}

static int event_flag_wait(VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait,
                           uint32_t *out_bits, const SceUInt *timeout, bool blocking)
{
    int result = 0;

    pthread_mutex_lock(&evf->mutex);

    for (;;) {
        if (event_flag_matches(evf, bits, wait)) {
            if (out_bits)
                *out_bits = evf->pattern;
            event_flag_consume(evf, bits, wait);
            break;
        }

        if (!blocking) {
            if (out_bits)
                *out_bits = evf->pattern;
            result = SCE_KERNEL_ERROR_EVF_COND;
            break;
        }

        evf->waiting++;
        if (timeout) {
            struct timespec ts;

            deadline_from_us(&ts, *timeout);
            if (pthread_cond_timedwait(&evf->cond, &evf->mutex, &ts) == ETIMEDOUT)
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        } else {
            pthread_cond_wait(&evf->cond, &evf->mutex);
        }
        evf->waiting--;

        if (result != 0) {
            if (out_bits)
                *out_bits = evf->pattern;
            break;
        }
    }

    pthread_mutex_unlock(&evf->mutex);
    return result;
}

EXPORT(SceLibKernel, 0x8516D040, SceUID, sceKernelCreateEventFlag, const char *name, int attr,
       int init_pattern, SceKernelEventFlagOptParam *option)
{
    VitaEventFlagInfo *evf;

    (void)option;

    evf = calloc(1, sizeof(*evf));
    if (!evf)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    evf->uid = SceSysmem_get_next_uid();
    evf->attr = (SceUInt)attr;
    evf->init_pattern = (uint32_t)init_pattern;
    evf->pattern = (uint32_t)init_pattern;
    if (name)
        strncpy(evf->name, name, sizeof(evf->name) - 1);

    pthread_mutex_init(&evf->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&evf->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_event_flags_lock);
    vita_event_flag_dict_set_at(g_event_flags, evf->uid, evf);
    rwlockWriteUnlock(&g_event_flags_lock);

    return evf->uid;
}

EXPORT(SceThreadmgr, 0x38AA5E8E, SceUID, _sceKernelCreateEventFlag, const char *name, int attr,
       int init_pattern, SceKernelEventFlagOptParam *option)
{
    return sceKernelCreateEventFlag(name, attr, init_pattern, option);
}

EXPORT(SceThreadmgr, 0x5840162C, int, sceKernelDeleteEventFlag, SceUID uid)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    rwlockWriteLock(&g_event_flags_lock);
    vita_event_flag_dict_erase(g_event_flags, uid);
    rwlockWriteUnlock(&g_event_flags_lock);

    pthread_mutex_lock(&evf->mutex);
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    pthread_cond_destroy(&evf->cond);
    pthread_mutex_destroy(&evf->mutex);
    free(evf);

    return 0;
}

EXPORT(SceThreadmgr, 0xEC94DFF7, int, sceKernelSetEventFlag, SceUID uid, unsigned int bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    evf->pattern |= bits;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x4CB87CA7, int, sceKernelClearEventFlag, SceUID uid, unsigned int bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    evf->pattern &= ~bits;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x83C0E2AF, int, sceKernelWaitEventFlag, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    return event_flag_wait(evf, bits, wait, out_bits, timeout, true);
}

EXPORT(SceLibKernel, 0xE737B1DF, int, sceKernelWaitEventFlagCB, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    return sceKernelWaitEventFlag(uid, bits, wait, out_bits, timeout);
}

EXPORT(SceThreadmgr, 0xFCE2F728, int, _sceKernelWaitEventFlag, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    return sceKernelWaitEventFlag(uid, bits, wait, out_bits, timeout);
}

EXPORT(SceThreadmgr, 0x401E0C68, int, _sceKernelWaitEventFlagCB, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    return sceKernelWaitEventFlagCB(uid, bits, wait, out_bits, timeout);
}

EXPORT(SceThreadmgr, 0xDAB1B1C8, int, _sceKernelPollEventFlag, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits)
{
    return sceKernelPollEventFlag(uid, bits, wait, out_bits);
}

EXPORT(SceLibKernel, 0x1FBB0FE1, int, sceKernelPollEventFlag, int uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    return event_flag_wait(evf, bits, wait, out_bits, NULL, false);
}

EXPORT(SceLibKernel, 0x8BA4C0C1, int, sceKernelGetEventFlagInfo, SceUID uid,
       SceKernelEventFlagInfo *info)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->evfId = evf->uid;
    memcpy(info->name, evf->name, sizeof(info->name));
    info->attr = evf->attr;
    info->initPattern = evf->init_pattern;

    pthread_mutex_lock(&evf->mutex);
    info->currentPattern = evf->pattern;
    info->numWaitThreads = evf->waiting;
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x106C216F, int, _sceKernelGetEventFlagInfo, SceUID uid,
       SceKernelEventFlagInfo *info)
{
    return sceKernelGetEventFlagInfo(uid, info);
}

int sceKernelCancelEventFlag(SceUID uid, unsigned int set_pattern, int *num_wait_threads);

EXPORT(SceThreadmgr, 0xF76F3056, int, _sceKernelCancelEventFlag, SceUID uid,
       unsigned int set_pattern, int *num_wait_threads)
{
    return sceKernelCancelEventFlag(uid, set_pattern, num_wait_threads);
}

EXPORT(SceLibKernel, 0x2A12D9B7, int, sceKernelCancelEventFlag, SceUID uid, unsigned int set_pattern,
       int *num_wait_threads)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    if (num_wait_threads)
        *num_wait_threads = evf->waiting;
    evf->pattern = set_pattern;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xBC19F8A1, SceUID, sceKernelOpenEventFlag, const char *name)
{
    VitaEventFlagInfo *result = NULL;
    vita_event_flag_dict_it_t it;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_event_flags_lock);
    for (vita_event_flag_dict_it(it, g_event_flags); !vita_event_flag_dict_end_p(it);
         vita_event_flag_dict_next(it)) {
        VitaEventFlagInfo *evf = vita_event_flag_dict_ref(it)->value;

        if (strncmp(evf->name, name, sizeof(evf->name)) == 0) {
            result = evf;
            break;
        }
    }
    rwlockReadUnlock(&g_event_flags_lock);

    return result ? result->uid : SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
}

EXPORT(SceThreadmgr, 0x9A68F547, int, sceKernelCloseEventFlag, SceUID uid)
{
    return event_flag_lookup(uid) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
}

/* ------------------------------------------------------------ lw mutexes */

/*
 * Lightweight mutexes live in guest-provided work structures. A host mutex is
 * allocated on create and its address is kept in the work area's first word,
 * while a small registry keeps the metadata needed by the introspection APIs
 * keyed by the UID the kernel would assign.
 */

#define VITA_LW_MUTEX_SLOTS 64

static VitaLwMutex *g_lw_mutexes[VITA_LW_MUTEX_SLOTS];
static pthread_mutex_t g_lw_mutex_table_lock = PTHREAD_MUTEX_INITIALIZER;

static SceUID sync_current_thread_uid(void)
{
    VitaThreadInfo *info = SceKernelThreadMgr_get_thread_info();

    return info ? info->uid : 0;
}

VitaLwMutex *SceKernelSync_lw_mutex_from_work(SceKernelLwMutexWork *work)
{
    void *pointer;

    /* Page zero is never mapped on Vita; reject obviously invalid work areas
     * instead of dereferencing them on the host. */
    if (!work || (uintptr_t)work < 0x10000)
        return NULL;

    memcpy(&pointer, &work->data[0], sizeof(pointer));
    return pointer;
}

VitaLwMutex *SceKernelSync_lw_mutex_lookup(SceUID uid)
{
    VitaLwMutex *result = NULL;

    pthread_mutex_lock(&g_lw_mutex_table_lock);
    for (int i = 0; i < VITA_LW_MUTEX_SLOTS; i++) {
        if (g_lw_mutexes[i] && g_lw_mutexes[i]->uid == uid) {
            result = g_lw_mutexes[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_lw_mutex_table_lock);

    return result;
}

static int lw_mutex_register(VitaLwMutex *lw)
{
    int slot = -1;

    pthread_mutex_lock(&g_lw_mutex_table_lock);
    for (int i = 0; i < VITA_LW_MUTEX_SLOTS; i++) {
        if (!g_lw_mutexes[i]) {
            g_lw_mutexes[i] = lw;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lw_mutex_table_lock);

    return slot;
}

static void lw_mutex_unregister(VitaLwMutex *lw)
{
    pthread_mutex_lock(&g_lw_mutex_table_lock);
    for (int i = 0; i < VITA_LW_MUTEX_SLOTS; i++) {
        if (g_lw_mutexes[i] == lw) {
            g_lw_mutexes[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_lw_mutex_table_lock);
}

void SceKernelSync_lw_mutex_fill_info(const VitaLwMutex *lw, VitaLwMutexInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->uid = lw->uid;
    memcpy(info->name, lw->name, sizeof(info->name));
    info->attr = (int)lw->attr;
    info->work = lw->work;
    info->init_count = lw->init_count;
    info->current_count = lw->current_count;
    info->current_owner_id = lw->owner;
    info->num_wait_threads = lw->waiting;
}

int SceKernelSync_lw_mutex_get_info(SceUID uid, VitaLwMutexInfo *info, SceSize size)
{
    VitaLwMutex *lw = SceKernelSync_lw_mutex_lookup(uid);

    if (!lw)
        return SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    if (size != 0 && size < sizeof(VitaLwMutexInfo)) {
        VitaLwMutexInfo local;

        SceKernelSync_lw_mutex_fill_info(lw, &local);
        local.size = size;
        memcpy(info, &local, size);
    } else {
        SceKernelSync_lw_mutex_fill_info(lw, info);
    }

    return 0;
}

EXPORT(SceThreadmgr, 0xFEC9E946, int, _sceKernelGetLwMutexInfoById, SceUID lwmutex_id,
       VitaLwMutexInfo *info, SceSize size)
{
    return SceKernelSync_lw_mutex_get_info(lwmutex_id, info, size);
}

EXPORT(SceLibKernel, 0xF7D8F1FC, int, sceKernelGetLwMutexInfo, SceKernelLwMutexWork *work,
       VitaLwMutexInfo *info)
{
    VitaLwMutex *lw = SceKernelSync_lw_mutex_from_work(work);

    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    return SceKernelSync_lw_mutex_get_info(lw->uid, info, 0);
}

EXPORT(SceLibKernel, 0x4846613D, int, sceKernelGetLwMutexInfoById, SceUID lwmutex_id,
       VitaLwMutexInfo *info)
{
    SceSize size = info ? info->size : 0;

    return SceKernelSync_lw_mutex_get_info(lwmutex_id, info, size);
}

EXPORT(SceLibKernel, 0xDA6EC8EF, int, sceKernelCreateLwMutex, SceKernelLwMutexWork *work,
       const char *name, unsigned int attr, int init_count, const SceKernelLwMutexOptParam *option)
{
    VitaLwMutex *lw;
    pthread_mutexattr_t mutex_attr;

    (void)option;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (init_count < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    lw = calloc(1, sizeof(*lw));
    if (!lw)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    lw->uid = SceSysmem_get_next_uid();
    lw->attr = attr;
    lw->work = work;
    lw->init_count = init_count;
    if (name)
        strncpy(lw->name, name, sizeof(lw->name) - 1);

    pthread_mutexattr_init(&mutex_attr);
    if (attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE)
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lw->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    if (init_count > 0) {
        for (int i = 0; i < init_count; i++)
            pthread_mutex_lock(&lw->mutex);
        lw->current_count = init_count;
        lw->owner = sync_current_thread_uid();
    }

    if (lw_mutex_register(lw) < 0) {
        pthread_mutex_destroy(&lw->mutex);
        free(lw);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    memset(work, 0, sizeof(*work));
    memcpy(&work->data[0], &lw, sizeof(lw));

    return 0;
}

EXPORT(SceLibKernel, 0x244E76D2, int, sceKernelDeleteLwMutex, SceKernelLwMutexWork *work)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = SceKernelSync_lw_mutex_from_work(work);
    if (!lw)
        return 0;

    lw_mutex_unregister(lw);
    pthread_mutex_destroy(&lw->mutex);
    free(lw);
    memset(work, 0, sizeof(*work));

    return 0;
}

EXPORT(SceLibKernel, 0x46E7BE7B, int, sceKernelLockLwMutex, SceKernelLwMutexWork *work,
       int count, SceUInt *timeout)
{
    VitaLwMutex *lw;

    (void)timeout;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = SceKernelSync_lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (count < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    lw->waiting++;
    for (int i = 0; i < count; i++)
        pthread_mutex_lock(&lw->mutex);
    lw->waiting--;

    lw->current_count += count;
    if (count > 0)
        lw->owner = sync_current_thread_uid();

    return 0;
}

EXPORT(SceLibKernel, 0xA6A2C915, int, sceKernelTryLockLwMutex, SceKernelLwMutexWork *work,
       int count)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = SceKernelSync_lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (count < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    for (int i = 0; i < count; i++) {
        if (pthread_mutex_trylock(&lw->mutex) != 0) {
            if (getenv("VITA2NATIVE_TRACE_SYNC"))
                LOG("sceKernelTryLockLwMutex(\"%s\") failed on entry %d/%d (owner %d)",
                    lw->name, i, count, lw->owner);
            return SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN;
        }
    }

    lw->current_count += count;
    if (count > 0)
        lw->owner = sync_current_thread_uid();

    return 0;
}

EXPORT(SceLibKernel, 0x91FA6614, int, sceKernelUnlockLwMutex, SceKernelLwMutexWork *work,
       int count)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = SceKernelSync_lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (count < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    for (int i = 0; i < count; i++)
        pthread_mutex_unlock(&lw->mutex);

    if (count >= lw->current_count)
        lw->current_count = 0;
    else
        lw->current_count -= count;
    if (lw->current_count == 0)
        lw->owner = 0;

    return 0;
}

int SceKernelSync_classify_uid(SceUID uid)
{
    if (sema_lookup(uid))
        return SCE_KERNEL_TMID_Semaphore;
    if (event_flag_lookup(uid))
        return SCE_KERNEL_TMID_EventFlag;

    return -1;
}

int SceKernelSync_init(void)
{
    vita_sema_dict_init(g_semas);
    vita_event_flag_dict_init(g_event_flags);
    rwlockInit(&g_semas_lock);
    rwlockInit(&g_event_flags_lock);
    return 0;
}

int SceKernelSync_finish(void)
{
    vita_sema_dict_clear(g_semas);
    vita_event_flag_dict_clear(g_event_flags);
    rwlockDestroy(&g_semas_lock);
    rwlockDestroy(&g_event_flags_lock);
    return 0;
}
