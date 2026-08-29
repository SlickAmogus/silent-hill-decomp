/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_N64_TRACE_H
#define PC_N64_TRACE_H

/* TEMPORARY: a bounded one-shot capture of PC ground truth for the N64 port.
 *
 * Four traces, all keyed to the same room so the two builds can be compared
 * stage by stage instead of inferred from screenshots:
 *   [GOLD]  per-vertex model -> screen for the first prims of the first mesh
 *   [DRAW]  the whole frame's prim census, including what the backface cull ate
 *   [VANG]  vwVectorToAngle's inputs and its two output angles
 *   [BONE]  Harry's per-bone world matrices at one frame
 *
 * It ARMS ITSELF: nothing prints until the map has been in gameplay with the
 * world actually drawn for N64_TRACE_SETTLE_FRAMES straight frames, so a boot
 * straight into a map cannot catch half-streamed geometry. It then runs for a
 * fixed number of slices and stops for good -- this is a capture, not a probe,
 * and it must never be able to sit in a build printing every frame.
 *
 * REMOVE THIS FILE AND ITS FOUR CALL SITES once the N64 framing is settled.
 * Search: N64Trace_. */

/* Once per frame, before the frame's draw flags are cleared. */
void N64Trace_Tick(void);

/* 1 while the capture window is open. */
int  N64Trace_Active(void);

/* One-shot gates, each true for exactly one slice/frame when it is that
 * trace's turn. Callers must not print unless the matching one returns 1. */
int  N64Trace_GoldWant(void);   /* first mesh of this frame only */
int  N64Trace_VangWant(void);   /* budgeted per slice */
int  N64Trace_BonesWant(void);  /* true once, for the whole session */

/* Every func_8005AC50 call adds its tally; the tick prints the frame total. */
void N64Trace_AddCensus(int prims, int passed, int tris, int depthRej,
                        int backfaceRej, int oobRej);

#endif /* PC_N64_TRACE_H */
