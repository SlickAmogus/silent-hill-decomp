/*
 * sh_log.h - Simple logging for Silent Hill PC port
 */
#ifndef SH_LOG_H
#define SH_LOG_H

#include <stdio.h>

#define SH_LOG(fmt, ...) printf("[SH] " fmt "\n", ##__VA_ARGS__)
#define SH_WARN(fmt, ...) printf("[SH WARN] " fmt "\n", ##__VA_ARGS__)
#define SH_ERR(fmt, ...) fprintf(stderr, "[SH ERROR] " fmt "\n", ##__VA_ARGS__)

/* Debug log — writes to stdout (SilentHill.log via freopen), auto-opens on first use */
#ifdef __cplusplus
extern "C" {
#endif
extern FILE* g_ShDebugLog;
void SH_DebugLogInit(void);
#ifdef __cplusplus
}
#endif

#define SH_DBG(fmt, ...) do { \
    if (!g_ShDebugLog) SH_DebugLogInit(); \
    fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
    fflush(g_ShDebugLog); \
} while (0)

#endif /* SH_LOG_H */
