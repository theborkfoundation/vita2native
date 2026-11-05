/*
 * ScePower: battery and clock queries.
 *
 * This host has no Vita power management. Titles poll the battery during boot
 * and throttle quality settings from the clock queries, so the answers have to
 * be plausible rather than absent: a full, charging battery and the Vita's
 * default clocks.
 */

#include <psp2/power.h>

#include "module.h"

EXPORT(ScePower, 0x2085D15D, int, scePowerGetBatteryLifePercent, void)
{
    return 100;
}

EXPORT(ScePower, 0x8EFB3FA2, int, scePowerGetBatteryLifeTime, void)
{
    return 3600;
}

EXPORT(ScePower, 0x94F5A53F, int, scePowerGetBatteryRemainCapacity, void)
{
    return 2200;
}

EXPORT(ScePower, 0xFD18A0FF, int, scePowerGetBatteryFullCapacity, void)
{
    return 2200;
}

EXPORT(ScePower, 0x28E12023, int, scePowerGetBatteryTemp, void)
{
    return 25;
}

EXPORT(ScePower, 0x483CE86B, int, scePowerGetBatteryVolt, void)
{
    return 4200;
}

EXPORT(ScePower, 0xA88A2B65, int, scePowerGetBatterySOH, void)
{
    return 100;
}

EXPORT(ScePower, 0x008A54B9, int, scePowerGetBatteryCycleCount, void)
{
    return 1;
}

EXPORT(ScePower, 0x1E490401, int, scePowerIsBatteryCharging, void)
{
    return 1;
}

EXPORT(ScePower, 0x0AFD0D8B, int, scePowerIsBatteryExist, void)
{
    return 1;
}

EXPORT(ScePower, 0xD3075926, int, scePowerIsLowBattery, void)
{
    return 0;
}

EXPORT(ScePower, 0x87440F5E, int, scePowerIsPowerOnline, void)
{
    return 1;
}

EXPORT(ScePower, 0xB4432BC8, int, scePowerGetBatteryChargingStatus, void)
{
    return 1;
}

EXPORT(ScePower, 0xD541FF03, int, scePowerGetUsingWireless, void)
{
    return 0;
}

EXPORT(ScePower, 0x4D695C1F, int, scePowerSetUsingWireless, int enabled)
{
    (void)enabled;

    return 0;
}

EXPORT(ScePower, 0x525592E4, int, scePowerGetCaseTemp, void)
{
    return 30;
}

EXPORT(ScePower, 0x862AE1A6, int, scePowerGetBatteryElec, void)
{
    return 4200;
}

EXPORT(ScePower, 0xEA3E3715, int, scePowerGetBatteryRemainLevel, void)
{
    return 100;
}

EXPORT(ScePower, 0x1DB00F10, int, scePowerGetBatteryRemainMaxLevel, void)
{
    return 100;
}

EXPORT(ScePower, 0xABC6F88F, int, scePowerGetArmClockFrequency, void)
{
    return 444;
}

EXPORT(ScePower, 0x478FE6F5, int, scePowerGetBusClockFrequency, void)
{
    return 222;
}

EXPORT(ScePower, 0x1B04A1D6, int, scePowerGetGpuClockFrequency, void)
{
    return 222;
}

EXPORT(ScePower, 0x0A750DEE, int, scePowerGetGpuXbarClockFrequency, void)
{
    return 222;
}

EXPORT(ScePower, 0x74DB5AE5, int, scePowerSetArmClockFrequency, int freq)
{
    (void)freq;

    return 0;
}

EXPORT(ScePower, 0xB8D7B3FB, int, scePowerSetBusClockFrequency, int freq)
{
    (void)freq;

    return 0;
}

EXPORT(ScePower, 0x717DB06C, int, scePowerSetGpuClockFrequency, int freq)
{
    (void)freq;

    return 0;
}

EXPORT(ScePower, 0xA7739DBE, int, scePowerSetGpuXbarClockFrequency, int freq)
{
    (void)freq;

    return 0;
}

EXPORT(ScePower, 0x3CE187B6, int, scePowerSetConfigurationMode, int conf)
{
    (void)conf;

    return 0;
}

EXPORT(ScePower, 0xB11EF149, int, scePowerSetIdleTimerCount, int count)
{
    (void)count;

    return 0;
}

EXPORT(ScePower, 0x04B7766E, int, scePowerRegisterCallback, SceUID cbid)
{
    (void)cbid;

    return 0;
}

EXPORT(ScePower, 0xDFA8BAF8, int, scePowerUnregisterCallback, SceUID cbid)
{
    (void)cbid;

    return 0;
}

EXPORT(ScePower, 0xDB62C9CF, int, scePowerCancelRequest, void)
{
    return 0;
}

EXPORT(ScePower, 0x78A1A796, int, scePowerIsSuspendRequired, void)
{
    return 0;
}

EXPORT(ScePower, 0x0442D852, int, scePowerRequestColdReset, void)
{
    return 0;
}

EXPORT(ScePower, 0x2B7C7CF4, int, scePowerRequestStandby, void)
{
    return 0;
}

EXPORT(ScePower, 0xAC32C9CC, int, scePowerRequestSuspend, void)
{
    return 0;
}

EXPORT(ScePower, 0x3EA75C88, int, scePowerRequestDisplayOn, void)
{
    return 0;
}

EXPORT(ScePower, 0x160EB506, int, scePowerRequestDisplayOff, void)
{
    return 0;
}

EXPORT(ScePower, 0x27F3292C, int, scePowerBatteryUpdateInfo, void)
{
    return 0;
}

EXPORT(ScePower, 0xE3E45636, int, scePowerIsLowBatteryInhibitUpdateDownload, void)
{
    return 0;
}

EXPORT(ScePower, 0x9AA05A48, int, scePowerIsLowBatteryInhibitUpdateReboot, void)
{
    return 0;
}

EXPORT(ScePower, 0x7FA406DD, int, scePowerIsRequest, void)
{
    return 0;
}

DECLARE_LIBRARY(ScePower, 0x1082da7f);
