/*
 * sh_log.h - Simple logging for Silent Hill PC port
 */
#ifndef SH_LOG_H
#define SH_LOG_H

#include <stdio.h>

#define SH_LOG(fmt, ...) printf("[SH] " fmt "\n", ##__VA_ARGS__)
#define SH_WARN(fmt, ...) printf("[SH WARN] " fmt "\n", ##__VA_ARGS__)
#define SH_ERR(fmt, ...) fprintf(stderr, "[SH ERROR] " fmt "\n", ##__VA_ARGS__)

/* Debug log — writes to a fopen'd SilentHill.log handle (g_ShDebugLog).
 * When the show_console config option is on, g_ShDebugEchoStdout is set
 * and SH_DBG_ECHO additionally writes to stdout so the user sees the
 * line in the visible console window.  SH_DBG itself never echoes — it
 * only goes to the log file, so file-only diagnostic spam stays out of
 * the console. */
#ifdef __cplusplus
extern "C" {
#endif
extern FILE* g_ShDebugLog;
extern int   g_ShDebugEchoStdout;   /* set by main_pc.c after PcConfig_Load */
void SH_DebugLogInit(void);
#ifdef __cplusplus
}
#endif

/* No fflush per call — that adds ~5us per SH_DBG and combat hit-detection
 * during a knife swing emits 2000-3000 logs/frame, halving the framerate.
 * Rely on stdio's _IOLBF line buffering (set in SH_DebugLogInit). On
 * unhandled crash, our SetUnhandledExceptionFilter handler in main_pc.c
 * does the final fflush. */
#define SH_DBG(fmt, ...) do { \
    if (!g_ShDebugLog) SH_DebugLogInit(); \
    fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
} while (0)

/* Like SH_DBG but also prints to stdout when show_console is on — use for
 * lines you want to watch live in the console window during a session. */
#define SH_DBG_ECHO(fmt, ...) do { \
    if (!g_ShDebugLog) SH_DebugLogInit(); \
    fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
    fflush(g_ShDebugLog); \
    if (g_ShDebugEchoStdout) { \
        printf(fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while (0)

#endif /* SH_LOG_H */
