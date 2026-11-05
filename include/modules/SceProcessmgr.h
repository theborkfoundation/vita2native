#ifndef VITA2NATIVE_SCE_PROCESSMGR_H
#define VITA2NATIVE_SCE_PROCESSMGR_H

int SceProcessmgr_init(void);
int SceProcessmgr_finish(void);

/* Invoked once the process is about to exit, before module cleanup. */
void SceProcessmgr_run_termination_callbacks(void);

#endif /* VITA2NATIVE_SCE_PROCESSMGR_H */
