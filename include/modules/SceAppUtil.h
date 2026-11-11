#ifndef SCE_APPUTIL_H
#define SCE_APPUTIL_H

#include <psp2/apputil.h>

/* Reads a save slot's param record from savedata0:. Returns 0 when the slot
   holds data and a negative value when it is empty. Used by SceCommonDialog to
   describe an existing slot in the save-data dialog result. */
int SceAppUtil_read_slot_param(unsigned int slot_id, SceAppUtilSaveDataSlotParam *param);

#endif
