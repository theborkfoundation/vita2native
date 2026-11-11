/*
 * SceNpManager / SceNpBasic / SceNpScore / SceNpActivity / SceNpTrophy /
 * SceNpCommon / SceNpCommerce2 / SceNearUtil / SceLiveAreaUtil.
 *
 * There is no PlayStation Network on this host, so the console is presented as
 * permanently signed out: sceNpGetServiceState reports SIGNED_OUT and the
 * service-state callback is dispatched with the same value. Titles such as
 * Angry Birds Trilogy use that to take their offline path (usually a message
 * dialog) instead of waiting forever for an account that never appears.
 *
 * The remaining entry points mirror Vita3K's behaviour: they succeed without
 * doing anything, which is what the titles expect from an emulator that does
 * not implement online services.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "settings.h"

#define SCE_NP_SERVICE_STATE_SIGNED_OUT 1

/* SceNpId, 0x24 bytes (see the Vita NP headers). */
typedef struct {
    char data[16];
    char term;
    char dummy[3];
} SceNpOnlineId;

typedef struct {
    uint8_t unknown[4];
    char platformType[4];
} SceNpIdOptParam;

typedef struct {
    SceNpOnlineId handle;
    SceNpIdOptParam opt;
    int8_t isIdValid;
    uint8_t reserved[7];
} SceNpId;

#define NP_MAX_SERVICE_CALLBACKS 4

typedef struct {
    void (*callback)(uint32_t state, uint32_t unknown, void *arg);
    void *arg;
} NpServiceCallback;

static NpServiceCallback g_service_callbacks[NP_MAX_SERVICE_CALLBACKS];
static bool g_np_initialized;

static int np_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG");
    return enabled;
}

#define NP_LOG(name)                                                                               \
    do {                                                                                           \
        if (np_debug())                                                                            \
            fprintf(stderr, "vita2native: %s\n", name);                                            \
    } while (0)

static uint32_t np_service_state(void)
{
    return SCE_NP_SERVICE_STATE_SIGNED_OUT;
}

EXPORT(SceNpManager, 0x04D9F484, int, sceNpInit, const void *config, void *pool)
{
    NP_LOG("sceNpInit");
    (void)config;
    (void)pool;

    g_np_initialized = true;
    if (np_debug())
        fprintf(stderr, "vita2native: sceNpInit\n");
    return 0;
}

EXPORT(SceNpManager, 0x19E40AE1, int, sceNpTerm, void)
{
    NP_LOG("sceNpTerm");
    g_np_initialized = false;
    memset(g_service_callbacks, 0, sizeof(g_service_callbacks));
    return 0;
}

EXPORT(SceNpManager, 0x54060DF6, int, sceNpGetServiceState, uint32_t *state)
{
    NP_LOG("sceNpGetServiceState");
    if (!state)
        return 0x80550503; /* SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT */

    *state = np_service_state();
    if (np_debug()) {
#if defined(__arm__)
        void *lr;

        __asm__ volatile("mov %0, lr" : "=r"(lr));
#else
        void *lr = __builtin_return_address(0);
#endif
        fprintf(stderr, "vita2native: sceNpGetServiceState lr=%p -> %u\n", lr,
                (unsigned)*state);
    }
    return 0;
}

EXPORT(SceNpManager, 0x3B0AE9A9, int, sceNpCheckCallback, void)
{
    uint32_t state = np_service_state();

    for (unsigned int i = 0; i < NP_MAX_SERVICE_CALLBACKS; i++) {
        if (g_service_callbacks[i].callback)
            g_service_callbacks[i].callback(state, 0, g_service_callbacks[i].arg);
    }
    return 0;
}

