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
    DLL_SECURITY_ERR_NO_PLUGIN_EXPORTS
} DllSecurityResult;

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
 * requirePluginExports: 1 for plugins/ (SH_Plugin_* contract), 0 for map
 * overlay / chara DLLs (different export set).
 * Returns DLL_SECURITY_OK when nothing objectionable was found; writes a
 * diagnostic to outReason if provided.
 */
DllSecurityResult DllSecurity_AuditPlugin(const char* path, int requirePluginExports, char* outReason, int maxReasonLen);

#ifdef __cplusplus
}
#endif

#endif /* DLL_SECURITY_H */
