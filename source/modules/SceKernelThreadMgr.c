#include <m-dict.h>
#include <errno.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "util.h"

#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceSysmem.h"

#define SCE_KERNEL_HIGHEST_PRIORITY_USER               64
#define SCE_KERNEL_LOWEST_PRIORITY_USER                191
#define SCE_KERNEL_DEFAULT_PRIORITY                    ((SceInt32)0x10000100)
#define SCE_KERNEL_DEFAULT_PRIORITY_GAME_APP           160
#define SCE_KERNEL_DEFAULT_PRIORITY_USER               SCE_KERNEL_DEFAULT_PRIORITY
#define SCE_KERNEL_THREAD_STACK_SIZE_DEFAULT_USER_MAIN (256 * 1024)
#define SCE_KERNEL_TLS_SIZE                            0x800

#define HOST_HIGHEST_PRIORITY 28
#define HOST_LOWEST_PRIORITY  59

#define SCE_KERNEL_THREAD_EVENT_TYPE_START 0x04
#define SCE_KERNEL_THREAD_EVENT_TYPE_END   0x08

static s32 g_vita_thread_info_tls_slot_id;
static SceUID g_main_thread_uid;

DICT_DEF2(vita_thread_info_dict, SceUID, M_DEFAULT_OPLIST, VitaThreadInfo *, M_POD_OPLIST)
static vita_thread_info_dict_t g_vita_thread_infos;
static RwLock g_vita_thread_infos_lock;

typedef struct {
    SceUID uid;
    char name[32];
    int init_count;
    int current_count;
    int waiting;
    SceUID owner;
    pthread_mutex_t mutex;
} VitaMutexInfo;

typedef struct {
    SceUID uid;
    char name[32];
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t used;
    uint32_t head;
    uint32_t tail;
    uint32_t send_waiters;
    uint32_t receive_waiters;
    int refs;
    Mutex lock;
    CondVar not_empty;
    CondVar not_full;
} VitaMsgPipeInfo;

#define VITA_CALLBACK_SLOTS 32

struct SceKernelThreadEventHandlerOpt {
    void *handler;
    void *common;
    int reserved0;
    int reserved1;
};

typedef int (*VitaThreadEventHandler)(SceUInt32 event_type, SceUID thread_id, SceUInt32 reserved,
                                      void *common);

typedef struct {
    bool used;
    SceUID uid;
    SceUID thread_id;
    uint32_t attr;
    char name[32];
    SceKernelCallbackFunction callback;
    void *user_data;
    SceUID event_id;
    SceUID notifier_id;
    int notify_count;
    int notify_arg;
} VitaCallbackInfo;

typedef struct {
    bool used;
    SceUID uid;
    SceUID thread_mask;
    uint32_t event_mask;
    char name[32];
    VitaThreadEventHandler handler;
    void *common;
} VitaThreadEventHandlerInfo;

DICT_DEF2(vita_mutex_info_dict, SceUID, M_DEFAULT_OPLIST, VitaMutexInfo *, M_POD_OPLIST)
DICT_DEF2(vita_msgpipe_info_dict, SceUID, M_DEFAULT_OPLIST, VitaMsgPipeInfo *, M_POD_OPLIST)
static vita_mutex_info_dict_t g_vita_mutex_infos;
static vita_msgpipe_info_dict_t g_vita_msgpipe_infos;
static RwLock g_vita_sync_objects_lock;

static VitaCallbackInfo g_vita_callbacks[VITA_CALLBACK_SLOTS];
static pthread_mutex_t g_vita_callbacks_lock = PTHREAD_MUTEX_INITIALIZER;

static VitaThreadEventHandlerInfo g_vita_thread_event_handlers[VITA_CALLBACK_SLOTS];
static pthread_mutex_t g_vita_thread_event_handlers_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int vita_priority_to_host_priority(int priority)
{
    if ((priority & SCE_KERNEL_DEFAULT_PRIORITY) == SCE_KERNEL_DEFAULT_PRIORITY)
        priority = SCE_KERNEL_DEFAULT_PRIORITY_GAME_APP + (priority & ~SCE_KERNEL_DEFAULT_PRIORITY);

    if ((priority < SCE_KERNEL_HIGHEST_PRIORITY_USER) ||
        (priority > SCE_KERNEL_LOWEST_PRIORITY_USER))
        return SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;

    return HOST_HIGHEST_PRIORITY +
           ((priority - SCE_KERNEL_HIGHEST_PRIORITY_USER) *
            (HOST_LOWEST_PRIORITY - HOST_HIGHEST_PRIORITY)) /
               (SCE_KERNEL_LOWEST_PRIORITY_USER - SCE_KERNEL_HIGHEST_PRIORITY_USER);
}

static VitaThreadInfo *get_thread_info_for_uid(SceUID thid)
{
    VitaThreadInfo *ti;

    VitaThreadInfo **slot;

    rwlockReadLock(&g_vita_thread_infos_lock);
    slot = vita_thread_info_dict_get(g_vita_thread_infos, thid);
    ti = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_thread_infos_lock);

    return ti;
}

static VitaThreadInfo *get_thread_info_or_current(SceUID thid)
{
    if (thid == 0)
        return SceKernelThreadMgr_get_thread_info();

    return get_thread_info_for_uid(thid);
}

static SceUID current_thread_uid(void)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    return ti ? ti->uid : 0;
}

static void thread_event_notify(SceUInt32 event_type, SceUID thread_id)
{
    VitaThreadEventHandlerInfo handlers[VITA_CALLBACK_SLOTS];
    int count = 0;

    pthread_mutex_lock(&g_vita_thread_event_handlers_lock);
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        VitaThreadEventHandlerInfo *info = &g_vita_thread_event_handlers[i];

        if (!info->used || !(info->event_mask & event_type))
            continue;
        if (info->thread_mask != 0 && info->thread_mask != thread_id)
            continue;

        handlers[count++] = *info;
    }
    pthread_mutex_unlock(&g_vita_thread_event_handlers_lock);

    for (int i = 0; i < count; i++) {
        if (handlers[i].handler)
            handlers[i].handler(event_type, thread_id, 0, handlers[i].common);
    }
}

static int run_pending_callbacks(SceUID thread_id)
{
    int processed = 0;

    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        VitaCallbackInfo cb;
        bool run = false;

        pthread_mutex_lock(&g_vita_callbacks_lock);
        if (g_vita_callbacks[i].used && g_vita_callbacks[i].thread_id == thread_id &&
            g_vita_callbacks[i].notify_count > 0) {
            cb = g_vita_callbacks[i];
            g_vita_callbacks[i].notify_count = 0;
            g_vita_callbacks[i].notifier_id = 0;
            run = true;
        }
        pthread_mutex_unlock(&g_vita_callbacks_lock);

        if (!run)
            continue;
        processed++;
        if (cb.callback)
            cb.callback((int)cb.notifier_id, cb.notify_count, cb.notify_arg, cb.user_data);
    }

    return processed;
}

