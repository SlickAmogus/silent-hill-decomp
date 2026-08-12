/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_retroachievements.h - RetroAchievements (softcore) integration.
 *
 * Plays the user's real PSX disc, so the RA disc hash identifies the genuine
 * game and unlocks post to the user's real account. Softcore only: the port
 * ships quick save/load, debug controls, alternate cameras and gamemodes that
 * no hardcore ruleset could accept.
 *
 * All entry points are no-ops unless the feature is compiled in
 * (SH_RETROACHIEVEMENTS), enabled in config, and credentials are present.
 */
#ifndef PC_RETROACHIEVEMENTS_H
#define PC_RETROACHIEVEMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Boot: create the client, log in with the launcher-stored token, hash the
 * disc, and request the achievement set. Safe to call when disabled. */
void Pc_Ra_Init(void);

/* Per frame from MainLoop. Pumps queued server responses (all rc_client calls
 * stay on the main thread) and evaluates achievements while in live gameplay. */
void Pc_Ra_Update(void);

/* Flush pending unlocks and tear down. */
void Pc_Ra_Shutdown(void);

/* 1 once the achievement set is loaded and evaluating. */
int Pc_Ra_IsActive(void);

/* "12/40 (135 pts)" for HUD/console use; empty string when inactive. */
const char* Pc_Ra_StatusLine(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_RETROACHIEVEMENTS_H */
