/*
 * SceRegistryMgr: the firmware configuration registry.
 *
 * The host has no equivalent database. Reads fail with -1 after clearing the
 * caller's buffer so applications fall back to their built-in defaults.
 */

#include <string.h>

#include <psp2/types.h>

#include "module.h"

EXPORT(SceRegMgr, 0x0B98D646, int, sceRegMgrGetKeyBin, const char *category, const char *name,
       void *buffer, int size)
{
    (void)category;
    (void)name;

    if (buffer && size > 0)
        memset(buffer, 0, (size_t)size);

    return -1;
}

DECLARE_LIBRARY(SceRegMgr, 0xC436F916);
