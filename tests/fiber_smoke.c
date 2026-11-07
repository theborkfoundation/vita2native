/*
 * Exercises SceFiber on the host ucontext implementation: initialization,
 * run/return, switch between fibers, introspection and finalization.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/fiber.h>

static int g_stage;
static SceUInt32 g_seen_init;
static SceUInt32 g_seen_run;
static SceUInt32 g_switch_value;
static SceUInt32 g_resumed_with;

static void entry_increment(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run)
{
    g_stage = 1;
    g_seen_init = arg_on_initialize;
    g_seen_run = arg_on_run;
}

static void entry_yield(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run)
{
    (void)arg_on_initialize;
    g_seen_run = arg_on_run;

    /* Return to the thread and record the value it hands back on resume. */
    g_stage = 1;
    g_resumed_with = 0;
    sceFiberReturnToThread(0x1234, &g_resumed_with);
    g_stage = 2;
}

static SceFiber *g_ping_to;
static SceFiber *g_pong_to;

static void entry_ping(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run)
{
    SceUInt32 value = 0;

    (void)arg_on_initialize;
    g_stage = 10;
    sceFiberSwitch(g_ping_to, 0x2222, &value);
    g_switch_value = value;
}

static void entry_pong(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run)
{
    SceUInt32 value = 0;

    (void)arg_on_initialize;
    g_stage = 20;
    sceFiberSwitch(g_pong_to, 0x3333, &value);
}

int main(void)
{
    SceFiber increment;
    SceFiber yielder;
    SceFiber ping;
    SceFiber pong;
    SceFiberInfo info;
    SceFiber self;
    SceUInt32 out;

    /* Plain run-to-completion. */
    assert(_sceFiberInitializeImpl(&increment, "increment", entry_increment, 3, NULL, 0, NULL) ==
           0);
    out = 0;
    assert(sceFiberRun(&increment, 5, &out) == 0);
    assert(g_stage == 1);
    assert(g_seen_init == 3);
    assert(g_seen_run == 5);
    assert(out == 0);

    memset(&info, 0, sizeof(info));
    assert(sceFiberGetInfo(&increment, &info) == 0);
    assert(info.entry == entry_increment);
    assert(info.argOnInitialize == 3);
    assert(strcmp(info.name, "increment") == 0);
    assert(info.sizeContext >= 4096);

    assert(sceFiberFinalize(&increment) == 0);

    /* Return to the thread, then resume with a new argument. */
    assert(_sceFiberInitializeImpl(&yielder, "yielder", entry_yield, 0, NULL, 0, NULL) == 0);
    out = 0;
    assert(sceFiberRun(&yielder, 7, &out) == 0);
    assert(g_stage == 1);
    assert(out == 0x1234);

    out = 0;
    assert(sceFiberRun(&yielder, 9, &out) == 0);
    assert(g_stage == 2);
    assert(g_resumed_with == 9);
    assert(out == 0);

    assert(sceFiberFinalize(&yielder) == 0);

    /* Switch between two fibers and back. */
    assert(_sceFiberInitializeImpl(&ping, "ping", entry_ping, 0, NULL, 0, NULL) == 0);
    assert(_sceFiberInitializeImpl(&pong, "pong", entry_pong, 0, NULL, 0, NULL) == 0);
    g_ping_to = &pong;
    g_pong_to = &ping;
    out = 0;
    assert(sceFiberRun(&ping, 0, &out) == 0);
    /* ping -> pong -> ping; ping finishes and returns to the thread. */
    assert(g_stage == 20 || g_stage == 10);
    assert(g_switch_value == 0x3333);
    assert(sceFiberFinalize(&pong) == 0);
    assert(sceFiberFinalize(&ping) == 0);

    /* GetSelf outside a fiber is a state error. */
    assert(sceFiberGetSelf(&self) == SCE_FIBER_ERROR_STATE);
    assert(sceFiberRun(NULL, 0, NULL) == SCE_FIBER_ERROR_INVALID);
    assert(sceFiberReturnToThread(0, NULL) == SCE_FIBER_ERROR_STATE);

    /* A fiber with a caller-provided stack region. */
    {
        static uint8_t stack[64 * 1024] __attribute__((aligned(16)));
        SceFiber embedded;

        assert(_sceFiberInitializeImpl(&embedded, "embedded", entry_increment, 1, stack,
                                       sizeof(stack), NULL) == 0);
        assert(sceFiberRun(&embedded, 2, NULL) == 0);
        assert(g_seen_init == 1 && g_seen_run == 2);
        assert(sceFiberFinalize(&embedded) == 0);
    }

    puts("fiber_smoke: ok");
    return 0;
}
