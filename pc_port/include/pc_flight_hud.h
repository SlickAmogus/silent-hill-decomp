/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fighter-jet style HUD: persistent flight overlay, "MISSILE ALERT" when an
 * enemy locks on to Harry (the whole HUD turns red), and flares on L3+R3 (or the
 * touch FLARE button) that break every lock. Config key: flight_hud. */
#ifndef PC_FLIGHT_HUD_H
#define PC_FLIGHT_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game-loop tick (gameplay state only): lock tracking, flare input, flare
 * physics and the warning tones. */
void Pc_FlightHud_Update(void);

/* Post-capture GL draw, from DbgOverlay_Render. Self-contained GL state. */
void Pc_FlightHud_Draw(void);

/* 1 while an enemy holds a lock on Harry. Other HUD elements tint red on it. */
int Pc_FlightHud_AlertActive(void);

/* Queue a flare launch from a non-pad source (the touch FLARE button). */
void Pc_FlightHud_FlareRequest(void);

/* 1 when the HUD (and so the flare chord) is enabled. */
int Pc_FlightHud_Enabled(void);

/* A stick-click bind (Change Camera on R3, Quick Options on L3) must not also
 * fire when that click is half of the L3+R3 flare chord. Call every frame with
 * the bind's SDL controller button and its held level; returns 1 on the frame
 * the bind should act. For a stick button with the HUD on, that is the RELEASE
 * of a click during which the other stick was never pressed; otherwise it is
 * the plain press edge. `state` is caller-owned, zero-initialised. */
int Pc_FlightHud_StickBindEdge(int sdlButton, int held, unsigned char* state);

/* 1 when sdlButton is a stick click that Pc_FlightHud_StickBindEdge would defer. */
int Pc_FlightHud_StickBindDeferred(int sdlButton);

/* State flight_gameplay reads. */
int Pc_FlightHud_JamActive(void);
int Pc_FlightHud_LockState(int slot);
int Pc_FlightHud_SeekerLockedSlot(void);
int Pc_FlightHud_FlarePositions(float* xyz, int max);
int Pc_FlightHud_IsBoss(int charaId);
/* Change Target: move the seeker to the next enemy in front, by distance. */
void Pc_FlightHud_NextTarget(void);
/* The seeker's current pick, locked or still closing; -1 if none. */
int Pc_FlightHud_SeekerSlot(void);
int Pc_FlightHud_IsLiveEnemy(int slot);

#ifdef __cplusplus
}
#endif

#endif /* PC_FLIGHT_HUD_H */
