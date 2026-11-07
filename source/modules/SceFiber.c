/*
 * SceFiber.
 *
 * Fibers are cooperative user-space contexts. The host runs guest code
 * natively, so the cleanest mapping is the host's own ucontext API: each
 * SceFiber owns a ucontext_t and switches with swapcontext/setcontext.
 *
 * The guest supplies the context stack region in _sceFiberInitializeImpl, and
 * it is used directly because guest and host addresses are the same here. If
 * the caller does not supply one, a host stack is allocated.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include <psp2/fiber.h>
#include <psp2/types.h>

#include "log.h"
#include "module.h"

#define SCE_FIBER_LIBRARY_NID 0xF88DAA70

#define FIBER_STACK_FALLBACK (128 * 1024)

typedef enum {
    FIBER_STATE_NEW,
    FIBER_STATE_RUNNING,
    FIBER_STATE_SUSPENDED,
    FIBER_STATE_FINISHED,
} FiberState;

typedef struct VitaFiber {
    SceFiber *guest;
    ucontext_t context;
    SceFiberEntry *entry;
    SceUInt32 arg_on_initialize;
    SceUInt32 arg_on_run;
    SceUInt32 arg_on_return;
    void *context_addr;
    SceSize context_size;
    void *owned_stack;
    char name[32];
    FiberState state;
    struct VitaFiber *next;
} VitaFiber;

typedef struct {
    VitaFiber *current;
    ucontext_t root_context;
    int has_root;
} FiberThreadState;

static __thread FiberThreadState g_fiber_thread;

static VitaFiber *g_fibers;
static pthread_mutex_t g_fibers_lock = PTHREAD_MUTEX_INITIALIZER;

static void fiber_register(VitaFiber *fiber)
{
    pthread_mutex_lock(&g_fibers_lock);
    fiber->next = g_fibers;
    g_fibers = fiber;
    pthread_mutex_unlock(&g_fibers_lock);
}

static void fiber_unregister(VitaFiber *fiber)
{
    pthread_mutex_lock(&g_fibers_lock);
    for (VitaFiber **link = &g_fibers; *link; link = &(*link)->next) {
        if (*link == fiber) {
            *link = fiber->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_fibers_lock);
}

static VitaFiber *fiber_lookup(const SceFiber *guest)
{
    VitaFiber *result = NULL;

    if (!guest)
        return NULL;

    pthread_mutex_lock(&g_fibers_lock);
    for (VitaFiber *fiber = g_fibers; fiber; fiber = fiber->next) {
        if (fiber->guest == guest) {
            result = fiber;
            break;
        }
    }
    pthread_mutex_unlock(&g_fibers_lock);

    return result;
}

static void fiber_trampoline(void)
{
    VitaFiber *fiber = g_fiber_thread.current;

    if (!fiber)
        return;

    fiber->state = FIBER_STATE_RUNNING;
    fiber->entry(fiber->arg_on_initialize, fiber->arg_on_run);
    fiber->state = FIBER_STATE_FINISHED;
    fiber->arg_on_return = 0;
    setcontext(&g_fiber_thread.root_context);
}

static SceInt32 fiber_initialize(SceFiber *fiber, const char *name, SceFiberEntry *entry,
                                 SceUInt32 arg_on_initialize, void *addr_context,
                                 SceSize size_context, SceFiberOptParam *params)
{
    VitaFiber *vf;
    void *stack_memory;

    (void)params;

    if (!fiber || !entry)
        return SCE_FIBER_ERROR_NULL;
    if (((uintptr_t)fiber & 7) != 0)
        return SCE_FIBER_ERROR_ALIGNMENT;
    if (fiber_lookup(fiber))
        return SCE_FIBER_ERROR_INVALID;

    if (addr_context) {
        if (((uintptr_t)addr_context & 7) != 0)
            return SCE_FIBER_ERROR_ALIGNMENT;
        if (size_context < 4 * 1024)
            return SCE_FIBER_ERROR_RANGE;
        stack_memory = NULL;
    } else {
        stack_memory = malloc(FIBER_STACK_FALLBACK);
        if (!stack_memory)
            return SCE_FIBER_ERROR_FATAL;
        addr_context = stack_memory;
        size_context = FIBER_STACK_FALLBACK;
    }

    vf = calloc(1, sizeof(*vf));
    if (!vf) {
        free(stack_memory);
        return SCE_FIBER_ERROR_FATAL;
    }

    vf->guest = fiber;
    vf->entry = entry;
    vf->arg_on_initialize = arg_on_initialize;
    vf->arg_on_run = 0;
    vf->context_addr = addr_context;
    vf->context_size = size_context;
    vf->owned_stack = stack_memory;
    vf->state = FIBER_STATE_NEW;
    if (name)
        strncpy(vf->name, name, sizeof(vf->name) - 1);

    if (getcontext(&vf->context) != 0) {
        free(stack_memory);
        free(vf);
        return SCE_FIBER_ERROR_FATAL;
    }
    vf->context.uc_stack.ss_sp = addr_context;
    vf->context.uc_stack.ss_size = size_context;
    vf->context.uc_link = NULL;
    makecontext(&vf->context, fiber_trampoline, 0);

    memset(fiber, 0, sizeof(*fiber));
    fiber_register(vf);

    return 0;
}

EXPORT(SceFiber, 0xC6A3F9BB, SceInt32, _sceFiberInitializeWithInternalOptionImpl, SceFiber *fiber,
       const char *name, SceFiberEntry *entry, SceUInt32 arg_on_initialize, void *addr_context,
       SceSize size_context, SceFiberOptParam *params)
{
    return fiber_initialize(fiber, name, entry, arg_on_initialize, addr_context, size_context,
                            params);
}

EXPORT(SceFiber, 0xF24A298C, SceInt32, _sceFiberInitializeImpl, SceFiber *fiber, char *name,
       SceFiberEntry *entry, SceUInt32 arg_on_initialize, void *addr_context, SceSize size_context,
       SceFiberOptParam *params)
{
    return fiber_initialize(fiber, name, entry, arg_on_initialize, addr_context, size_context,
                            params);
}

EXPORT(SceFiber, 0x801AB334, SceInt32, sceFiberOptParamInitialize, SceFiberOptParam *params)
{
    if (!params)
        return SCE_FIBER_ERROR_NULL;

    memset(params, 0, sizeof(*params));
    return 0;
}

EXPORT(SceFiber, 0xE160F844, SceInt32, sceFiberFinalize, SceFiber *fiber)
{
    VitaFiber *vf = fiber_lookup(fiber);

    if (!vf)
        return SCE_FIBER_ERROR_INVALID;
    if (vf->state == FIBER_STATE_RUNNING)
        return SCE_FIBER_ERROR_BUSY;

    fiber_unregister(vf);
    free(vf->owned_stack);
    free(vf);
    memset(fiber, 0, sizeof(*fiber));

    return 0;
}

EXPORT(SceFiber, 0x7DF23243, SceInt32, sceFiberRun, SceFiber *fiber, SceUInt32 arg_on_run_to,
       SceUInt32 *arg_on_run)
{
    VitaFiber *vf = fiber_lookup(fiber);
    VitaFiber *previous = g_fiber_thread.current;

    if (!vf)
        return SCE_FIBER_ERROR_INVALID;
    if (g_fiber_thread.current)
        return SCE_FIBER_ERROR_STATE;
    if (vf->state == FIBER_STATE_RUNNING || vf->state == FIBER_STATE_FINISHED)
        return SCE_FIBER_ERROR_STATE;

    vf->arg_on_run = arg_on_run_to;
    vf->state = FIBER_STATE_RUNNING;
    g_fiber_thread.current = vf;

    g_fiber_thread.has_root = 1;
    swapcontext(&g_fiber_thread.root_context, &vf->context);

    g_fiber_thread.current = previous;
    if (vf->state != FIBER_STATE_FINISHED)
        vf->state = FIBER_STATE_SUSPENDED;

    if (arg_on_run)
        *arg_on_run = vf->arg_on_return;

    return 0;
}

EXPORT(SceFiber, 0xE4283144, SceInt32, sceFiberSwitch, SceFiber *fiber, SceUInt32 arg_on_run_to,
       SceUInt32 *arg_on_run)
{
    VitaFiber *target = fiber_lookup(fiber);
    VitaFiber *current = g_fiber_thread.current;

    if (!target)
        return SCE_FIBER_ERROR_INVALID;
    if (!current)
        return SCE_FIBER_ERROR_STATE;
    if (target == current)
        return SCE_FIBER_ERROR_BUSY;
    if (target->state == FIBER_STATE_RUNNING || target->state == FIBER_STATE_FINISHED)
        return SCE_FIBER_ERROR_STATE;

    target->arg_on_run = arg_on_run_to;
    target->state = FIBER_STATE_RUNNING;
    current->state = FIBER_STATE_SUSPENDED;
    g_fiber_thread.current = target;

    swapcontext(&current->context, &target->context);

    /* Resumed: the other fiber switched back to us. */
    g_fiber_thread.current = current;
    current->state = FIBER_STATE_RUNNING;
    if (arg_on_run)
        *arg_on_run = current->arg_on_run;

    return 0;
}

