/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the flight HUD does to the game (config key: flight_gameplay). */
#ifndef PC_FLIGHT_ARCADE_H
#define PC_FLIGHT_ARCADE_H

#include "pc_flight_missile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _SubCharacter;

int   Pc_FlightArcade_Active(void);
int   Pc_FlightArcade_ShieldsHarryFrom(const struct _SubCharacter* attacker);
int   Pc_FlightArcade_ClaimsLightButton(void);
int   Pc_FlightArcade_MapButton(int clicked, int held);
void  Pc_FlightArcade_Update(float dt);
void  Pc_FlightArcade_Reset(void);
/* World-OT smoke, once this frame's camera is set. */
void  Pc_FlightArcade_DrawWorld(void);

int   Pc_FlightArcade_Missiles(const AfMissile** out);
int   Pc_FlightArcade_Inbound(void);
float Pc_FlightArcade_InboundDist(void);
int   Pc_FlightArcade_Stock(void);
int   Pc_FlightArcade_StockMax(void);
float Pc_FlightArcade_Recharge01(void);
float Pc_FlightArcade_LaunchMsgT(void);
float Pc_FlightArcade_NoMslT(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_FLIGHT_ARCADE_H */
