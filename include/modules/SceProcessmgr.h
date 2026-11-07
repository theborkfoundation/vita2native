#ifndef VITA2NATIVE_SCE_PROCESSMGR_H
#define VITA2NATIVE_SCE_PROCESSMGR_H

/* Value of the SceLibcParam:__sce_libcparam variable. */
extern void *g_sce_libc_param;

int SceProcessmgr_init(void);
int SceProcessmgr_finish(void);

/* Invoked once the process is about to exit, before module cleanup. */
void SceProcessmgr_run_termination_callbacks(void);

#endif /* VITA2NATIVE_SCE_PROCESSMGR_H */