static void NORETURN thread_entry_wrapper(void *arg)
{
    VitaThreadInfo *ti = arg;
    int ret;

    threadTlsSet(g_vita_thread_info_tls_slot_id, ti);

    thread_event_notify(SCE_KERNEL_THREAD_EVENT_TYPE_START, ti->uid);

    ret = ti->entry(ti->arglen, ti->argp);
    ti->return_status = ret;

    thread_event_notify(SCE_KERNEL_THREAD_EVENT_TYPE_END, ti->uid);

    LOG("Thread 0x%x returned with: 0x%x", ti->uid, ret);

    /*
     * Returning from the Vita main thread is a normal way for simple
     * homebrew to finish. Treat it like sceKernelExitProcess() so the host
     * main loop does not wait forever.
     */
    if (g_main_thread_uid != 0 && ti->uid == g_main_thread_uid)
        ueventSignal(SceLibKernel_get_process_exit_uevent());

    threadExit();
}

static SceUID create_thread(const char *name, SceKernelThreadEntry entry, int initPriority,
                            SceSize stackSize, SceUInt attr)
{
    VitaThreadInfo *ti;
    Result res;
    int priority = vita_priority_to_host_priority(initPriority);

    if (priority < 0)
        return priority;

    ti = malloc(sizeof(*ti));
    if (!ti) {
        LOG("Could not allocate thread info for thread \"%s\"", name);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    memset(ti, 0, sizeof(*ti));
    ti->uid = SceSysmem_get_next_uid();
    strncpy(ti->name, name, sizeof(ti->name) - 1);
    ti->entry = entry;
    ti->init_priority = initPriority;
    ti->current_priority = initPriority;
    ti->stack_size = stackSize;
    ti->attr = attr;
    ti->vita_tls = malloc(SCE_KERNEL_TLS_SIZE);

    res = threadCreate(&ti->thread, thread_entry_wrapper, ti, NULL, stackSize, priority, -2);
    if (R_FAILED(res)) {
        LOG("Error creating thread: 0x%" PRIx32, res);
        free(ti->vita_tls);
        free(ti);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    LOG("Created thread \"%s\", thid: 0x%x", name, ti->uid);

    rwlockWriteLock(&g_vita_thread_infos_lock);
    vita_thread_info_dict_set_at(g_vita_thread_infos, ti->uid, ti);
    rwlockWriteUnlock(&g_vita_thread_infos_lock);

    return ti->uid;
}

static int start_thread(SceUID thid, SceSize arglen, void *argp)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    if (arglen && argp) {
        ti->argp = malloc(arglen);
        if (!ti->argp)
            return SCE_KERNEL_ERROR_NO_MEMORY;

        memcpy(ti->argp, argp, arglen);
        ti->arglen = arglen;
    }

    res = threadStart(&ti->thread);
    if (R_FAILED(res)) {
        LOG("Error starting thread 0x%x: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    return 0;
}

EXPORT(SceLibKernel, 0xC5C11EE7, SceUID, sceKernelCreateThread, const char *name,
       SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
       int cpuAffinityMask, const SceKernelThreadOptParam *option)
{
    return create_thread(name, entry, initPriority, stackSize, attr);
}

EXPORT(SceThreadmgr, 0x1BBDE3D9, int, sceKernelDeleteThread, SceUID thid)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    res = threadClose(&ti->thread);
    if (R_FAILED(res)) {
        LOG("Error closing thread 0x%x exit: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    free(ti->argp);
    free(ti->vita_tls);
    rwlockWriteLock(&g_vita_thread_infos_lock);
    vita_thread_info_dict_erase(g_vita_thread_infos, thid);
    rwlockWriteUnlock(&g_vita_thread_infos_lock);
    free(ti);
    LOG("Deleted thread 0x%x", thid);

    return 0;
}

EXPORT(SceLibKernel, 0xF08DE149, int, sceKernelStartThread, SceUID thid, SceSize arglen, void *argp)
{
    return start_thread(thid, arglen, argp);
}

EXPORT(SceThreadmgr, 0xC30B1745, int, _sceKernelStartThread, SceUID thid, SceSize arglen, void *argp)
{
    return sceKernelStartThread(thid, arglen, argp);
}

EXPORT(SceThreadmgrCoredumpTime, 0x0C8A38E1, int NORETURN, sceKernelExitThread, int status)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    ti->return_status = status;
    threadExit();
}

EXPORT(SceThreadmgr, 0x1D17DECF, int NORETURN, sceKernelExitDeleteThread, int status)
{
    sceKernelExitThread(status);
    // TODO: Delete
}

int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);

EXPORT(SceThreadmgr, 0xEA5C52F5, int, _sceKernelWaitThreadEnd, SceUID thid, int *stat,
       SceUInt *timeout)
{
    return sceKernelWaitThreadEnd(thid, stat, timeout);
}

EXPORT(SceLibKernel, 0xDDB395A9, int, sceKernelWaitThreadEnd, SceUID thid, int *stat,
       SceUInt *timeout)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    uint64_t ns;
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    if (!timeout)
        ns = UINT64_MAX;
    else
        ns = *timeout * 1000ull;

    res = waitSingle(waiterForThread(&ti->thread), ns);
    if (R_FAILED(res)) {
        LOG("Error waiting for thread 0x%x exit: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    if (stat)
        *stat = ti->return_status;

    return 0;
}

int SceKernelThreadMgr_main_entry(SceKernelThreadEntry entry, int args, void *argp)
{
    SceUID thid;
    UEvent *process_exit_event;
    int ret;
    Result res;

    thid = create_thread("<main>", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER,
                         SCE_KERNEL_THREAD_STACK_SIZE_DEFAULT_USER_MAIN, 0);
    if (thid < 0)
        return thid;

    g_main_thread_uid = thid;

    ret = start_thread(thid, args, argp);
    if (ret < 0) {
        sceKernelDeleteThread(thid);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    process_exit_event = SceLibKernel_get_process_exit_uevent();

    res = waitSingle(waiterForUEvent(process_exit_event), -1);
    if (R_FAILED(res)) {
        LOG("Error waiting for the process to finish: 0x%" PRIx32, res);
        return -1;
    }

    // TODO: Also wait for all threads to finish?
    sceKernelWaitThreadEnd(thid, NULL, NULL);
    if (thid == g_main_thread_uid)
        g_main_thread_uid = 0;
    sceKernelDeleteThread(thid);

    return 0;
}

EXPORT(SceThreadmgr, 0x4B675D05, int, sceKernelDelayThread, SceUInt delay)
{
    host_sleep_ns((s64)delay * 1000);
    return 0;
}

EXPORT(SceThreadmgr, 0x9C0180E1, int, sceKernelDelayThreadCB, SceUInt delay)
{
    return sceKernelDelayThread(delay);
}

EXPORT(SceThreadmgr, 0x97C4A7C4, int, sceKernelDelayThread200, SceUInt delay)
{
    return sceKernelDelayThread(delay);
}

EXPORT(SceThreadmgr, 0x34938242, int, sceKernelDelayThreadCB200, SceUInt delay)
{
    return sceKernelDelayThread(delay);
}

EXPORT(SceThreadmgr, 0xBACA6891, void *, sceKernelGetThreadTLSAddr, SceUID thid, int key)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);

    if (!ti)
        return NULL;

    if (key >= 0 && key <= 0x100)
        return &ti->vita_tls[key];

    return NULL;
}

/* Thread callbacks. */

static VitaCallbackInfo *callback_lookup(SceUID uid)
{
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        if (g_vita_callbacks[i].used && g_vita_callbacks[i].uid == uid)
            return &g_vita_callbacks[i];
    }

    return NULL;
}

EXPORT(SceThreadmgr, 0xB19CF7E9, SceUID, sceKernelCreateCallback, const char *name,
       SceUInt32 attr, SceKernelCallbackFunction callback, void *user_data)
{
    SceUID uid = 0;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        VitaCallbackInfo *info = &g_vita_callbacks[i];

        if (info->used)
            continue;

        memset(info, 0, sizeof(*info));
        info->used = true;
        info->uid = SceSysmem_get_next_uid();
        info->thread_id = current_thread_uid();
        info->attr = attr;
        info->callback = callback;
        info->user_data = user_data;
        if (name)
            strncpy(info->name, name, sizeof(info->name) - 1);
        uid = info->uid;
        break;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return uid ? uid : SCE_KERNEL_ERROR_NO_MEMORY;
}

EXPORT(SceThreadmgr, 0xD469676B, int, sceKernelDeleteCallback, SceUID callback_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        memset(info, 0, sizeof(*info));
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceThreadmgr, 0xA4683592, int, sceKernelNotifyCallback, SceUID callback_id,
       int notify_arg)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        if (info->notify_count < 0x7FFFFFFF)
            info->notify_count++;
        info->notify_arg = notify_arg;
        info->notifier_id = current_thread_uid();
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceThreadmgr, 0x30741EF2, int, sceKernelCancelCallback, SceUID callback_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        info->notify_count = 0;
        info->notify_arg = 0;
        info->notifier_id = 0;
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceThreadmgr, 0x038644D5, int, sceKernelGetCallbackCount, SceUID callback_id)
{
    int count = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info)
        count = info->notify_count;
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return count;
}

