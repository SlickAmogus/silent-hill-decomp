#ifndef PC_NIGHTMARE_H
#define PC_NIGHTMARE_H

#include "bodyprog/math/math.h"

/* True when the mode is enabled AND a run is live. */
int Pc_Nightmare_Active(void);

/* True when the mode is merely enabled (menu/highlight state, before a run). */
int Pc_Nightmare_Enabled(void);

/* Menu toggle: flips the mode and persists the config key. */
void Pc_Nightmare_Toggle(void);

/* Boot, before MapRegistry_Init: forces the global chara pool on so the
 * Otherworld models are resident in maps that never spawn them natively. */
void Pc_Nightmare_Init(void);

/* New Game: arms the run. Continue/Load leave an existing save alone. */
void Pc_Nightmare_OnNewGame(void);

/* End of GameBoot_MapLoad, after the language patch and the randomizer have
 * installed their own header copies (we must copy from whatever is live). */
void Pc_Nightmare_OnMapLoad(s32 mapIdx);

/* Once per frame from the main loop: holds the darkness overrides, which the
 * options screen / brightness slider can otherwise stomp. */
void Pc_Nightmare_Update(void);

/* Poison Status Effect API (Twinfeeler acid spit) */
int  Pc_Nightmare_IsPoisoned(void);
void Pc_Nightmare_SetPoisoned(int active);
void Pc_Nightmare_OnItemUsed(s32 itemId);
void Pc_Nightmare_DrawPoisonFeedback(void);

#endif /* PC_NIGHTMARE_H */
