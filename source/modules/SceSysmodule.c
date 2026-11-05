/*
 * SceSysmodule: module loading is a no-op here.
 *
 * All functionality lives in the single host executable, so every module is
 * always "loaded" and load/unload requests succeed.
 */

#include <psp2/sysmodule.h>

#include "module.h"

EXPORT(SceSysmodule, 0x79A0160A, int, sceSysmoduleLoadModule, SceSysmoduleModuleId id)
{
    (void)id;

    return 0;
}

EXPORT(SceSysmodule, 0x31D87805, int, sceSysmoduleUnloadModule, SceSysmoduleModuleId id)
{
    (void)id;

    return 0;
}

EXPORT(SceSysmodule, 0x53099B7A, int, sceSysmoduleIsLoaded, SceSysmoduleModuleId id)
{
    (void)id;

    return SCE_SYSMODULE_LOADED;
}

EXPORT(SceSysmodule, 0x2399BF45, int, sceSysmoduleLoadModuleInternal,
       SceSysmoduleInternalModuleId id)
{
    (void)id;

    return 0;
}

EXPORT(SceSysmodule, 0xC3C26339, int, sceSysmoduleLoadModuleInternalWithArg,
       SceSysmoduleInternalModuleId id, SceSize args, void *argp, const SceSysmoduleOpt *option)
{
    (void)id;
    (void)args;
    (void)argp;
    (void)option;

    return 0;
}

EXPORT(SceSysmodule, 0x09A4AF1D, int, sceSysmoduleIsLoadedInternal,
       SceSysmoduleInternalModuleId id)
{
    (void)id;

    return SCE_SYSMODULE_LOADED;
}

DECLARE_LIBRARY(SceSysmodule, 0x03fcf19d);
