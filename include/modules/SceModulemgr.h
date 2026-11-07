#ifndef SCE_MODULEMGR_H
#define SCE_MODULEMGR_H

/*
 * Loads every user module shipped inside the application directory
 * (`sce_module/` and `Media/Modules/`, for files ending in `.suprx`), resolves their
 * imports once they are all registered, and starts them.
 *
 * On a real Vita these modules are loaded by the application (or by the
 * firmware before it starts) and export libc/libm/pthread/IL2CPP, which the
 * eboot imports directly. Loading them before the main executable lets its
 * imports bind in a single pass.
 *
 * Returns the number of modules started, or -1 if nothing could be loaded.
 */
int SceModulemgr_load_app_modules(void);

/* Re-resolve the imports of every loaded module (including the main
   executable, if it was registered here). */
void SceModulemgr_repatch_modules(void);

/* Starts every loaded-but-not-started module in load order. */
int SceModulemgr_start_modules(void);

#endif