static int callback_get_info(SceUID callback_id, SceKernelCallbackInfo *info)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *cb = callback_lookup(callback_id);

    if (cb) {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        info->callbackId = cb->uid;
        memcpy(info->name, cb->name, sizeof(info->name));
        info->threadId = cb->thread_id;
        info->callback = cb->callback;
        info->common = cb->user_data;
        info->notifyCount = cb->notify_count;
        info->notifyArg = cb->notify_arg;
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceLibKernel, 0x88DD1BC8, int, sceKernelGetCallbackInfo, SceUID callback_id,
       SceKernelCallbackInfo *info)
{
    return callback_get_info(callback_id, info);
}

EXPORT(SceThreadmgr, 0x86761234, int, _sceKernelGetCallbackInfo, SceUID callback_id,
       SceKernelCallbackInfo *info)
{
    return callback_get_info(callback_id, info);
}

EXPORT(SceThreadmgr, 0xE53E41F6, int, sceKernelCheckCallback, void)
{
    return run_pending_callbacks(current_thread_uid());
}

EXPORT(SceThreadmgr, 0x2682E6ED, int, _sceKernelExitCallback, void)
{
    return 0;
}

EXPORT(SceThreadmgr, 0x76FB37E9, int, sceKernelRegisterCallbackToEvent, SceUID callback_id,
       SceUID event_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        info->event_id = event_id;
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceThreadmgr, 0x18462B11, int, sceKernelUnregisterCallbackFromEvent, SceUID callback_id,
       SceUID event_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        if (info->event_id == event_id)
            info->event_id = 0;
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

EXPORT(SceThreadmgr, 0x888A7361, int, sceKernelUnregisterCallbackFromEventAll, SceUID callback_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID;

    pthread_mutex_lock(&g_vita_callbacks_lock);
    VitaCallbackInfo *info = callback_lookup(callback_id);

    if (info) {
        info->event_id = 0;
        ret = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);

    return ret;
}

void SceKernelThreadMgr_notify_event_callbacks(SceUID event_id, int notify_arg)
{
    pthread_mutex_lock(&g_vita_callbacks_lock);
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        VitaCallbackInfo *info = &g_vita_callbacks[i];

        if (!info->used || info->event_id != event_id)
            continue;
        if (info->notify_count < 0x7FFFFFFF)
            info->notify_count++;
        info->notify_arg = notify_arg;
        info->notifier_id = 0;
    }
    pthread_mutex_unlock(&g_vita_callbacks_lock);
}

/* Thread event handlers. */

static int register_thread_event_handler(const char *name, SceUID thread_mask, SceUInt32 event_mask,
                                         VitaThreadEventHandler handler, void *common)
{
    SceUID uid = 0;

    if (!handler || !(event_mask & (SCE_KERNEL_THREAD_EVENT_TYPE_START |
                                    SCE_KERNEL_THREAD_EVENT_TYPE_END)))
        return SCE_KERNEL_ERROR_THREAD_EVENT_ERROR;

    pthread_mutex_lock(&g_vita_thread_event_handlers_lock);
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        VitaThreadEventHandlerInfo *info = &g_vita_thread_event_handlers[i];

        if (info->used)
            continue;

        memset(info, 0, sizeof(*info));
        info->used = true;
        info->uid = SceSysmem_get_next_uid();
        info->thread_mask = thread_mask;
        info->event_mask = event_mask;
        info->handler = handler;
        info->common = common;
        if (name)
            strncpy(info->name, name, sizeof(info->name) - 1);
        uid = info->uid;
        break;
    }
    pthread_mutex_unlock(&g_vita_thread_event_handlers_lock);

    return uid ? (int)uid : SCE_KERNEL_ERROR_NO_MEMORY;
}

EXPORT(SceLibKernel, 0x6D8C0F13, int, sceKernelRegisterThreadEventHandler, const char *name,
       SceUID thread_mask, SceUInt32 event_mask, VitaThreadEventHandler handler, void *common)
{
    return register_thread_event_handler(name, thread_mask, event_mask, handler, common);
}

