/*
 * sh_log.h - Simple logging for Silent Hill PC port
 */
#ifndef SH_LOG_H
#define SH_LOG_H

#include <stdio.h>

#define SH_LOG(fmt, ...) do { \
    printf("[SH] " fmt "\n", ##__VA_ARGS__); \
    if (g_ShOverlayPushLine) { \
        char _sh_log_buf[64]; \
        snprintf(_sh_log_buf, sizeof(_sh_log_buf), "[SH] " fmt, ##__VA_ARGS__); \
        g_ShOverlayPushLine(_sh_log_buf); \
    } \
} while (0)
#define SH_WARN(fmt, ...) do { \
    printf("[SH WARN] " fmt "\n", ##__VA_ARGS__); \
    if (g_ShOverlayPushLine) { \
        char _sh_warn_buf[64]; \
        snprintf(_sh_warn_buf, sizeof(_sh_warn_buf), "[SH WARN] " fmt, ##__VA_ARGS__); \
        g_ShOverlayPushLine(_sh_warn_buf); \
    } \
} while (0)
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
extern int   g_ShDebugEchoStdout;              /* set by main_pc.c after PcConfig_Load */
extern void (*g_ShOverlayPushLine)(const char*); /* NULL, or DbgOverlay_PushLine when show_console>=2 */
void SH_DebugLogInit(void);
#ifdef __cplusplus
}
#endif

/* SH_DBG short-circuits when g_ShDebugLog is NULL — the log file is only
 * opened (via SH_DebugLogInit) when config has enable_debug_log=1. That keeps
 * SilentHill.log from ever being created in release builds and turns
 * SH_DBG into a near-zero-cost branch when logging is disabled.
 *
 * No fflush per call — that adds ~5us per SH_DBG and combat hit-detection
 * during a knife swing emits 2000-3000 logs/frame, halving the framerate.
 * Rely on stdio's _IOLBF line buffering (set in SH_DebugLogInit). On
 * unhandled crash, our SetUnhandledExceptionFilter handler in main_pc.c
 * does the final fflush. */
#define SH_DBG(fmt, ...) do { \
    if (g_ShDebugLog) { \
        fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

/* Like SH_DBG but also prints to stdout when show_console is on — use for
 * lines you want to watch live in the console window during a session.
 * Echo path runs even with the log file closed, so user-facing console
 * output isn't silenced by enable_debug_log=0. */
#define SH_DBG_ECHO(fmt, ...) do { \
    if (g_ShDebugLog) { \
        fprintf(g_ShDebugLog, fmt "\n", ##__VA_ARGS__); \
        fflush(g_ShDebugLog); \
    } \
    if (g_ShDebugEchoStdout) { \
        printf(fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
    if (g_ShOverlayPushLine) { \
        char _sh_echo_buf[64]; \
        snprintf(_sh_echo_buf, sizeof(_sh_echo_buf), fmt, ##__VA_ARGS__); \
        g_ShOverlayPushLine(_sh_echo_buf); \
    } \
} while (0)

#endif /* SH_LOG_H */
