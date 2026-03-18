/*
 * dll_loader.h — Thin OS abstraction for loading shared libraries.
 *
 * Deliberately includes NO game headers so it can safely include
 * <windows.h> / <dlfcn.h> without conflicting with PSX decomp types.
 */
#ifndef DLL_LOADER_H
#define DLL_LOADER_H

/* Opaque handle to a loaded shared library. */
typedef void* DllHandle;

/* Load a shared library. Returns NULL on failure. */
DllHandle DllLoader_Open(const char* path);

/* Look up a symbol by name. Returns NULL if not found. */
void* DllLoader_GetSymbol(DllHandle handle, const char* name);

/* Unload a previously loaded library. Safe to call with NULL. */
void DllLoader_Close(DllHandle handle);

/* Return a human-readable error string for the last failure. */
const char* DllLoader_GetError(void);

#endif /* DLL_LOADER_H */