EXPORT(SceThreadmgr, 0xCE6B49D8, int, _sceKernelRegisterThreadEventHandler, const char *name,
       SceUID thread_mask, SceUInt32 event_mask,
       const struct SceKernelThreadEventHandlerOpt *opt)
{
    if (!opt)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    return register_thread_event_handler(name, thread_mask, event_mask, opt->handler, opt->common);
}

EXPORT(SceThreadmgr, 0x2C8ED6F0, int, sceKernelUnregisterThreadEventHandler, SceUID handler_id)
{
    int ret = SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID;

    pthread_mutex_lock(&g_vita_thread_event_handlers_lock);
    for (int i = 0; i < VITA_CALLBACK_SLOTS; i++) {
        if (g_vita_thread_event_handlers[i].used && g_vita_thread_event_handlers[i].uid == handler_id) {
            memset(&g_vita_thread_event_handlers[i], 0,
                   sizeof(g_vita_thread_event_handlers[i]));
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_vita_thread_event_handlers_lock);

    return ret;
}

/* Serialization objects. */

static VitaMutexInfo *get_mutex_info(SceUID uid)
{
    VitaMutexInfo *info;

    VitaMutexInfo **slot;

    rwlockReadLock(&g_vita_sync_objects_lock);
    slot = vita_mutex_info_dict_get(g_vita_mutex_infos, uid);
    info = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return info;
}

static VitaMsgPipeInfo *get_msgpipe_info(SceUID uid)
{
    VitaMsgPipeInfo *info;

    VitaMsgPipeInfo **slot;

    rwlockReadLock(&g_vita_sync_objects_lock);
    slot = vita_msgpipe_info_dict_get(g_vita_msgpipe_infos, uid);
    info = slot ? *slot : NULL;
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return info;
}

EXPORT(SceLibKernel, 0xED53334A, SceUID, sceKernelCreateMutex, const char *name, SceUInt attr,
       int initCount, SceKernelMutexOptParam *option)
{
    pthread_mutexattr_t mutex_attr;
    VitaMutexInfo *info = calloc(1, sizeof(*info));

    (void)attr;
    (void)option;

    if (!info)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    info->uid = SceSysmem_get_next_uid();
    info->init_count = initCount;
    info->current_count = initCount;
    if (initCount > 0)
        info->owner = current_thread_uid();
    if (name)
        strncpy(info->name, name, sizeof(info->name) - 1);

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&info->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    for (int i = 0; i < initCount; i++)
        pthread_mutex_lock(&info->mutex);

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_mutex_info_dict_set_at(g_vita_mutex_infos, info->uid, info);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    return info->uid;
}

EXPORT(SceThreadmgr, 0x92667AE5, SceUID, _sceKernelCreateMutex, const char *name, SceUInt attr,
       int init_count, SceKernelMutexOptParam *option)
{
    return sceKernelCreateMutex(name, attr, init_count, option);
}

EXPORT(SceThreadmgr, 0x52E17182, SceUID, sceKernelOpenMutex, const char *name)
{
    VitaMutexInfo *result = NULL;
    vita_mutex_info_dict_it_t it;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_vita_sync_objects_lock);
    for (vita_mutex_info_dict_it(it, g_vita_mutex_infos);
         !vita_mutex_info_dict_end_p(it); vita_mutex_info_dict_next(it)) {
        VitaMutexInfo *info = vita_mutex_info_dict_ref(it)->value;

        if (strncmp(info->name, name, sizeof(info->name)) == 0) {
            result = info;
            break;
        }
    }
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return result ? result->uid : SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
}

EXPORT(SceThreadmgr, 0x2928D2EC, SceUID, sceKernelOpenMutex_089, const char *name)
{
    return sceKernelOpenMutex(name);
}

EXPORT(SceThreadmgr, 0x03E23AF6, int, sceKernelCloseMutex, SceUID mutexid)
{
    return get_mutex_info(mutexid) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
}

EXPORT(SceThreadmgr, 0xA35427EE, int, sceKernelCloseMutex_089, SceUID mutexid)
{
    return sceKernelCloseMutex(mutexid);
}

EXPORT(SceThreadmgr, 0xCB78710D, int, sceKernelDeleteMutex, SceUID mutexid)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_mutex_info_dict_erase(g_vita_mutex_infos, mutexid);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    pthread_mutex_destroy(&info->mutex);
    free(info);
    return 0;
}

EXPORT(SceLibKernel, 0x1D8D7945, int, sceKernelLockMutex, SceUID mutexid, int lockCount,
       unsigned int *timeout)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (lockCount < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    info->waiting += lockCount;
    for (int i = 0; i < lockCount; i++)
        pthread_mutex_lock(&info->mutex);
    info->waiting -= lockCount;
    info->current_count += lockCount;
    info->owner = current_thread_uid();

    return 0;
}

int sceKernelLockMutexCB(SceUID mutexid, int lock_count, SceUInt *timeout);

EXPORT(SceThreadmgr, 0x7FA945AD, int, _sceKernelLockMutex, SceUID mutexid, int lock_count,
       SceUInt *timeout)
{
    return sceKernelLockMutex(mutexid, lock_count, timeout);
}

EXPORT(SceThreadmgr, 0xDB9F5333, int, _sceKernelLockMutexCB, SceUID mutexid, int lock_count,
       SceUInt *timeout)
{
    return sceKernelLockMutexCB(mutexid, lock_count, timeout);
}

EXPORT(SceLibKernel, 0x2BDAA524, int, sceKernelLockMutexCB, SceUID mutexid, int lockCount,
       unsigned int *timeout)
{
    return sceKernelLockMutex(mutexid, lockCount, timeout);
}

EXPORT(SceLibKernel, 0x72FC1F54, int, sceKernelTryLockMutex, SceUID mutexid, int lockCount)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    for (int i = 0; i < lockCount; i++) {
        if (pthread_mutex_trylock(&info->mutex) != 0)
            return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
        info->current_count++;
        info->owner = current_thread_uid();
    }

    return 0;
}

EXPORT(SceThreadmgr, 0x1A372EC8, int, sceKernelUnlockMutex, SceUID mutexid, int unlockCount)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (unlockCount < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    for (int i = 0; i < unlockCount; i++)
        pthread_mutex_unlock(&info->mutex);
    if (unlockCount > info->current_count)
        info->current_count = 0;
    else
        info->current_count -= unlockCount;
    if (info->current_count == 0)
        info->owner = 0;

    return 0;
}

EXPORT(SceLibKernel, 0x9A6C43CA, int, sceKernelGetMutexInfo, SceUID mutexid,
       SceKernelMutexInfo *info)
{
    VitaMutexInfo *mutex = get_mutex_info(mutexid);

    if (!mutex)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->mutexId = mutex->uid;
    memcpy(info->name, mutex->name, sizeof(info->name));
    info->attr = 0;
    info->initCount = mutex->init_count;
    info->currentCount = mutex->current_count;
    info->currentOwnerId = mutex->owner;
    info->numWaitThreads = mutex->waiting;

    return 0;
}

