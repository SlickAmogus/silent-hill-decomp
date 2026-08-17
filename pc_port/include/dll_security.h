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
 * Statically audits a DLL on disk before LoadLibrary to detect malicious imports
 * and ensure it implements the Silent Hill PC Port plugin contract.
 *
 * Returns DLL_SECURITY_OK if valid and safe.
 * Writes diagnostic message to outReason if provided.
 */
DllSecurityResult DllSecurity_AuditPlugin(const char* path, char* outReason, int maxReasonLen);

#ifdef __cplusplus
}
#endif

#endif /* DLL_SECURITY_H */
