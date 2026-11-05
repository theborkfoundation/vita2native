/*
 * SceNpDrm: licence file naming. There is no PSN account or activation data
 * on the host, but the rif name is a pure function of the account id and the
 * file managers use it to probe for existing licences.
 */

#include <stdint.h>
#include <stdio.h>

#include <psp2/npdrm.h>

#include "module.h"

static int format_rif_name(char *rif_name, uint64_t aid)
{
    if (!rif_name)
        return -1;

    snprintf(rif_name, 17, "%016llX", (unsigned long long)aid);
    return 0;
}

EXPORT(SceNpDrm, 0xB8C5DA7C, int, _sceNpDrmGetRifName, char *rif_name, uint64_t aid)
{
    return format_rif_name(rif_name, aid);
}

EXPORT(SceNpDrm, 0xE935B0FC, int, _sceNpDrmGetFixedRifName, char *rif_name, uint64_t aid)
{
    return format_rif_name(rif_name, aid);
}

DECLARE_LIBRARY(SceNpDrm, 0xF2799B1B);