EXPORT(SceNpManager, 0x44239C35, int, sceNpRegisterServiceStateCallback,
       void (*callback)(uint32_t state, uint32_t unknown, void *arg), void *arg)
{
    NP_LOG("sceNpRegisterServiceStateCallback");
    if (!callback)
        return 0x80550503;

    for (unsigned int i = 0; i < NP_MAX_SERVICE_CALLBACKS; i++) {
        if (!g_service_callbacks[i].callback) {
            g_service_callbacks[i].callback = callback;
            g_service_callbacks[i].arg = arg;
            if (np_debug())
                fprintf(stderr, "vita2native: sceNpRegisterServiceStateCallback %p\n",
                        (void *)callback);
            return 0;
        }
    }
    return 0x80550504; /* SCE_NP_MANAGER_ERROR_OUT_OF_MEMORY */
}

EXPORT(SceNpManager, 0xD9E6E56C, int, sceNpUnregisterServiceStateCallback, void)
{
    memset(g_service_callbacks, 0, sizeof(g_service_callbacks));
    return 0;
}

EXPORT(SceNpManager, 0x3C94B4B4, int, sceNpManagerGetNpId, SceNpId *id)
{
    NP_LOG("sceNpManagerGetNpId");
    if (!id)
        return 0x80550503;

    memset(id, 0, sizeof(*id));
    memcpy(id->handle.data, "vita2native", 11);
    id->handle.term = '\0';
    id->opt.platformType[0] = 'p';
    id->opt.platformType[1] = 's';
    id->opt.platformType[2] = 'p';
    id->opt.platformType[3] = '2';
    id->isIdValid = 1;

    if (np_debug())
        fprintf(stderr, "vita2native: sceNpManagerGetNpId -> %s\n", id->handle.data);
    return 0;
}

EXPORT(SceNpManager, 0xAF0073B2, int, sceNpManagerGetContentRatingFlag, int *is_restricted,
       int *age)
{
    NP_LOG("sceNpManagerGetContentRatingFlag");
    if (is_restricted)
        *is_restricted = 0;
    if (age)
        *age = 21;
    return 0;
}

/* ------------------------------------------------------------ SceNpBasic */

EXPORT(SceNpBasic, 0xEFB91A99, int, sceNpBasicInit, void)
{
    NP_LOG("sceNpBasicInit");
    return 0;
}

EXPORT(SceNpBasic, 0x389BCB3B, int, sceNpBasicTerm, void)
{
    NP_LOG("sceNpBasicTerm");
    return 0;
}

EXPORT(SceNpBasic, 0x26E6E048, int, sceNpBasicRegisterHandler, const void *handler, void *arg)
{
    NP_LOG("sceNpBasicRegisterHandler");
    (void)handler;
    (void)arg;
    return 0;
}

EXPORT(SceNpBasic, 0x050AE072, int, sceNpBasicUnregisterHandler, void)
{
    NP_LOG("sceNpBasicUnregisterHandler");
    return 0;
}

EXPORT(SceNpBasic, 0x20146AEC, int, sceNpBasicCheckCallback, void)
{
    NP_LOG("sceNpBasicCheckCallback");
    return 0;
}

/* ------------------------------------------------------------ SceNpScore */

EXPORT(SceNpScore, 0x0433069F, int, sceNpScoreInit, void)
{
    NP_LOG("sceNpScoreInit");
    return 0;
}

EXPORT(SceNpScore, 0x2050F98F, int, sceNpScoreTerm, void)
{
    NP_LOG("sceNpScoreTerm");
    return 0;
}

EXPORT(SceNpScore, 0x5685F225, int, sceNpScoreCreateTitleCtx, void)
{
    NP_LOG("sceNpScoreCreateTitleCtx");
    return 0;
}

EXPORT(SceNpScore, 0xF52EA88A, int, sceNpScoreDeleteTitleCtx, void)
{
    NP_LOG("sceNpScoreDeleteTitleCtx");
    return 0;
}

EXPORT(SceNpScore, 0xD30D1993, int, sceNpScoreCreateRequest, void)
{
    NP_LOG("sceNpScoreCreateRequest");
    return 0;
}

EXPORT(SceNpScore, 0xFFF24BB1, int, sceNpScoreDeleteRequest, void)
{
    NP_LOG("sceNpScoreDeleteRequest");
    return 0;
}

