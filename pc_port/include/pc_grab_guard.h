/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_GRAB_GUARD_H
#define PC_GRAB_GUARD_H

/* Grab attacks from monsters the current map never hosts.
 *
 * A monster's grab drives the player through Harry reaction animations that
 * live in the map's OWN keyframe bank (HB_M<map>.ANM), indexed by rows in the
 * overlay header's field_38 table. Each map carries only the rows its native
 * monsters need — map0_s00 has the leg-grab pair (0xEC/0xF6) because Grey
 * Children live there, map0_s01 has neither.
 *
 * When the console SPAWN command or the global chara pool places a monster in a
 * map with no row for its grab, func_8007FB94 finds no match, so it leaves
 * controlState at 0 AND leaves D_800AF220 pointing at whatever row was looked
 * up last. The release state's exit gate then waits for
 * keyframeIdx == field_38[D_800AF220].keyframeIdx_6 — a keyframe belonging to
 * an unrelated animation — which never arrives, and the player is held for the
 * rest of the session.
 *
 * Supplying the row without the keyframes is NOT a fix: 36 maps used to share
 * map0_s00's table by a linker accident, and the result was Harry "shaking"
 * through animations whose indices meant something else in his own bank.
 */

/** @brief Can this grab actually be animated in the current map?
 *
 * Checks that field_38 carries rows for BOTH the grab animation and the release
 * animation it transitions to — a missing release row wedges just as hard as a
 * missing grab row, and only the release row is on the freezing path.
 *
 * @param attackReceived `s_SubCharacter::attackReceived` (45/56 torso, 49 legs,
 *                       54 Romper pin, 66 neck). Anything else returns 1.
 *                       The pin checks four links per side, not two: its *Start
 *                       state has no mash timer, so a missing row wedges before
 *                       the player can struggle, and its garbage collision
 *                       offset drops them through the floor (bounded at
 *                       player_control.c's D_800AF1FC read).
 * @param isFrontal      Non-zero when the grab comes from the front.
 * @return Non-zero when the grab is playable and stock behaviour should stand.
 */
int Pc_GrabGuard_GrabIsPlayable(int attackReceived, int isFrontal);

/** @brief Can the ordinary torso reaction be animated in the current map?
 *
 * The substitute for a refused grab needs its own check: DamageTorso goes
 * through the same table (ANIM_STATUS(105, ...)), and map1_s04 ships a table
 * with only the sentinel row.
 */
int Pc_GrabGuard_DamageFallbackIsPlayable(int isFrontal);

#endif /* PC_GRAB_GUARD_H */