EXPORT(SceThreadmgr, 0xE8CC3DF0, int, _sceKernelGetMutexInfo, SceUID mutexid,
       SceKernelMutexInfo *info)
{
    return sceKernelGetMutexInfo(mutexid, info);
}

EXPORT(SceLibKernel, 0x2144890D, int, sceKernelCancelMutex, SceUID mutexid, int new_count,
       int *num_wait_threads)
{
    VitaMutexInfo *mutex = get_mutex_info(mutexid);

    if (!mutex)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (new_count < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    if (num_wait_threads)
        *num_wait_threads = mutex->waiting;
    mutex->current_count = new_count;
    if (new_count == 0)
        mutex->owner = 0;

    return 0;
}

EXPORT(SceThreadmgr, 0x1B74CB89, int, _sceKernelCancelMutex, SceUID mutexid, int new_count,
       int *num_wait_threads)
{
    return sceKernelCancelMutex(mutexid, new_count, num_wait_threads);
}

EXPORT(SceLibKernel, 0x0A10C1C8, SceUID, sceKernelCreateMsgPipe, const char *name, int type,
       int attr, unsigned int bufSize, void *option)
{
    VitaMsgPipeInfo *info = calloc(1, sizeof(*info));

    (void)type;
    (void)attr;
    (void)option;

    if (!info)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    info->uid = SceSysmem_get_next_uid();
    info->refs = 1;
    if (name)
        strncpy(info->name, name, sizeof(info->name) - 1);
    info->buffer = malloc(bufSize ? bufSize : 1);
    info->capacity = bufSize ? bufSize : 1;
    if (!info->buffer) {
        free(info);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    mutexInit(&info->lock);
    condvarInit(&info->not_empty);
    condvarInit(&info->not_full);

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_msgpipe_info_dict_set_at(g_vita_msgpipe_infos, info->uid, info);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    return info->uid;
}

EXPORT(SceThreadmgr, 0xE78BCCF7, int, sceKernelDeleteMsgPipe, SceUID uid)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_msgpipe_info_dict_erase(g_vita_msgpipe_infos, uid);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    mutexDestroy(&info->lock);
    condvarDestroy(&info->not_empty);
    condvarDestroy(&info->not_full);
    free(info->buffer);
    free(info);
    return 0;
}

static int msgpipe_send(VitaMsgPipeInfo *info, const void *message, unsigned int size,
                        unsigned int *result, bool wait)
{
    const uint8_t *src = message;

    if (size > info->capacity)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    mutexLock(&info->lock);
    if (wait) {
        info->send_waiters++;
        while (info->capacity - info->used < size)
            condvarWait(&info->not_full, &info->lock);
        info->send_waiters--;
    } else if (info->capacity - info->used < size) {
        mutexUnlock(&info->lock);
        return SCE_KERNEL_ERROR_MSG_PIPE_FULL;
    }

    for (unsigned int i = 0; i < size; i++) {
        info->buffer[(info->head + i) % info->capacity] = src[i];
    }
    info->head = (info->head + size) % info->capacity;
    info->used += size;

    if (result)
        *result = size;

    condvarWakeAll(&info->not_empty);
    mutexUnlock(&info->lock);
    return 0;
}

static int msgpipe_receive(VitaMsgPipeInfo *info, void *message, unsigned int size,
                           unsigned int *result, bool wait)
{
    uint8_t *dst = message;
    unsigned int count;

    mutexLock(&info->lock);
    if (wait) {
        info->receive_waiters++;
        while (!info->used)
            condvarWait(&info->not_empty, &info->lock);
        info->receive_waiters--;
    } else if (!info->used) {
        mutexUnlock(&info->lock);
        return SCE_KERNEL_ERROR_MSG_PIPE_EMPTY;
    }

    count = info->used < size ? info->used : size;
    for (unsigned int i = 0; i < count; i++)
        dst[i] = info->buffer[(info->tail + i) % info->capacity];

    info->tail = (info->tail + count) % info->capacity;
    info->used -= count;

    if (result)
        *result = count;

    condvarWakeAll(&info->not_full);
    mutexUnlock(&info->lock);
    return 0;
}

typedef struct {
    const void *message;
    SceSize size;
} VitaMsgPipeVector;

static int msgpipe_send_vector(SceUID uid, const VitaMsgPipeVector *vec, unsigned int count,
                               void *result_ptr, bool wait)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);
    unsigned int total = 0;
    int ret = 0;

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;
    if (!vec && count)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    for (unsigned int i = 0; i < count; i++) {
        unsigned int sent = 0;

        if (!vec[i].message && vec[i].size)
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        ret = msgpipe_send(info, vec[i].message, vec[i].size, &sent, wait);
        if (ret < 0)
            break;
        total += sent;
    }

    if (result_ptr)
        *(unsigned int *)result_ptr = total;

    return ret;
}

static int msgpipe_receive_vector(SceUID uid, const VitaMsgPipeVector *vec, unsigned int count,
                                  void *result_ptr, bool wait)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);
    unsigned int total = 0;
    int ret = 0;

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;
    if (!vec && count)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    for (unsigned int i = 0; i < count; i++) {
        unsigned int received = 0;

        if (!vec[i].message && vec[i].size)
            return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
        ret = msgpipe_receive(info, (void *)vec[i].message, vec[i].size, &received, wait);
        if (ret < 0)
            break;
        total += received;
        if (received < vec[i].size)
            break;
    }

    if (result_ptr)
        *(unsigned int *)result_ptr = total;

    return ret;
}

EXPORT(SceLibKernel, 0x0CA71EA2, int, sceKernelSendMsgPipe, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_send(info, message, size, unk2, true);
}

EXPORT(SceLibKernel, 0xA5CA74AC, int, sceKernelSendMsgPipeCB, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    run_pending_callbacks(current_thread_uid());
    return sceKernelSendMsgPipe(uid, message, size, unk1, unk2, timeout);
}

EXPORT(SceLibKernel, 0x94D506F7, int, sceKernelSendMsgPipeVector, SceUID uid,
       const VitaMsgPipeVector *vec, unsigned int count, int unk1, void *unk2,
       unsigned int *timeout)
{
    (void)unk1;
    (void)timeout;
    return msgpipe_send_vector(uid, vec, count, unk2, true);
}

EXPORT(SceLibKernel, 0x9C6F7F79, int, sceKernelSendMsgPipeVectorCB, SceUID uid,
       const VitaMsgPipeVector *vec, unsigned int count, int unk1, void *unk2,
       unsigned int *timeout)
{
    run_pending_callbacks(current_thread_uid());
    return sceKernelSendMsgPipeVector(uid, vec, count, unk1, unk2, timeout);
}

