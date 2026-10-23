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
/* Why no achievement set is loaded, for the player (never NULL). */
const char* Pc_Ra_UnavailableReason(void);
/* 1 when a set is loaded and Pc_Ra_SnapshotAchievements can return it. */
int Pc_Ra_SetLoaded(void);
/* 1 while login or the set load is still in flight, so the reason is not final. */
int Pc_Ra_IsPending(void);

/* One achievement, flattened out of rcheevos so UI TUs need no rcheevos
 * headers. Sizes follow RA's own limits with room to spare. */
typedef struct PcRaAch
{
    char     title[128];
    char     desc[256];
    char     badge[16];
    unsigned points;
    int      unlocked;
    long long unlockTime;   /* unix seconds; 0 when unknown or still locked */
} PcRaAch;

/* Fill `out` with the loaded set. Returns the count written, 0 when no set is
 * loaded. Defined in pc_retroachievements.c, which owns the rcheevos client. */
int  Pc_Ra_SnapshotAchievements(PcRaAch* out, int max);

/* Queue a badge PNG download. No-op for a name already requested this session. */
void Pc_Ra_RequestBadge(const char* badgeName);

/* Interactive sign-in, for targets with no launcher to authenticate first
 * (iOS). Exchanges a password for a connect token, stores the token in the
 * config, and continues into the normal disc-hash-and-load path. The password
 * is never stored. Asynchronous: returns 1 if the request was started, then
 * poll Pc_Ra_LoginPending() and read Pc_Ra_LoginResult() when it clears. */
int Pc_Ra_BeginPasswordLogin(const char* username, const char* password);

/* 1 while a sign-in request is in flight. */
int Pc_Ra_LoginPending(void);

/* Last sign-in outcome, for display: "Signed in as X", or the server's error. */
const char* Pc_Ra_LoginResult(void);

/* Forget the stored account and turn the feature off. */
void Pc_Ra_SignOut(void);

/* 1 when a username and token are on file (says nothing about whether the set
 * has loaded — that is Pc_Ra_IsActive). */
int Pc_Ra_IsSignedIn(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_RETROACHIEVEMENTS_H */