EXPORT(SceNpScore, 0x24B09634, int, sceNpScoreRecordScoreAsync, void)
{
    NP_LOG("sceNpScoreRecordScoreAsync");
    return 0;
}

EXPORT(SceNpScore, 0x45CD1D00, int, sceNpScoreGetRankingByNpIdAsync, void)
{
    NP_LOG("sceNpScoreGetRankingByNpIdAsync");
    return 0;
}

EXPORT(SceNpScore, 0xC45E3FCD, int, sceNpScoreGetRankingByRangeAsync, void)
{
    NP_LOG("sceNpScoreGetRankingByRangeAsync");
    return 0;
}

EXPORT(SceNpScore, 0x9F2A7AC9, int, sceNpScorePollAsync, void)
{
    NP_LOG("sceNpScorePollAsync");
    return 0;
}

EXPORT(SceNpScore, 0xA7E93CE1, int, sceNpScoreAbortRequest, void)
{
    NP_LOG("sceNpScoreAbortRequest");
    return 0;
}

/* --------------------------------------------------------- SceNpActivity */

EXPORT(SceNpActivity, 0xE0FFEE97, int, sceNpActivityInit, void)
{
    NP_LOG("sceNpActivityInit");
    return 0;
}

EXPORT(SceNpActivity, 0x9EA4901F, int, sceNpActivityTerm, void)
{
    NP_LOG("sceNpActivityTerm");
    return 0;
}

EXPORT(SceNpActivity, 0xBC7FDC77, int, sceNpActivityPostStatus, void)
{
    NP_LOG("sceNpActivityPostStatus");
    return 0;
}

/* ---------------------------------------------------------- SceNpTrophy */

EXPORT(SceNpTrophy, 0x34516838, int, sceNpTrophyInit, void)
{
    NP_LOG("sceNpTrophyInit");
    return 0;
}

EXPORT(SceNpTrophy, 0xBFE0F28F, int, sceNpTrophyTerm, void)
{
    NP_LOG("sceNpTrophyTerm");
    return 0;
}

EXPORT(SceNpTrophy, 0xC49FD33F, int, sceNpTrophyCreateContext, void)
{
    NP_LOG("sceNpTrophyCreateContext");
    return 0;
}

EXPORT(SceNpTrophy, 0x56F5CBA5, int, sceNpTrophyDestroyContext, void)
{
    NP_LOG("sceNpTrophyDestroyContext");
    return 0;
}

EXPORT(SceNpTrophy, 0x4EBC6977, int, sceNpTrophyCreateHandle, void)
{
    NP_LOG("sceNpTrophyCreateHandle");
    return 0;
}

EXPORT(SceNpTrophy, 0xFF142071, int, sceNpTrophyDestroyHandle, void)
{
    NP_LOG("sceNpTrophyDestroyHandle");
    return 0;
}

EXPORT(SceNpTrophy, 0xBA2B7F2A, int, sceNpTrophyGetGameInfo, void)
{
    NP_LOG("sceNpTrophyGetGameInfo");
    return 0;
}

EXPORT(SceNpTrophy, 0xB397AA24, int, sceNpTrophyUnlockTrophy, void)
{
    NP_LOG("sceNpTrophyUnlockTrophy");
    return 0;
}

EXPORT(SceNpTrophy, 0xC8D2A4DE, int, sceNpTrophyGetTrophyUnlockState, void)
{
    NP_LOG("sceNpTrophyGetTrophyUnlockState");
    return 0;
}

/* --------------------------------------------------------- SceNearUtil */

EXPORT(SceNearUtil, 0xBBCA5813, int, sceNearInitialize, void)
{
    NP_LOG("sceNearInitialize");
    return 0;
}

EXPORT(SceNearUtil, 0xFF3BC581, int, sceNearFinalize, void)
{
    NP_LOG("sceNearFinalize");
    return 0;
}