EXPORT(SceLibKernel, 0xDFC670E0, int, sceKernelTrySendMsgPipe, SceUID uid, void *message,
       SceSize size, int unk1, void *unk2)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_send(info, message, size, unk2, false);
}

EXPORT(SceLibKernel, 0x60DB346F, int, sceKernelTrySendMsgPipeVector, SceUID uid,
       const VitaMsgPipeVector *vec, SceSize count, int unk1, void *unk2)
{
    (void)unk1;
    return msgpipe_send_vector(uid, vec, count, unk2, false);
}

EXPORT(SceLibKernel, 0x4E81DD5C, int, sceKernelReceiveMsgPipe, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_receive(info, message, size, unk2, true);
}

EXPORT(SceLibKernel, 0x33AF829B, int, sceKernelReceiveMsgPipeCB, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    run_pending_callbacks(current_thread_uid());
    return sceKernelReceiveMsgPipe(uid, message, size, unk1, unk2, timeout);
}

EXPORT(SceLibKernel, 0x9F899087, int, sceKernelReceiveMsgPipeVector, SceUID uid,
       const VitaMsgPipeVector *vec, unsigned int count, int unk1, void *unk2,
       unsigned int *timeout)
{
    (void)unk1;
    (void)timeout;
    return msgpipe_receive_vector(uid, vec, count, unk2, true);
}

EXPORT(SceLibKernel, 0xBE5B3E27, int, sceKernelReceiveMsgPipeVectorCB, SceUID uid,
       const VitaMsgPipeVector *vec, unsigned int count, int unk1, void *unk2,
       unsigned int *timeout)
{
    run_pending_callbacks(current_thread_uid());
    return sceKernelReceiveMsgPipeVector(uid, vec, count, unk1, unk2, timeout);
}

EXPORT(SceLibKernel, 0x5615B006, int, sceKernelTryReceiveMsgPipe, SceUID uid, void *message,
       SceSize size, int unk1, void *unk2)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_receive(info, message, size, unk2, false);
}

EXPORT(SceLibKernel, 0x86ECC0FF, int, sceKernelTryReceiveMsgPipeVector, SceUID uid,
       const VitaMsgPipeVector *vec, SceSize count, int unk1, void *unk2)
{
    (void)unk1;
    return msgpipe_receive_vector(uid, vec, count, unk2, false);
}

EXPORT(SceThreadmgr, 0xCE769C83, int, _sceKernelCancelMsgPipe, SceUID uid, int *send_waiters,
       int *receive_waiters)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    mutexLock(&info->lock);
    if (send_waiters)
        *send_waiters = (int)info->send_waiters;
    if (receive_waiters)
        *receive_waiters = (int)info->receive_waiters;
    condvarWakeAll(&info->not_empty);
    condvarWakeAll(&info->not_full);
    mutexUnlock(&info->lock);

    return 0;
}

EXPORT(SceLibKernel, 0xEF14BA37, int, sceKernelCancelMsgPipe, SceUID uid, int *send_waiters,
       int *receive_waiters)
{
    return _sceKernelCancelMsgPipe(uid, send_waiters, receive_waiters);
}

EXPORT(SceThreadmgr, 0x0E1CB9F6, SceUID, sceKernelOpenMsgPipe, const char *name)
{
    VitaMsgPipeInfo *info = NULL;
    vita_msgpipe_info_dict_it_t it;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_vita_sync_objects_lock);
    for (vita_msgpipe_info_dict_it(it, g_vita_msgpipe_infos);
         !vita_msgpipe_info_dict_end_p(it); vita_msgpipe_info_dict_next(it)) {
        VitaMsgPipeInfo *candidate = vita_msgpipe_info_dict_ref(it)->value;

        if (strncmp(candidate->name, name, sizeof(candidate->name)) == 0) {
            mutexLock(&candidate->lock);
            candidate->refs++;
            mutexUnlock(&candidate->lock);
            info = candidate;
            break;
        }
    }
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return info ? info->uid : SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;
}

EXPORT(SceThreadmgr, 0x1305A065, int, sceKernelCloseMsgPipe, SceUID uid)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    mutexLock(&info->lock);
    if (info->refs > 0)
        info->refs--;
    mutexUnlock(&info->lock);

    return 0;
}

EXPORT(SceThreadmgr, 0x4046D16B, int, sceKernelGetMsgPipeInfo, SceUID uid,
       SceKernelMppInfo *info)
{
    VitaMsgPipeInfo *pipe = get_msgpipe_info(uid);

    if (!pipe)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    mutexLock(&pipe->lock);
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->mppId = pipe->uid;
    memcpy(info->name, pipe->name, sizeof(info->name));
    info->attr = 0;
    info->bufSize = (int)pipe->capacity;
    info->freeSize = (int)(pipe->capacity - pipe->used);
    info->numSendWaitThreads = (int)pipe->send_waiters;
    info->numReceiveWaitThreads = (int)pipe->receive_waiters;
    mutexUnlock(&pipe->lock);

    return 0;
}

EXPORT(SceThreadmgr, 0x7AE31060, int, _sceKernelGetMsgPipeInfo, SceUID uid,
       SceKernelMppInfo *info)
{
    return sceKernelGetMsgPipeInfo(uid, info);
}

EXPORT(SceLibKernel, 0xD7E10BB1, SceUID, sceKernelCreateMsgPipeWithLR, const char *name, int type,
       int attr, unsigned int buf_size, void *option)
{
    return sceKernelCreateMsgPipe(name, type, attr, buf_size, option);
}

EXPORT(SceThreadmgr, 0x2AAC8BFD, SceUID, _sceKernelCreateMsgPipeWithLR, const char *name,
       int type, int attr, unsigned int buf_size, void *option)
{
    return sceKernelCreateMsgPipe(name, type, attr, buf_size, option);
}

EXPORT(SceThreadmgr, 0x70E2A6D2, SceUID, sceKernelGetMsgPipeCreatorId, SceUID uid)
{
    (void)uid;
    return 1;
}

EXPORT(SceLibKernel, 0x6E903AB2, int, sceIoChstatByFd, SceUID fd, const SceIoStat *stat,
       unsigned int bits)
{
    (void)fd;
    (void)stat;
    (void)bits;
    return 0;
}

EXPORT(SceLibKernel, 0x04B30CB2, int, sceIoDevctl, const char *dev, unsigned int cmd,
       void *indata, int inlen, void *outdata, int outlen)
{
    (void)dev;
    (void)cmd;
    (void)indata;
    (void)inlen;

    if (outdata && outlen > 0)
        memset(outdata, 0, (size_t)outlen);

    return 0;
}

