/*
 * fmv_player.h - PC FMV playback for Silent Hill
 *
 * Plays pre-converted AVI (MJPG) files using libjpeg + OpenGL.
 * Based on REDRIVER2's VideoPlayer approach (BSD licensed).
 *
 * Usage: Extract STR files from disc with jPSXdec, convert to AVI (MJPG),
 * place in data/FMV/ directory next to the executable.
 */
#ifndef FMV_PLAYER_H
#define FMV_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Play an FMV by file table index. Returns 0 on success, -1 if file not found. */
int FMV_Play(int file_idx, int max_frames);

/* Initialize/shutdown GL resources (call once at startup/exit) */
void FMV_Init(void);
void FMV_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* FMV_PLAYER_H */
