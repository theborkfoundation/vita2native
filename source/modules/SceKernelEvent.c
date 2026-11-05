/*
 * SceKernel simple events.
 *
 * A simple event is a 32-bit notification pattern plus a 64-bit user value.
 * Waiters block until any requested bit is set. Pulse notifies waiters without
 * requiring an explicit clear. There is no callback delivery thread; the
 * callback-configured entry points behave like their plain counterparts.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/error.h>
#include <psp2/types.h>

#include "host.h"
#include "module.h"
#include "modules/SceSysmem.h"

#define VITA_EVENT_SLOTS 64

typedef struct {
    SceSize size;
} SceKernelSimpleEventOptParam;

typedef struct {
    SceSize size;
    SceUID eventId;
    char name[32];
    SceUInt32 attr;
    SceUInt32 eventPattern;
    uint64_t userData;
    SceUInt32 numWaitThreads;
    SceInt32 reserved;
} SceKernelEventInfo;

typedef struct {
    bool used;
    SceUID uid;
    int refs;
    uint32_t attr;
    char name[32];
    uint32_t pattern;
    uint32_t waiters;
    uint64_t user_data;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaEvent;

static VitaEvent g_events[VITA_EVENT_SLOTS];
static pthread_mutex_t g_event_table_lock = PTHREAD_MUTEX_INITIALIZER;

static VitaEvent *event_lookup(SceUID uid)
{
    pthread_mutex_lock(&g_event_table_lock);
    for (int i = 0; i < VITA_EVENT_SLOTS; i++) {
        if (g_events[i].used && g_events[i].uid == uid) {
            pthread_mutex_unlock(&g_event_table_lock);
            return &g_events[i];
        }
    }
    pthread_mutex_unlock(&g_event_table_lock);

    return NULL;
}

static SceUID event_create(const char *name, uint32_t attr, uint32_t init_pattern)
{
    SceUID uid = 0;

    pthread_mutex_lock(&g_event_table_lock);
    for (int i = 0; i < VITA_EVENT_SLOTS; i++) {
        VitaEvent *event = &g_events[i];

        if (event->used)
            continue;

        memset(event, 0, sizeof(*event));
        pthread_mutex_init(&event->mutex, NULL);
        pthread_cond_init(&event->cond, NULL);
        event->used = true;
        event->uid = SceSysmem_get_next_uid();
        event->refs = 1;
        event->attr = attr;
        event->pattern = init_pattern;
        if (name)
            strncpy(event->name, name, sizeof(event->name) - 1);
        uid = event->uid;
        break;
    }
    pthread_mutex_unlock(&g_event_table_lock);

    return uid;
}

static int event_open(const char *name)
{
    int uid = SCE_KERNEL_ERROR_UNKNOWN_UID;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&g_event_table_lock);
    for (int i = 0; i < VITA_EVENT_SLOTS; i++) {
        if (g_events[i].used && strncmp(g_events[i].name, name, sizeof(g_events[i].name)) == 0) {
            g_events[i].refs++;
            uid = (int)g_events[i].uid;
            break;
        }
    }
    pthread_mutex_unlock(&g_event_table_lock);

    return uid;
}

static int event_close(SceUID uid)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_event_table_lock);
    if (event->refs > 0)
        event->refs--;
    pthread_mutex_unlock(&g_event_table_lock);

    return 0;
}

static int event_delete(SceUID uid)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_event_table_lock);
    pthread_cond_destroy(&event->cond);
    pthread_mutex_destroy(&event->mutex);
    memset(event, 0, sizeof(*event));
    pthread_mutex_unlock(&g_event_table_lock);

    return 0;
}

static int event_set(SceUID uid, uint32_t set_pattern, uint64_t user_data)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&event->mutex);
    event->pattern |= set_pattern;
    event->user_data = user_data;
    pthread_cond_broadcast(&event->cond);
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

static int event_clear(SceUID uid, uint32_t clear_pattern)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&event->mutex);
    event->pattern &= ~clear_pattern;
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

static int event_pulse(SceUID uid, uint32_t set_pattern, uint64_t user_data)
{
    return event_set(uid, set_pattern, user_data);
}

static int event_cancel(SceUID uid)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&event->mutex);
    pthread_cond_broadcast(&event->cond);
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

static int event_wait(SceUID uid, uint32_t bit_pattern, uint32_t *result_pattern,
                      uint64_t *user_data, uint32_t *timeout)
{
    VitaEvent *event = event_lookup(uid);
    int result = 0;

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&event->mutex);
    event->waiters++;

    if (timeout) {
        struct timespec ts;
        uint64_t micros = *timeout;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(micros / 1000000ull);
        ts.tv_nsec += (long)(micros % 1000000ull) * 1000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        while ((event->pattern & bit_pattern) == 0) {
            result = pthread_cond_timedwait(&event->cond, &event->mutex, &ts);
            if (result == ETIMEDOUT) {
                *timeout = 0;
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                break;
            }
        }
    } else {
        while ((event->pattern & bit_pattern) == 0)
            pthread_cond_wait(&event->cond, &event->mutex);
    }

    event->waiters--;

    if (result == 0) {
        if (result_pattern)
            *result_pattern = event->pattern;
        if (user_data)
            *user_data = event->user_data;
    }

    pthread_mutex_unlock(&event->mutex);

    return result;
}

static int event_poll(SceUID uid, uint32_t bit_pattern, uint32_t *result_pattern,
                      uint64_t *user_data)
{
    VitaEvent *event = event_lookup(uid);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&event->mutex);
    if ((event->pattern & bit_pattern) == 0) {
        pthread_mutex_unlock(&event->mutex);
        return SCE_KERNEL_ERROR_EVF_COND;
    }

    if (result_pattern)
        *result_pattern = event->pattern;
    if (user_data)
        *user_data = event->user_data;
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0xE6DB2494, SceUID, sceKernelCreateSimpleEvent, const char *name,
       SceUInt32 attr, SceUInt32 init_pattern, const SceKernelSimpleEventOptParam *opt_param)
{
    (void)opt_param;
    return event_create(name, attr, init_pattern);
}

EXPORT(SceThreadmgr, 0x9C187FAD, SceUID, _sceKernelCreateSimpleEvent, const char *name,
       SceUInt32 attr, SceUInt32 init_pattern, const SceKernelSimpleEventOptParam *opt_param)
{
    (void)opt_param;
    return event_create(name, attr, init_pattern);
}

EXPORT(SceThreadmgr, 0x4E1E4DF8, SceUID, sceKernelOpenSimpleEvent, const char *name)
{
    return event_open(name);
}

EXPORT(SceThreadmgr, 0xFEF4CA53, int, sceKernelCloseSimpleEvent, SceUID event_id)
{
    return event_close(event_id);
}

EXPORT(SceThreadmgr, 0x208CFE28, int, sceKernelDeleteSimpleEvent, SceUID event_id)
{
    return event_delete(event_id);
}

EXPORT(SceThreadmgr, 0x324218CD, int, sceKernelSetEvent, SceUID event_id, SceUInt32 set_pattern,
       SceUInt64 user_data)
{
    return event_set(event_id, set_pattern, user_data);
}

EXPORT(SceThreadmgr, 0x7B2A4B28, int, sceKernelClearEvent, SceUID event_id,
       SceUInt32 clear_pattern)
{
    return event_clear(event_id, clear_pattern);
}

EXPORT(SceThreadmgr, 0x8D27BAD6, int, sceKernelPulseEvent, SceUID event_id, SceUInt32 set_pattern,
       SceUInt64 user_data)
{
    return event_pulse(event_id, set_pattern, user_data);
}

EXPORT(SceLibKernel, 0x603AB770, int, sceKernelCancelEvent, SceUID event_id)
{
    return event_cancel(event_id);
}

EXPORT(SceThreadmgr, 0x29483405, int, _sceKernelCancelEvent, SceUID event_id)
{
    return event_cancel(event_id);
}

EXPORT(SceLibKernel, 0xB01387D5, int, sceKernelCancelEventWithSetPattern, SceUID event_id,
       SceUInt32 set_pattern)
{
    event_set(event_id, set_pattern, 0);
    return event_cancel(event_id);
}

EXPORT(SceThreadmgr, 0x8E68E870, int, _sceKernelCancelEventWithSetPattern, SceUID event_id,
       SceUInt32 set_pattern)
{
    event_set(event_id, set_pattern, 0);
    return event_cancel(event_id);
}

EXPORT(SceLibKernel, 0x120F03AF, int, sceKernelWaitEvent, SceUID event_id, SceUInt32 bit_pattern,
       SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout)
{
    return event_wait(event_id, bit_pattern, result_pattern, user_data, timeout);
}

EXPORT(SceLibKernel, 0xA0490795, int, sceKernelWaitEventCB, SceUID event_id, SceUInt32 bit_pattern,
       SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout)
{
    return event_wait(event_id, bit_pattern, result_pattern, user_data, timeout);
}

EXPORT(SceThreadmgr, 0x4E0EA70D, int, _sceKernelWaitEvent, SceUID event_id, SceUInt32 bit_pattern,
       SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout)
{
    return event_wait(event_id, bit_pattern, result_pattern, user_data, timeout);
}

EXPORT(SceThreadmgr, 0x7D483C33, int, _sceKernelWaitEventCB, SceUID event_id,
       SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data,
       SceUInt32 *timeout)
{
    return event_wait(event_id, bit_pattern, result_pattern, user_data, timeout);
}

EXPORT(SceLibKernel, 0x241F3634, int, sceKernelPollEvent, SceUID event_id, SceUInt32 bit_pattern,
       SceUInt32 *result_pattern, SceUInt64 *user_data)
{
    return event_poll(event_id, bit_pattern, result_pattern, user_data);
}

EXPORT(SceThreadmgr, 0x21C7913E, int, _sceKernelPollEvent, SceUID event_id, SceUInt32 bit_pattern,
       SceUInt32 *result_pattern, SceUInt64 *user_data)
{
    return event_poll(event_id, bit_pattern, result_pattern, user_data);
}

EXPORT(SceLibKernel, 0xE5A2AC66, int, sceKernelGetEventPattern, SceUID event_id,
       SceUInt32 *pattern)
{
    VitaEvent *event = event_lookup(event_id);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!pattern)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&event->mutex);
    *pattern = event->pattern;
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x70358258, int, _sceKernelGetEventPattern, SceUID event_id,
       SceUInt32 *pattern)
{
    return sceKernelGetEventPattern(event_id, pattern);
}

EXPORT(SceLibKernel, 0xE35F19D6, int, sceKernelGetEventInfo, SceUID event_id,
       SceKernelEventInfo *info)
{
    VitaEvent *event = event_lookup(event_id);

    if (!event)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&event->mutex);
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->eventId = event->uid;
    memcpy(info->name, event->name, sizeof(info->name));
    info->attr = event->attr;
    info->eventPattern = event->pattern;
    info->userData = event->user_data;
    info->numWaitThreads = event->waiters;
    pthread_mutex_unlock(&event->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xB395BBF1, int, _sceKernelGetEventInfo, SceUID event_id,
       SceKernelEventInfo *info)
{
    return sceKernelGetEventInfo(event_id, info);
}

EXPORT(SceLibKernel, 0x5FF3EE92, int, sceKernelSetEventWithNotifyCallback, SceUID event_id,
       SceUInt32 set_pattern, SceUInt64 user_data, void *callback)
{
    (void)callback;
    return event_set(event_id, set_pattern, user_data);
}

EXPORT(SceThreadmgr, 0x118F646E, int, _sceKernelSetEventWithNotifyCallback, SceUID event_id,
       SceUInt32 set_pattern, SceUInt64 user_data, void *callback)
{
    (void)callback;
    return event_set(event_id, set_pattern, user_data);
}

EXPORT(SceLibKernel, 0x92F162F3, int, sceKernelPulseEventWithNotifyCallback, SceUID event_id,
       SceUInt32 set_pattern, SceUInt64 user_data, void *callback)
{
    (void)callback;
    return event_pulse(event_id, set_pattern, user_data);
}

EXPORT(SceThreadmgr, 0x3E49D3F1, int, _sceKernelPulseEventWithNotifyCallback, SceUID event_id,
       SceUInt32 set_pattern, SceUInt64 user_data, void *callback)
{
    (void)callback;
    return event_pulse(event_id, set_pattern, user_data);
}
