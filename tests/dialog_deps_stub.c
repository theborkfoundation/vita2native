/*
 * Link stubs for dialog smoke tests.
 *
 * SceCommonDialog talks to SceSysmem (guest scratch) and SceAppUtil (save slot
 * params), but the tests link it without those modules. Reporting "no storage"
 * and "no slot data" keeps the dialog on its fallback path.
 */

#include <stddef.h>
#include <stdint.h>

/* The real types live in modules/SceSysmem.h and psp2/apputil.h, which pull in
   the GPU and filesystem headers these tests do not link against. */
typedef struct SceAppUtilSaveDataSlotParam SceAppUtilSaveDataSlotParam;

void *SceSysmem_alloc_guest_scratch(const char *name, uint32_t size)
{
    (void)name;
    (void)size;
    return NULL;
}

int SceAppUtil_read_slot_param(unsigned int slot_id, SceAppUtilSaveDataSlotParam *param)
{
    (void)slot_id;
    (void)param;
    return -1;
}