EXPORT(SceNearUtil, 0x2ED36EE2, int, sceNearRefresh, void)
{
    NP_LOG("sceNearRefresh");
    return 0;
}

EXPORT(SceNearUtil, 0xDE6F0859, int, sceNearGetDiscoveredGifts, void)
{
    NP_LOG("sceNearGetDiscoveredGifts");
    return 0;
}

EXPORT(SceNearUtil, 0xA64D9D38, int, sceNearGetDiscoveredGiftStatus, void)
{
    NP_LOG("sceNearGetDiscoveredGiftStatus");
    return 0;
}

EXPORT(SceNearUtil, 0x360D4A19, int, sceNearIgnoreDiscoveredGift, void)
{
    NP_LOG("sceNearIgnoreDiscoveredGift");
    return 0;
}

EXPORT(SceNearUtil, 0x06807CA3, int, sceNearOpenReceivedGiftData, void)
{
    NP_LOG("sceNearOpenReceivedGiftData");
    return 0;
}

EXPORT(SceNearUtil, 0x2F35C196, int, sceNearCloseReceivedGiftData, void)
{
    NP_LOG("sceNearCloseReceivedGiftData");
    return 0;
}

EXPORT(SceNearUtil, 0xB14145DD, int, sceNearReadReceivedGiftData, void)
{
    NP_LOG("sceNearReadReceivedGiftData");
    return 0;
}

EXPORT(SceNearUtil, 0x146BE236, int, sceNearSetGift, void)
{
    NP_LOG("sceNearSetGift");
    return 0;
}

EXPORT(SceNearUtil, 0xF3398774, int, sceNearLaunchNearAppForUpdate, void)
{
    NP_LOG("sceNearLaunchNearAppForUpdate");
    return 0;
}

/* ---------------------------------------------------------- SceNpCommon */

/*
 * Authentication only needs its pool set up: the request entry points stay
 * unresolved, so titles that query the ticket service fall back to their
 * offline path. Fort Defense asserts on a failed sceNpAuthInit during boot.
 */
EXPORT(SceNpCommon, 0x441D8B4E, int, sceNpAuthInit, void *pool, uint32_t pool_size)
{
    (void)pool;
    (void)pool_size;

    NP_LOG("sceNpAuthInit");
    return 0;
}

EXPORT(SceNpCommon, 0x6093B689, int, sceNpAuthTerm, void)
{
    NP_LOG("sceNpAuthTerm");
    return 0;
}

/* ------------------------------------------------------- SceNpCommerce2 */

/* The PS Store is not reachable, but the subsystem still initialises so the
   boot sequence of titles that link it completes. */
EXPORT(SceNpCommerce2, 0xC73F209A, int, sceNpCommerce2Init, void)
{
    NP_LOG("sceNpCommerce2Init");
    return 0;
}

EXPORT(SceNpCommerce2, 0xB99958AE, int, sceNpCommerce2Term, void)
{
    NP_LOG("sceNpCommerce2Term");
    return 0;
}

/* ----------------------------------------------------- SceLiveAreaUtil */

EXPORT(SceLiveAreaUtil, 0xD330285D, int, sceLiveAreaUpdateFrameAsync, void)
{
    NP_LOG("sceLiveAreaUpdateFrameAsync");
    return 0;
}

DECLARE_LIBRARY(SceNpManager, 0xD8835093);
DECLARE_LIBRARY(SceNpCommon, 0xECB62224);
DECLARE_LIBRARY(SceNpCommerce2, 0x40120DC3);
DECLARE_LIBRARY(SceNpBasic, 0x605BA760);
DECLARE_LIBRARY(SceNpScore, 0x75F052DB);
DECLARE_LIBRARY(SceNpActivity, 0xFEF492ED);
DECLARE_LIBRARY(SceNpTrophy, 0x4332C10D);
DECLARE_LIBRARY(SceNearUtil, 0x560D1608);
DECLARE_LIBRARY(SceLiveAreaUtil, 0x5303CB07);
