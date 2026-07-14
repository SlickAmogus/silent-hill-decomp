#ifndef PC_RANDO_H
#define PC_RANDO_H

/* Randomizer gamemode (config key `randomizer`, default 0).
 *
 * Every entry point below is a no-op unless the mode is on, so the vanilla game
 * is byte-identical with `randomizer = 0`. Nothing here mutates map-DLL data:
 * the mode installs its own copy of the active s_MapOverlayHdr (the same trick
 * pc_port/src/lang_text.c already uses for translated messages) and rewrites the
 * event / event-func / message / spawn tables inside that copy. The DLL's tables
 * stay pristine, so re-entering an area always re-randomizes from the original.
 *
 * See pc_port/docs/Randomizer_Mode.md.
 */

#include "bodyprog/map/map.h"

/* True when the mode is enabled AND a run is live. */
int  Pc_Rando_Active(void);

/* Boot, before MapRegistry_Init: forces the start map to map2_s04. */
void Pc_Rando_Init(void);

/* New Game: resets score, area count and the run RNG. */
void Pc_Rando_OnNewGame(void);

/* End of GameBoot_MapLoad, after the language patch has installed its own header
 * copy (we must copy from whatever is live, and we must be the last writer). */
void Pc_Rando_OnMapLoad(s32 mapIdx);

/* Once per frame from the main loop. */
void Pc_Rando_Update(void);

/* SysState_LoadArea_Update: a rewritten door's destination has no arrival record
 * in the source map's mapPoints, so supply the harvested one for the target. */
void Pc_Rando_ArrivalOverride(s_MapPoint2d* arrival, const s_EventData* evt);

/* Event_ItemTake: remap a world pickup to a healing item, a weapon the player
 * lacks, or ammo for a gun they carry. Stable for the life of one pickup. */
void Pc_Rando_RemapItemTake(s32* itemId, s32* itemCount, s32 eventFlagIdx, s32* mapMsgIdx);

/* Extra handgun rounds on top of the custom-map starting loadout. 0 when off. */
int  Pc_Rando_ExtraHandgunAmmo(void);

/* Score inputs. */
void Pc_Rando_OnEnemyKilled(void);
void Pc_Rando_OnDamageTaken(s32 amount); /* q19_12 HP actually deducted */

/* Overlay: fills "Score: N". Returns 0 when the panel should not be drawn. */
int  Pc_Rando_ScoreLine(char* buf, int cap);

#endif /* PC_RANDO_H */
