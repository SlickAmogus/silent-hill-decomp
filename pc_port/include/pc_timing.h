#pragma once
#ifdef SH_PC_PORT

/* Note: this header relies on the project's `bool` typedef from
 * include/decomp/types.h (enum, NOT C99 _Bool). game.h pulls it in
 * transitively, so any TU including this should include "game.h" first.
 * Do NOT add <stdbool.h> here — it conflicts with the enum-typedef. */

/* Accumulator-based 30Hz tick check.
 *
 * Pass a per-call-site static accumulator (initialized to 0). Adds the
 * current frame's g_DeltaTime each call. Returns true (and decrements the
 * accumulator) when one full PSX game frame (1/30s) of game time has
 * elapsed; otherwise returns false.
 *
 * Why: PSX game logic ran at fixed 30Hz. Frame-counter state machines in
 * the game code (attack shift register, multi-tap detectors, etc.) assume
 * each call equals 1/30s. At PC framerates of 60-240Hz, those state
 * machines fire too quickly — the 4-frame attack hold detector triggers
 * in 17ms at 240fps instead of 133ms at 30fps, so even a quick tap
 * registers as a "hold". Wrap each such state machine in a guard like:
 *
 *     static int s_accum = 0;
 *     if (PC_Tick30HzReady(&s_accum)) { ... shift register ... }
 *
 * so it advances at the original PSX cadence regardless of render rate. */
bool PC_Tick30HzReady(int* accumulator_q12);

#endif /* SH_PC_PORT */
