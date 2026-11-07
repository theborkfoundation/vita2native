#ifndef SCE_CTRL_SCRIPT_H
#define SCE_CTRL_SCRIPT_H

#include <stdint.h>

/*
 * Deterministic button script shared by both pad backends. Parsed from
 * VITA2NATIVE_CTRL_SCRIPT on first use (see SceCtrl_script.c); returns 0 when
 * no script is configured.
 */
uint32_t SceCtrl_script_buttons(void);

#endif