EXPORT(SceFiber, 0x3B42921F, SceInt32, sceFiberReturnToThread, SceUInt32 arg_on_return,
       SceUInt32 *arg_on_run)
{
    VitaFiber *current = g_fiber_thread.current;

    if (!current)
        return SCE_FIBER_ERROR_STATE;
    if (!g_fiber_thread.has_root)
        return SCE_FIBER_ERROR_STATE;

    current->arg_on_return = arg_on_return;
    current->state = FIBER_STATE_SUSPENDED;
    g_fiber_thread.current = NULL;

    swapcontext(&current->context, &g_fiber_thread.root_context);

    /* Resumed by a later sceFiberRun. */
    g_fiber_thread.current = current;
    current->state = FIBER_STATE_RUNNING;
    if (arg_on_run)
        *arg_on_run = current->arg_on_run;

    return 0;
}

EXPORT(SceFiber, 0xE00B9AFE, SceInt32, _sceFiberAttachContextAndSwitch, SceFiber *fiber,
       void *addr_context, SceSize size_context, SceUInt32 arg_on_run_to, SceUInt32 *arg_on_run)
{
    (void)addr_context;
    (void)size_context;

    return sceFiberSwitch(fiber, arg_on_run_to, arg_on_run);
}

EXPORT(SceFiber, 0x7D0C7DDB, SceInt32, _sceFiberAttachContextAndRun, SceFiber *fiber,
       void *addr_context, SceSize size_context, SceUInt32 arg_on_run_to, SceUInt32 *arg_on_run)
{
    (void)addr_context;
    (void)size_context;

    return sceFiberRun(fiber, arg_on_run_to, arg_on_run);
}

