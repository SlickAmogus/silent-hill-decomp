/*
 * sh_log.h - Simple logging for Silent Hill PC port
 */
#ifndef SH_LOG_H
#define SH_LOG_H

#include <stdio.h>

#define SH_LOG(fmt, ...) printf("[SH] " fmt "\n", ##__VA_ARGS__)
#define SH_WARN(fmt, ...) printf("[SH WARN] " fmt "\n", ##__VA_ARGS__)
#define SH_ERR(fmt, ...) fprintf(stderr, "[SH ERROR] " fmt "\n", ##__VA_ARGS__)

/* Debug log — writes to SilentHill.log (auto-opens on first use).
 *
 * Gated at runtime by `g_ShDebugLogEnabled` (set from `enable_debug_log`
 * in config.cfg, default off). When disabled the macro short-circuits
 * to a no-op — call site formatting cost stays compiled in but the
 * fprintf+fflush hot-path doesn't run, so leaving SH_DBG calls in
 * place during normal play is cheap. Flip to 1 in config to dump. */
#ifdef __cplusplus
extern "C" {
#endif
extern FILE* g_ShDebugLog;
extern int   g_ShDebugLogEnabled;
void SH_DebugLogInit(void);
#ifdef __cplusplus
}
#endif

#define SH_DBG(fmt, ...) do { \
    if (g_ShDebugLogEnabled) { \
        if (!g_ShDebugLog) SH_DebugLogInit(); \
        fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
        fflush(g_ShDebugLog); \
    } \
} while (0)

/* HARRY_CHECK: PC port debug helper used in chara_init.c.  Not implemented;
 * stub to no-op so calls compile and link cleanly. */
#define HARRY_CHECK(tag) ((void)0)

#endif /* SH_LOG_H */