EXPORT(SceLibKernel, 0x8D9C5461, int, sceKernelGetThreadInfo, SceUID thid,
       SceKernelThreadInfo *info)
{
    VitaThreadInfo *ti = get_thread_info_or_current(thid);

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->processId = 1;
    strncpy(info->name, ti->name, sizeof(info->name) - 1);
    info->attr = ti->attr;
    info->status = 0x2; /* running */
    info->entry = ti->entry;
    info->stackSize = (SceInt32)ti->stack_size;
    info->initPriority = ti->init_priority;
    info->currentPriority = ti->current_priority;
    info->initCpuAffinityMask = 1;
    info->currentCpuAffinityMask = 1;
    info->exitStatus = ti->return_status;

    return 0;
}

EXPORT(SceThreadmgr, 0xB373D8A1, int, _sceKernelGetThreadInfo, SceUID thid,
       SceKernelThreadInfo *info)
{
    return sceKernelGetThreadInfo(thid, info);
}

EXPORT(SceThreadmgr, 0x9DCB4B7A, SceUID, sceKernelGetProcessId, void)
{
    return 1;
}

EXPORT(SceLibKernel, 0xE9F973B1, SceUInt32, sceKernelGetProcessTimeLow, void)
{
    return (SceUInt32)(host_monotonic_ns() / 1000ull);
}

EXPORT(SceProcessmgr, 0x4B879059, int, sceKernelLibcGettimeofday, SceKernelTimeval *tv,
       SceKernelTimezone *tz)
{
    struct timeval host_time;

    if (!tv)
        return -1;

    gettimeofday(&host_time, NULL);
    tv->sec = (SceInt32)host_time.tv_sec;
    tv->usec = (SceInt32)host_time.tv_usec;

    if (tz)
        tz->value = 0;

    return 0;
}

/* SceLibKernel declared in SceLibKernel.c */
/* --------------------------------------------------- condition variables */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    SceUID mutex_id;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaCondInfo;

DICT_DEF2(vita_cond_dict, SceUID, M_DEFAULT_OPLIST, VitaCondInfo *, M_POD_OPLIST)
static vita_cond_dict_t g_conds;
static RwLock g_conds_lock;

static void sync_deadline_from_us(struct timespec *ts, unsigned int microseconds)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(microseconds / 1000000u);
    ts->tv_nsec += (long)(microseconds % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static VitaCondInfo *cond_lookup(SceUID uid)
{
    VitaCondInfo *cond;

    VitaCondInfo **slot;

    rwlockReadLock(&g_conds_lock);
    slot = vita_cond_dict_get(g_conds, uid);
    cond = slot ? *slot : NULL;
    rwlockReadUnlock(&g_conds_lock);

    return cond;
}

SceUID sceKernelCreateCond(const char *name, SceUInt attr, SceUID mutex_id,
                           const SceKernelCondOptParam *option);

EXPORT(SceThreadmgr, 0xCC14FA59, SceUID, _sceKernelCreateCond, const char *name, SceUInt attr,
       SceUID mutex_id, const SceKernelCondOptParam *option)
{
    return sceKernelCreateCond(name, attr, mutex_id, option);
}

EXPORT(SceLibKernel, 0x50572FDA, SceUID, sceKernelCreateCond, const char *name, SceUInt attr,
       SceUID mutex_id, const SceKernelCondOptParam *option)
{
    VitaCondInfo *cond;

    (void)option;

    cond = calloc(1, sizeof(*cond));
    if (!cond)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    cond->uid = SceSysmem_get_next_uid();
    cond->attr = attr;
    cond->mutex_id = mutex_id;
    if (name)
        strncpy(cond->name, name, sizeof(cond->name) - 1);

    pthread_mutex_init(&cond->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cond->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_conds_lock);
    vita_cond_dict_set_at(g_conds, cond->uid, cond);
    rwlockWriteUnlock(&g_conds_lock);

    return cond->uid;
}

EXPORT(SceThreadmgr, 0x879E6EBD, int, sceKernelDeleteCond, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    rwlockWriteLock(&g_conds_lock);
    vita_cond_dict_erase(g_conds, uid);
    rwlockWriteUnlock(&g_conds_lock);

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_broadcast(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    pthread_cond_destroy(&cond->cond);
    pthread_mutex_destroy(&cond->mutex);
    free(cond);

    return 0;
}

static int cond_wait(VitaCondInfo *cond, unsigned int *timeout)
{
    int result = 0;

    /* The caller holds the associated mutex; release it while waiting. */
    if (sceKernelUnlockMutex(cond->mutex_id, 1) != 0)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    pthread_mutex_lock(&cond->mutex);
    cond->waiting++;
    if (timeout) {
        struct timespec ts;

        sync_deadline_from_us(&ts, *timeout);
        if (pthread_cond_timedwait(&cond->cond, &cond->mutex, &ts) == ETIMEDOUT)
            result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    } else {
        pthread_cond_wait(&cond->cond, &cond->mutex);
    }
    cond->waiting--;
    pthread_mutex_unlock(&cond->mutex);

    sceKernelLockMutex(cond->mutex_id, 1, NULL);

    return result;
}

EXPORT(SceLibKernel, 0xC88D44AD, int, sceKernelWaitCond, SceUID uid, unsigned int *timeout)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    return cond_wait(cond, timeout);
}

EXPORT(SceThreadmgr, 0x040795C7, int, _sceKernelWaitCond, SceUID uid, unsigned int *timeout)
{
    return sceKernelWaitCond(uid, timeout);
}

EXPORT(SceThreadmgr, 0x452B0AB3, int, _sceKernelWaitCondCB, SceUID uid, unsigned int *timeout)
{
    return sceKernelWaitCondCB(uid, timeout);
}

EXPORT(SceLibKernel, 0x4CE42CE2, int, sceKernelWaitCondCB, SceUID uid, unsigned int *timeout)
{
    return sceKernelWaitCond(uid, timeout);
}

EXPORT(SceThreadmgr, 0x6ED2E2DC, int, sceKernelSignalCond, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_signal(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xC2E7AC22, int, sceKernelSignalCondAll, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_broadcast(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x1269F4EC, int, sceKernelSignalCondTo, SceUID uid, SceUID thread_id)
{
    (void)thread_id;

    return sceKernelSignalCond(uid);
}

EXPORT(SceLibKernel, 0x6864DCE2, int, sceKernelGetCondInfo, SceUID uid, SceKernelCondInfo *info)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->condId = cond->uid;
    memcpy(info->name, cond->name, sizeof(info->name));
    info->attr = cond->attr;
    info->mutexId = cond->mutex_id;

    pthread_mutex_lock(&cond->mutex);
    info->numWaitThreads = cond->waiting;
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x76BDA02F, SceUID, sceKernelOpenCond, const char *name)
{
    VitaCondInfo *result = NULL;
    vita_cond_dict_it_t it;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    rwlockReadLock(&g_conds_lock);
    for (vita_cond_dict_it(it, g_conds); !vita_cond_dict_end_p(it); vita_cond_dict_next(it)) {
        VitaCondInfo *cond = vita_cond_dict_ref(it)->value;

        if (strncmp(cond->name, name, sizeof(cond->name)) == 0) {
            result = cond;
            break;
        }
    }
    rwlockReadUnlock(&g_conds_lock);

    return result ? result->uid : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}

EXPORT(SceThreadmgr, 0x15C690E0, int, sceKernelCloseCond, SceUID uid)
{
    return cond_lookup(uid) ? 0 : SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}


EXPORT(SceThreadmgr, 0x05C51CE1, int, _sceKernelGetCondInfo, SceUID uid,
       SceKernelCondInfo *info)
{
    return sceKernelGetCondInfo(uid, info);
}

EXPORT(SceLibKernel, 0xC53A9848, int, sceKernelGetThreadCurrentPriority, void)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    return ti ? ti->current_priority : 0;
}

EXPORT(SceLibKernel, 0xD5DC26C4, int, sceKernelGetThreadExitStatus, SceUID thid, int *status)
{
    VitaThreadInfo *ti = get_thread_info_or_current(thid);

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (!status)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    *status = ti->return_status;
    return 0;
}

EXPORT(SceThreadmgr, 0xD3210C08, int, _sceKernelGetThreadExitStatus, SceUID thid, int *status)
{
    return sceKernelGetThreadExitStatus(thid, status);
}

EXPORT(SceLibKernel, 0xD6B01013, int, sceKernelGetThreadRunStatus, SceUID thid,
       SceKernelThreadRunStatus *status)
{
    VitaThreadInfo *ti = thid ? get_thread_info_for_uid(thid) : SceKernelThreadMgr_get_thread_info();

    if (!status)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);
    status->cpuInfo[0].processId = 1;
    status->cpuInfo[0].threadId = ti->uid;
    status->cpuInfo[0].priority = ti->current_priority;

    return 0;
}

