/*
 * SceShellUtil: the handful of system-shell hooks titles call.
 *
 * There is no shell process here, so locking/unlocking the shell is a no-op
 * and launching another app is reported as unavailable. Registering a handler
 * succeeds: titles then simply never receive events, which is exactly what
 * happens when the shell has nothing to report.
 */

#include <psp2/shellutil.h>

#include "module.h"

EXPORT(SceShellUtil, 0xE8AD11EC, int, sceShellUtilInitEvents, int num_events)
{
    (void)num_events;

    return 0;
}

EXPORT(SceShellUtil, 0xA9537233, int, sceShellUtilLock, SceShellUtilLockType type)
{
    (void)type;

    return 0;
}

EXPORT(SceShellUtil, 0x21A6CF54, int, sceShellUtilUnlock, SceShellUtilLockType type)
{
    (void)type;

    return 0;
}

EXPORT(SceShellUtil, 0x7B5EDFE7, int, sceShellUtilRegisterEventHandler,
       SceShellUtilEventHandler *handler, void *user_data)
{
    (void)handler;
    (void)user_data;

    return 0;
}

EXPORT(SceShellUtil, 0xFD521712, int, sceShellUtilRequestLaunchApp,
       SceShellUtilLaunchAppParam *param)
{
    (void)param;

    return -1;
}

EXPORT(SceShellUtilLaunchApp, 0x5372EEDB, int, sceShellUtilLaunchAppRequestLaunchApp,
       SceShellUtilLaunchAppParam *param)
{
    (void)param;

    return -1;
}

DECLARE_LIBRARY(SceShellUtil, 0xd2b1c8ae);
DECLARE_LIBRARY(SceShellUtilLaunchApp, 0xa0fd0261);