EXPORT(SceFiber, 0x414D8CA5, SceInt32, sceFiberGetSelf, SceFiber *fiber)
{
    if (!fiber)
        return SCE_FIBER_ERROR_NULL;
    if (!g_fiber_thread.current)
        return SCE_FIBER_ERROR_STATE;

    *fiber = *g_fiber_thread.current->guest;
    return 0;
}

EXPORT(SceFiber, 0x189599B4, SceInt32, sceFiberGetInfo, SceFiber *fiber, SceFiberInfo *info)
{
    VitaFiber *vf = fiber_lookup(fiber);

    if (!vf || !info)
        return SCE_FIBER_ERROR_NULL;

    memset(info, 0, sizeof(*info));
    info->entry = vf->entry;
    info->argOnInitialize = vf->arg_on_initialize;
    info->addrContext = vf->context_addr;
    info->sizeContext = vf->context_size;
    memcpy(info->name, vf->name, sizeof(info->name));

    return 0;
}

EXPORT(SceFiber, 0xDD50DF3D, SceInt32, sceFiberRenameSelf, const char *name)
{
    if (!g_fiber_thread.current || !name)
        return SCE_FIBER_ERROR_INVALID;

    memset(g_fiber_thread.current->name, 0, sizeof(g_fiber_thread.current->name));
    strncpy(g_fiber_thread.current->name, name, sizeof(g_fiber_thread.current->name) - 1);

    return 0;
}

EXPORT(SceFiber, 0xCD543A71, SceInt32, sceFiberStartContextSizeCheck, SceUInt32 flags)
{
    (void)flags;
    return 0;
}

EXPORT(SceFiber, 0x67F70867, SceInt32, sceFiberStopContextSizeCheck, void)
{
    return 0;
}

EXPORT(SceFiber, 0x9C414B09, SceInt32, sceFiberPushUserMarkerWithHud, const char *marker)
{
    (void)marker;
    return 0;
}

EXPORT(SceFiber, 0x4EC01F9C, SceInt32, sceFiberPopUserMarkerWithHud, void)
{
    return 0;
}

DECLARE_LIBRARY(SceFiber, SCE_FIBER_LIBRARY_NID);
