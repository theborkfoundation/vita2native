/*
 * ScePromoterUtil: package installation and LiveArea management.
 *
 * The host has no LiveArea and no package database. Bookkeeping calls succeed
 * so the caller's lifecycle works, while installation and deletion report
 * failure. CheckExist always reports that the title is not installed.
 */
#include <psp2/promoterutil.h>

#include "module.h"

EXPORT(ScePromoterUtil, 0x93451536, int, scePromoterUtilityInit, void)
{
    return 0;
}

EXPORT(ScePromoterUtil, 0xC95D24A6, int, scePromoterUtilityExit, void)
{
    return 0;
}

EXPORT(ScePromoterUtil, 0x7D46752F, int, scePromoterUtilityDeletePkg, const char *titleid)
{
    (void)titleid;
    return 0;
}

EXPORT(ScePromoterUtil, 0x17D73ECA, int, scePromoterUtilityUpdateLiveArea,
       ScePromoterUtilityLAUpdate *args)
{
    (void)args;
    return 0;
}

EXPORT(ScePromoterUtil, 0x4B37808F, int, scePromoterUtilityPromoteImport,
       ScePromoterUtilityImportParams *params)
{
    (void)params;
    return -1;
}

EXPORT(ScePromoterUtil, 0x716C81F4, int, scePromoterUtilityPromotePkg, const char *path,
       int sync)
{
    (void)path;
    (void)sync;
    return -1;
}

EXPORT(ScePromoterUtil, 0x86641BC6, int, scePromoterUtilityPromotePkgWithRif, const char *path,
       int sync)
{
    (void)path;
    (void)sync;
    return -1;
}

EXPORT(ScePromoterUtil, 0xABEC74D2, int, scePromoterUtilityGetState, int *state)
{
    if (!state)
        return -1;
    *state = 0;
    return 0;
}

EXPORT(ScePromoterUtil, 0x49B473F0, int, scePromoterUtilityGetResult, int *result)
{
    if (!result)
        return -1;
    *result = 0;
    return 0;
}

EXPORT(ScePromoterUtil, 0xBA9871E5, int, scePromoterUtilityCheckExist, const char *titleid,
       int *result)
{
    (void)titleid;
    if (!result)
        return -1;
    *result = 0;
    return 0;
}

DECLARE_LIBRARY(ScePromoterUtil, 0x31F237B6);
