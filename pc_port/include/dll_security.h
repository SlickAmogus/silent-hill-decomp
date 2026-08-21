#ifndef DLL_SECURITY_H
#define DLL_SECURITY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DLL_SECURITY_OK = 0,
    DLL_SECURITY_ERR_FILE_OPEN,
    DLL_SECURITY_ERR_INVALID_PE,
    DLL_SECURITY_ERR_BLOCKED_IMPORT,
    DLL_SECURITY_ERR_NO_PLUGIN_EXPORTS,
    DLL_SECURITY_ERR_UNKNOWN_IMPORT,   /* map mode: import outside the edited-game-code fingerprint */
    DLL_SECURITY_ERR_BLOCKED_FUNCTION  /* flagrant kernel32 function in the import thunks */
} DllSecurityResult;

typedef enum
{
    /* Strict fingerprint for maps/*.dll: our MinGW-built map overlays import
     * exactly SilentHillPC.exe + KERNEL32.dll + msvcrt.dll (verified against
     * every shipped map DLL), so anything importing beyond that set -- or
     * resolving APIs dynamically via LoadLibrary/GetProcAddress, or carrying a
     * delay-load directory -- does not look like edited game code and is
     * refused (config allow_unrecognized_dlls=1 downgrades ONLY the
     * unknown-import case to a logged warning). */
    DLL_AUDIT_MAP = 0,
    /* Plugins are arbitrary code by design (opt-in via enable_plugins):
     * blacklist of network/shell import names + SH_Plugin_* export contract. */
    DLL_AUDIT_PLUGIN = 1
} DllAuditMode;

/*
 * Static PE sanity check run on a DLL before LoadLibrary.
 *
 * WHAT THIS IS: a lint, not a sandbox. It rejects files that are not valid
 * 64-bit DLLs, files whose IMPORT TABLE names known network/shell libraries,
 * and (when requirePluginExports is set) files that do not export the
 * SH_Plugin_* contract. That catches accidents and the laziest bad actors.
 *
 * WHAT THIS IS NOT: any DLL can LoadLibrary()+GetProcAddress() its way to
 * every blocked API at runtime, and DllMain runs arbitrary code the moment
 * LoadLibrary succeeds. NO static check makes an untrusted DLL safe -- the
 * user's trust in the mod's source is the real security boundary, which is
 * why the launcher shows the install warning. Never present this audit to
 * the user as proof a DLL is safe.
 *
 * mode: DLL_AUDIT_MAP for maps/*.dll (strict edited-game-code fingerprint),
 * DLL_AUDIT_PLUGIN for plugins/*.dll (blacklist + SH_Plugin_* contract).
 * Returns DLL_SECURITY_OK when nothing objectionable was found; writes a
 * diagnostic to outReason if provided.
 */
DllSecurityResult DllSecurity_AuditPlugin(const char* path, int mode /* DllAuditMode */, char* outReason, int maxReasonLen);

/* allow_unrecognized_dlls override; see dll_security.c. */
extern int g_DllAllowUnrecognized;

#ifdef __cplusplus
}
#endif

#endif /* DLL_SECURITY_H */
