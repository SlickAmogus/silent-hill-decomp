/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_flight_arcade.c - what the flight HUD does to the game (config key:
 * flight_gameplay, off by default, needs flight_hud). With it off nothing here
 * acts and the game is the PSX one.
 *
 *  - Flares: while a salvo jams the seekers, no hit from a non-boss monster
 *    lands on Harry. Checked in the game's two hit functions.
 *  - A non-boss monster that holds its lock for AF_ENEMY_LAUNCH_DELAY fires a
 *    homing missile at Harry; a flare near it takes it.
 *  - With the seeker locked, the light button (Circle by default) fires one
 *    of Harry's missiles at the seeker's target instead of the flashlight.
 *  - Hits are written into the fields the game's own damage handlers read,
 *    so hurt and death animations are the original ones.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"

#include <string.h>

#include "sh_log.h"
#include "pc_config.h"
#include "pc_flight_hud.h"
#include "pc_flight_missile.h"
#include "pc_flight_arcade.h"

int Pc_FlightArcade_Active(void)
{
    return g_PcConfig.flightHud != 0 && g_PcConfig.flightGameplay != 0;
}

int Pc_FlightArcade_ShieldsHarryFrom(const s_SubCharacter* attacker)
{
    return Pc_FlightArcade_Active() && Pc_FlightHud_JamActive() &&
           attacker != &g_SysWork.playerWork.player && !Pc_FlightHud_IsBoss(attacker->model.charaId);
}

void Pc_FlightArcade_Reset(void)
{
}

void Pc_FlightArcade_Update(float dt)
{
    (void)dt;
    if (!Pc_FlightArcade_Active())
        Pc_FlightArcade_Reset();
}
