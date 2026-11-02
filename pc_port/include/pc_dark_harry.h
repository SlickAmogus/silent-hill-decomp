/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_DARK_HARRY_H
#define PC_DARK_HARRY_H

#ifdef SH_PC_PORT

/* "Dark Harry" -- NOT in the original game.
 *
 * A translucent copy of Harry that walks out of the fog and comes for the
 * player, slowly, and does not stop for anything the controls can do. Reach
 * the player and he kills them. On the silenthill-conscious branch he is a
 * talking NPC: the console (AFKEGG) or a monster calling for help puts him in
 * the map, and the conversation engine (src/conscious/pc_conscious.c) is what
 * makes him stop and listen, run, or leave. AFKEGGSTOP sends him away.
 *
 * DELIBERATELY SELF-CONTAINED so it can be pulled out again. Everything lives
 * in pc_dark_harry.c; outside it there are only these:
 *   1. Pc_DarkHarry_Update()  - game_main.c, once per frame
 *   2. Pc_DarkHarry_Draw()    - game_sys_states.c, beside the player's own draw
 *   3. g_PcCharaGhostDraw     - bodyprog_80055028.c, the two chara addPrim sites
 *   4. dark_harry* keys       - pc_config.{h,c} + config.cfg
 *   5. AFKEGG / AFKEGGSTOP    - pc_console_cmd.c, through pc_conscious.c
 *   6. the conscious engine   - pc_conscious.c drives Hold / Run / Dismiss
 *
 * It borrows two things while he is on screen and hands both back on every
 * exit: the radio's second proximity voice (Sfx_RadioStaticLoop, which rises
 * from barely audible to full as he closes) and g_cfg_brightness (the scene
 * darkens as he closes). Neither is written anywhere else in here.
 *
 * He knows nothing about the conversation: he is told to hold still, to run,
 * or to go, and reports where he is. */

/* Once per frame from the main loop. Takes him away the moment the player is
 * not in normal gameplay control (a cutscene, a menu, a map change). */
void Pc_DarkHarry_Update(void);

/* Draw him, if he is here. Poses his own skeleton (still, walking or running)
 * and renders it see-through. Does nothing when he is not present. */
void Pc_DarkHarry_Draw(void);

/* 1 while he exists. */
int Pc_DarkHarry_Active(void);

/* Put him there right now. fromBehind: at the fog wall behind the player
 * (a monster called him) rather than ahead or at the configured landmark.
 * Returns NULL if he is now here, otherwise why not, in words fit to print. */
const char* Pc_DarkHarry_Spawn(int fromBehind);

/* Take him away, logging why. Safe to call at any time. */
void Pc_DarkHarry_Dismiss(const char* why);

/* 1 = stand still and face the player (listening); 0 = come on. */
void Pc_DarkHarry_Hold(int hold);

/* 1 = he runs from now on instead of walking. */
void Pc_DarkHarry_Run(int run);

/* Where he stands, in world units (q19_12), and how far from the player he
 * is in metres. Both zero when he is not here. */
void  Pc_DarkHarry_Pos(int* x, int* y, int* z);
float Pc_DarkHarry_Dist(void);

/* 1 exactly once after he reached the player and killed them. */
int Pc_DarkHarry_PollTaken(void);

#endif /* SH_PC_PORT */
#endif /* PC_DARK_HARRY_H */
