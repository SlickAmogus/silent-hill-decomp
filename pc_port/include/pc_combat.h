#pragma once
#ifdef SH_PC_PORT

bool PC_PlayerManualReloadRequested(void);

/* Latched manual-reload request. Set on the bind's rising edge (sampled every
 * frame in Pc_ExtraActionsUpdate, weapon/ammo conditions checked at press time)
 * and cleared by whoever ACTS on it — clearing it in the query instead would
 * drop presses, since the free-aim FSM queries it every frame including on
 * frames it cannot start a reload. Self-expires after ~0.30s. */
extern int g_PcReloadRequest;

/* Rising-edge detection for a raw keyboard scancode. Returns true on the
 * frame the key transitions 0→1. Each scancode tracked separately. Used
 * for PC convenience hotkeys (M=map, I=inventory, R=reload) that live
 * outside the PSX controller mapping. */
bool PC_KeyboardKeyClicked(int sdlScancode);

/* OTS/TPS free-aim aim assist: snap the bullet aim point onto a nearby enemy's
 * body axis so a hit registers anywhere on the body (mouse) or with a magnetic
 * auto-aim window (controller). Returns the chosen g_SysWork.npcs[] index or
 * NO_VALUE, writing the aim point to *outAimPoint. See pc_combat.c. */
s32 Pc_AimAssistFind(const VECTOR3* camPos, const VECTOR3* camFwd, s32 aimRange, VECTOR3* outAimPoint);

#endif /* SH_PC_PORT */
