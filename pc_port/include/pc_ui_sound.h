/*
 * pc_ui_sound.h - one-shot WAV cues for port UI (achievement toast + browser).
 *
 * These play OUTSIDE the 24 emulated SPU voices, so a cue never steals a game
 * channel. OpenAL is used when the SPU renderer left a context current;
 * otherwise a plain SDL audio device is opened as a fallback, because the
 * authentic/high_precision/modern SPU renderers never initialise OpenAL at all
 * and alGen* would just raise AL_INVALID_OPERATION.
 */
#ifndef PC_UI_SOUND_H
#define PC_UI_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcUiSound PcUiSound;

/* Load a 16-bit PCM mono/stereo WAV. Returns NULL (and logs once) on any
 * problem; every other call tolerates NULL, so callers need no error path. */
PcUiSound* PcUiSound_Load(const char* path);

/* Restart the cue from the beginning. No-op on NULL. */
void PcUiSound_Play(PcUiSound* snd);

#ifdef __cplusplus
}
#endif

#endif /* PC_UI_SOUND_H */
