/*
 * SceKernel timers.
 *
 * Timers keep a real microsecond clock: start/stop freeze and resume elapsed
 * time, set-time rewinds it, and the event fields drive
 * sceKernelGetTimerEventRemainingTime. There is no host-side event thread, so
 * a title polling timer time sees the same values it would on hardware.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/error.h>
#include <psp2/types.h>

#include "host.h"
#include "module.h"
#include "modules/SceSysmem.h"

#define VITA_TIMER_SLOTS 64

typedef struct {
    SceSize size;
} SceKernelTimerOptParam;

typedef struct {
    SceSize size;
    SceUID timerId;
    char name[32];
    SceUInt32 attr;
    SceInt32 fActive;
    uint64_t baseTime;
    uint64_t currentTime;
    uint64_t schedule;
    uint64_t interval;
    SceInt32 type;
    SceInt32 fRepeat;
    SceUInt32 numWaitThreads;
    SceInt32 reserved;
} SceKernelTimerInfo;

typedef struct {
    bool used;
    SceUID uid;
    int refs;
    uint32_t attr;
    char name[32];

    bool active;
    bool event_set;
    int32_t type;
    bool repeat;

    int64_t base_us;
    int64_t started_us;
    uint64_t interval_us;
} VitaTimer;

static VitaTimer g_timers[VITA_TIMER_SLOTS];
static pthread_mutex_t g_timer_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_us(void)
{
    return (int64_t)(host_monotonic_ns() / 1000ull);
}

static VitaTimer *vita_timer_lookup(SceUID uid)
{
    pthread_mutex_lock(&g_timer_lock);
    for (int i = 0; i < VITA_TIMER_SLOTS; i++) {
        if (g_timers[i].used && g_timers[i].uid == uid) {
            pthread_mutex_unlock(&g_timer_lock);
            return &g_timers[i];
        }
    }
    pthread_mutex_unlock(&g_timer_lock);

    return NULL;
}

static uint64_t vita_timer_elapsed(const VitaTimer *timer)
{
    int64_t elapsed = timer->base_us;

    if (timer->active)
        elapsed += now_us() - timer->started_us;
    if (elapsed < 0)
        elapsed = 0;

    return (uint64_t)elapsed;
}

static SceUID vita_timer_create(const char *name, uint32_t attr)
{
    SceUID uid = 0;

    pthread_mutex_lock(&g_timer_lock);
    for (int i = 0; i < VITA_TIMER_SLOTS; i++) {
        VitaTimer *timer = &g_timers[i];

        if (timer->used)
            continue;

        memset(timer, 0, sizeof(*timer));
        timer->used = true;
        timer->uid = SceSysmem_get_next_uid();
        timer->refs = 1;
        timer->attr = attr;
        if (name)
            strncpy(timer->name, name, sizeof(timer->name) - 1);
        uid = timer->uid;
        break;
    }
    pthread_mutex_unlock(&g_timer_lock);

    return uid;
}

static int vita_timer_start(SceUID uid)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    if (!timer->active) {
        timer->started_us = now_us();
        timer->active = true;
    }
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

static int vita_timer_stop(SceUID uid)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    if (timer->active) {
        timer->base_us += now_us() - timer->started_us;
        timer->active = false;
    }
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

static int vita_timer_get_time(SceUID uid, uint64_t *out)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!out)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    *out = vita_timer_elapsed(timer);
    return 0;
}

static int vita_timer_get_base(SceUID uid, uint64_t *out)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!out)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    *out = (uint64_t)timer->base_us;
    return 0;
}

static int vita_timer_set_time(SceUID uid, uint64_t value)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    timer->base_us = (int64_t)value;
    if (timer->active)
        timer->started_us = now_us();
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

static int vita_timer_set_event(SceUID uid, int32_t type, const uint64_t *interval, bool repeat)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    timer->event_set = interval != NULL;
    timer->type = type;
    timer->repeat = repeat;
    timer->interval_us = interval ? *interval : 0;
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

static int vita_timer_get_remaining(SceUID uid, uint64_t *out)
{
    VitaTimer *timer = vita_timer_lookup(uid);
    uint64_t elapsed;
    uint64_t remaining = 0;

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!out)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    if (timer->event_set && timer->interval_us) {
        elapsed = vita_timer_elapsed(timer);
        remaining = timer->interval_us - (elapsed % timer->interval_us);
        if (remaining == timer->interval_us)
            remaining = 0;
    }

    *out = remaining;
    return 0;
}

static int vita_timer_cancel(SceUID uid, uint32_t *out)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    timer->active = false;
    timer->event_set = false;
    timer->base_us = 0;
    timer->started_us = 0;
    timer->interval_us = 0;
    pthread_mutex_unlock(&g_timer_lock);

    if (out)
        *out = 0;

    return 0;
}

static int vita_timer_get_info(SceUID uid, SceKernelTimerInfo *info)
{
    VitaTimer *timer = vita_timer_lookup(uid);
    uint64_t elapsed;

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    elapsed = vita_timer_elapsed(timer);

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->timerId = timer->uid;
    memcpy(info->name, timer->name, sizeof(info->name));
    info->attr = timer->attr;
    info->fActive = timer->active ? 1 : 0;
    info->baseTime = (uint64_t)timer->base_us;
    info->currentTime = elapsed;
    info->interval = timer->event_set ? timer->interval_us : 0;
    info->type = timer->type;
    info->fRepeat = timer->repeat ? 1 : 0;
    if (timer->event_set && timer->interval_us) {
        uint64_t remaining = timer->interval_us - (elapsed % timer->interval_us);
        info->schedule = elapsed + (remaining == timer->interval_us ? 0 : remaining);
    }

    return 0;
}

static int vita_timer_delete(SceUID uid)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    memset(timer, 0, sizeof(*timer));
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

static int vita_timer_open(const char *name)
{
    int uid = SCE_KERNEL_ERROR_UNKNOWN_UID;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&g_timer_lock);
    for (int i = 0; i < VITA_TIMER_SLOTS; i++) {
        if (g_timers[i].used && strncmp(g_timers[i].name, name, sizeof(g_timers[i].name)) == 0) {
            g_timers[i].refs++;
            uid = (int)g_timers[i].uid;
            break;
        }
    }
    pthread_mutex_unlock(&g_timer_lock);

    return uid;
}

static int vita_timer_close(SceUID uid)
{
    VitaTimer *timer = vita_timer_lookup(uid);

    if (!timer)
        return SCE_KERNEL_ERROR_UNKNOWN_UID;

    pthread_mutex_lock(&g_timer_lock);
    if (timer->refs > 0)
        timer->refs--;
    pthread_mutex_unlock(&g_timer_lock);

    return 0;
}

EXPORT(SceLibKernel, 0x2255B2A5, SceUID, sceKernelCreateTimer, const char *name, int attr,
       const SceKernelTimerOptParam *opt_param)
{
    (void)opt_param;
    return vita_timer_create(name, (uint32_t)attr);
}

EXPORT(SceLibKernel, 0x9CCF768C, int, sceKernelCancelTimer, SceUID timer_id, SceUInt32 *value)
{
    return vita_timer_cancel(timer_id, value);
}

EXPORT(SceLibKernel, 0x1F59E04D, int, sceKernelGetTimerBase, SceUID timer_id, uint64_t *value)
{
    return vita_timer_get_base(timer_id, value);
}

EXPORT(SceLibKernel, 0x75B1EB3E, int, sceKernelGetTimerEventRemainingTime, SceUID timer_id,
       uint64_t *value)
{
    return vita_timer_get_remaining(timer_id, value);
}

EXPORT(SceLibKernel, 0x7E35E10A, int, sceKernelGetTimerInfo, SceUID timer_id,
       SceKernelTimerInfo *info)
{
    return vita_timer_get_info(timer_id, info);
}

EXPORT(SceLibKernel, 0x381DC300, int, sceKernelGetTimerTime, SceUID timer_id, uint64_t *value)
{
    return vita_timer_get_time(timer_id, value);
}

EXPORT(SceLibKernel, 0x621D293B, int, sceKernelSetTimerEvent, SceUID timer_id, int type,
       uint64_t *value, SceBool repeat)
{
    return vita_timer_set_event(timer_id, type, value, repeat != 0);
}

EXPORT(SceLibKernel, 0xFFAD717F, int, sceKernelSetTimerTime, SceUID timer_id, uint64_t *value)
{
    if (!value)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    return vita_timer_set_time(timer_id, *value);
}

EXPORT(SceThreadmgr, 0xAB1E42C4, int, sceKernelDeleteTimer, SceUID timer_id)
{
    return vita_timer_delete(timer_id);
}

EXPORT(SceThreadmgr, 0xB6E286E7, SceUID, sceKernelOpenTimer, const char *name)
{
    return vita_timer_open(name);
}

EXPORT(SceThreadmgr, 0xACE60E4A, int, sceKernelCloseTimer, SceUID timer_id)
{
    return vita_timer_close(timer_id);
}

EXPORT(SceThreadmgr, 0x48091E0C, int, sceKernelStartTimer, SceUID timer_id)
{
    return vita_timer_start(timer_id);
}

EXPORT(SceThreadmgr, 0x869E9F20, int, sceKernelStopTimer, SceUID timer_id)
{
    return vita_timer_stop(timer_id);
}

EXPORT(SceThreadmgr, 0x5DBC1960, SceUInt64, sceKernelGetTimerBaseWide, SceUID timer_id)
{
    uint64_t value = 0;

    vita_timer_get_base(timer_id, &value);
    return value;
}

EXPORT(SceThreadmgr, 0x3EFD3165, SceUInt64, sceKernelGetTimerTimeWide, SceUID timer_id)
{
    uint64_t value = 0;

    vita_timer_get_time(timer_id, &value);
    return value;
}

EXPORT(SceThreadmgr, 0x273B4A4D, int, sceKernelSetTimerTimeWide, SceUID timer_id, SceUInt64 value)
{
    return vita_timer_set_time(timer_id, value);
}

EXPORT(SceThreadmgr, 0x79BA0A6D, SceUID, _sceKernelCreateTimer, const char *name, int attr,
       const SceKernelTimerOptParam *opt_param)
{
    (void)opt_param;
    return vita_timer_create(name, (uint32_t)attr);
}

EXPORT(SceThreadmgr, 0x921A043D, int, _sceKernelDeleteTimer, SceUID timer_id)
{
    return vita_timer_delete(timer_id);
}

EXPORT(SceThreadmgr, 0x8564E008, SceUID, _sceKernelOpenTimer, const char *name)
{
    return vita_timer_open(name);
}

EXPORT(SceThreadmgr, 0xC3ED6E4A, int, _sceKernelStartTimer, SceUID timer_id)
{
    return vita_timer_start(timer_id);
}

EXPORT(SceThreadmgr, 0xC96F4269, int, _sceKernelStopTimer, SceUID timer_id)
{
    return vita_timer_stop(timer_id);
}

EXPORT(SceThreadmgr, 0x6F2C41BA, int, _sceKernelGetTimerTime, SceUID timer_id, uint64_t *value)
{
    return vita_timer_get_time(timer_id, value);
}

EXPORT(SceThreadmgr, 0x865DA482, int, _sceKernelGetTimerBase, SceUID timer_id, uint64_t *value)
{
    return vita_timer_get_base(timer_id, value);
}

EXPORT(SceThreadmgr, 0xFF738CD9, int, _sceKernelSetTimerTime, SceUID timer_id, uint64_t *value)
{
    if (!value)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    return vita_timer_set_time(timer_id, *value);
}

EXPORT(SceThreadmgr, 0xE2C0BFEF, int, _sceKernelSetTimerEvent, SceUID timer_id, int type,
       uint64_t *value, SceBool repeat)
{
    return vita_timer_set_event(timer_id, type, value, repeat != 0);
}

EXPORT(SceThreadmgr, 0xAC7FE4F3, int, _sceKernelGetTimerInfo, SceUID timer_id,
       SceKernelTimerInfo *info)
{
    return vita_timer_get_info(timer_id, info);
}

EXPORT(SceThreadmgr, 0x13117B21, int, _sceKernelCancelTimer, SceUID timer_id, SceUInt32 *value)
{
    return vita_timer_cancel(timer_id, value);
}

EXPORT(SceThreadmgr, 0x9AC39954, SceUInt64, _sceKernelGetTimerTimeWide, SceUID timer_id)
{
    uint64_t value = 0;

    vita_timer_get_time(timer_id, &value);
    return value;
}

EXPORT(SceThreadmgr, 0x370E147B, SceUInt64, _sceKernelGetTimerBaseWide, SceUID timer_id)
{
    uint64_t value = 0;

    vita_timer_get_base(timer_id, &value);
    return value;
}

EXPORT(SceThreadmgr, 0x39364623, int, _sceKernelSetTimerTimeWide, SceUID timer_id, SceUInt64 value)
{
    return vita_timer_set_time(timer_id, value);
}

EXPORT(SceThreadmgr, 0x215FD24D, int, _sceKernelGetTimerEventRemainingTime, SceUID timer_id,
       uint64_t *value)
{
    return vita_timer_get_remaining(timer_id, value);
}