EXPORT(SceThreadmgr, 0xC7FB5497, int, _sceKernelGetThreadRunStatus, SceUID thid,
       SceKernelThreadRunStatus *status)
{
    return sceKernelGetThreadRunStatus(thid, status);
}

EXPORT(SceLibKernel, 0xE0241FAA, int, sceKernelGetSystemInfo, SceKernelSystemInfo *info)
{
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->activeCpuMask = 1;

    return 0;
}

EXPORT(SceThreadmgr, 0x80544E0C, int, _sceKernelGetSystemInfo, SceKernelSystemInfo *info)
{
    return sceKernelGetSystemInfo(info);
}

EXPORT(SceLibKernel, 0x84092FD0, int, sceKernelCheckThreadStack, void)
{
    return 0;
}

EXPORT(SceThreadmgr, 0x4F8A3DA0, int, sceKernelGetThreadStackFreeSize, SceUID thid)
{
    VitaThreadInfo *ti = get_thread_info_or_current(thid);

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    return ti->stack_size > 0x1000 ? (int)(ti->stack_size - 0x1000) : 0;
}

EXPORT(SceLibKernel, 0x36833F93, int, sceKernelChangeCurrentThreadAttr, SceUInt clear_attr,
       SceUInt set_attr)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    ti->attr = (ti->attr & ~clear_attr) | set_attr;
    return 0;
}

EXPORT(SceThreadmgr, 0xBD0139F2, int, sceKernelChangeThreadPriority, SceUID thid, int priority)
{
    VitaThreadInfo *ti = get_thread_info_or_current(thid);

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    ti->current_priority = priority;
    return 0;
}

EXPORT(SceThreadmgr, 0xF1AE5654, int, sceKernelGetThreadCpuAffinityMask, SceUID thid)
{
    return get_thread_info_or_current(thid) ? 1 : SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
}

EXPORT(SceThreadmgr, 0x212E6C35, int, _sceKernelGetThreadCpuAffinityMask, SceUID thid)
{
    return sceKernelGetThreadCpuAffinityMask(thid);
}

EXPORT(SceLibKernel, 0x8C57AC2A, int, sceKernelGetThreadCpuAffinityMask2, void)
{
    return 1;
}

EXPORT(SceThreadmgr, 0x15129174, int, sceKernelChangeThreadCpuAffinityMask, SceUID thid,
       int mask)
{
    (void)mask;
    return get_thread_info_or_current(thid) ? 1 : SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
}

EXPORT(SceLibKernel, 0xC54941ED, int, sceKernelWaitThreadEndCB, SceUID thid, int *stat,
       SceUInt *timeout)
{
    return sceKernelWaitThreadEnd(thid, stat, timeout);
}

EXPORT(SceThreadmgr, 0xFA3D4491, int, _sceKernelWaitThreadEndCB, SceUID thid, int *stat,
       SceUInt *timeout)
{
    return sceKernelWaitThreadEnd(thid, stat, timeout);
}

EXPORT(SceThreadmgr, 0x0373C5E3, int, sceKernelWaitThreadEndCB_089, SceUID thid, int *stat,
       SceUInt *timeout)
{
    return sceKernelWaitThreadEnd(thid, stat, timeout);
}

DECLARE_LIBRARY(SceThreadmgr, 0x859a24b1);
DECLARE_LIBRARY(SceThreadmgrCoredumpTime, 0x5E8D0E22);
DECLARE_LIBRARY(SceProcessmgr, 0x2dd91812);

int SceKernelThreadMgr_init(void)
{
    g_vita_thread_info_tls_slot_id = threadTlsAlloc(NULL);
    if (g_vita_thread_info_tls_slot_id < 0)
        return -1;

    vita_thread_info_dict_init(g_vita_thread_infos);
    rwlockInit(&g_vita_thread_infos_lock);
    vita_mutex_info_dict_init(g_vita_mutex_infos);
    vita_msgpipe_info_dict_init(g_vita_msgpipe_infos);
    vita_cond_dict_init(g_conds);
    rwlockInit(&g_conds_lock);
    rwlockInit(&g_vita_sync_objects_lock);

    return 0;
}

int SceKernelThreadMgr_finish(void)
{
    threadTlsFree(g_vita_thread_info_tls_slot_id);
    vita_thread_info_dict_clear(g_vita_thread_infos);
    vita_mutex_info_dict_clear(g_vita_mutex_infos);
    vita_msgpipe_info_dict_clear(g_vita_msgpipe_infos);
    vita_cond_dict_clear(g_conds);
    rwlockDestroy(&g_conds_lock);

    return 0;
}

VitaThreadInfo *SceKernelThreadMgr_get_thread_info(void)
{
    return threadTlsGet(g_vita_thread_info_tls_slot_id);
}
