/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_game.c - the half of the online client that knows what g_SysWork is.
 *
 * sh_net_client.c is deliberately engine-free; this file is the only place the
 * network layer reads the game. It runs once a frame from MainLoop and does
 * three things: publish where this player is, pull the worker's findings into
 * the game-thread copy, and notice the two events worth telling the server
 * about (a map change and a death).
 *
 * Everything here is a READ of game state except the death edge, which owns a
 * single static of its own. Nothing in this file can change how the game plays
 * — that is the point, and it is what keeps the online branch from drifting
 * away from the single-player one.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"

#include "sh_net.h"
#include "sh_net_memo.h"
#include "sh_net_chat.h"
#include "sh_net_session.h"
#include "sh_net_internal.h"
#include "sh_net_platform.h" /* ShNetPlat_Millis, for the roster re-request throttle */
#include "pc_discord.h" /* Pc_MapAreaName, for Steam rich presence */
#include "pc_config.h"
#include "pc_playas.h"
#include "sh_log.h"

/* Above this the player reads as "moving fast" to a watching ghost. Q19.12,
 * and comfortably under Harry's run speed so a jog still counts. */
#define SHNET_RUN_SPEED Q12(0.035f)

static int s_lastMap      = -1;
static int s_deathLatched = 0;
static int s_initDone     = 0;

/* Throttle for the "a ghost is still unnamed" roster re-request below. */
#define SHNET_NAME_REQ_MS 3000u
static unsigned int s_lastNameReqMs = 0;

/* Recompute every frame rather than caching: the player can change character
 * mid-session (PLAYAS), and a ghost wearing the wrong body is worse than the
 * three instructions this costs. */
static int ShNetG_LocalChara(void)
{
    int id = Pc_PlayAs_SkinCharaId();
    if (id < 0 || id > 255)
    {
        id = 0;
    }
    return id;
}

static int ShNetG_BuildFlags(void)
{
    int flags = 0;

    if (g_SysWork.playerWork.player.health > Q12(0.0f))
    {
        flags |= SHNET_PF_ALIVE;
    }
    if (g_SysWork.field_2388.isFlashlightOn_15)
    {
        flags |= SHNET_PF_FLASHLIGHT;
    }
    if (g_SysWork.playerWork.player.moveSpeed > SHNET_RUN_SPEED)
    {
        flags |= SHNET_PF_RUNNING;
    }
    /* The border state is the game's own "a scene is playing" signal, and it
     * is what the ghost renderer keys on: during a cutscene the player is
     * standing somewhere the camera put them, not somewhere they walked. */
    if (g_SysWork.cutsceneBorderState != 0)
    {
        flags |= SHNET_PF_CUTSCENE;
    }
    if (g_SysWork.sysState != SysState_Gameplay)
    {
        flags |= SHNET_PF_MENU;
    }
    return flags;
}

void ShNet_OnMapChanged(int mapIdx)
{
    if (mapIdx == s_lastMap)
    {
        return;
    }
    s_lastMap      = mapIdx;
    s_deathLatched = 0;
    ShNet_RequestMemos();
    ShNet_RequestRoster();
    /* What friends see on the Steam friends list. The area name is the same
     * one the Discord presence line and the player list use. */
    ShSession_PublishPresence(Pc_MapAreaName(mapIdx), mapIdx);
}

void ShNet_GameTick(void)
{
    int   inWorld;
    int   mapIdx;
    int   flags;
    short health;

    if (!s_initDone)
    {
        /* Deferred to the first frame rather than done in main_pc.c's init:
         * the config is fully parsed by then, and starting a thread before
         * SDL is up is the kind of ordering bug that only shows on someone
         * else's machine. */
        s_initDone = 1;
        ShNet_Init();
    }

    if (!ShNet_Enabled())
    {
        return;
    }

    ShNetMemo_Tick();
    ShNetChat_Tick();

    inWorld = (g_GameWork.gameState == GameState_InGame) &&
              (g_SavegamePtr != NULL) &&
              !(g_SysWork.sysFlags & SysFlag_DemoActive);

    if (!inWorld)
    {
        ShNet_PublishLocal(0, s_lastMap < 0 ? 0 : s_lastMap, ShNetG_LocalChara(), 0,
                           0, 0, 0, 0, 0, 0, 0);
        ShSession_PublishPresence("In the menus", -1);
        ShNet_PumpToGameThread();
        return;
    }

    mapIdx = (int)g_SavegamePtr->mapIdx;
    if (mapIdx != s_lastMap)
    {
        ShNet_OnMapChanged(mapIdx);
    }

    flags = ShNetG_BuildFlags();

    {
        q19_12 hp = g_SysWork.playerWork.player.health;
        int    pct;
        if (hp < 0)
        {
            hp = 0;
        }
        pct = (int)(hp >> 12);
        if (pct > 999)
        {
            pct = 999;
        }
        health = (short)pct;
    }

    ShNet_PublishLocal(1, mapIdx, ShNetG_LocalChara(), flags,
                       (int)g_SysWork.playerWork.player.position.vx,
                       (int)g_SysWork.playerWork.player.position.vy,
                       (int)g_SysWork.playerWork.player.position.vz,
                       (short)g_SysWork.playerWork.player.rotation.vy,
                       health,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.status,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.keyframeIdx);

    /* Death marker, once per death. Latched rather than edge-detected on the
     * health value alone: health sits at zero for the whole death animation
     * and the game-over screen, which would otherwise place a marker every
     * frame for several seconds. Cleared when the player is alive again,
     * which covers both continue and load. */
    if (g_PcConfig.onlineDeaths)
    {
        const int dead = (g_SysWork.playerWork.player.health <= Q12(0.0f)) ||
                         (g_SysWork.sysState == SysState_GameOver);
        if (dead && !s_deathLatched)
        {
            s_deathLatched = 1;
            ShNet_ReportDeath((int)g_SysWork.playerWork.player.position.vx,
                              (int)g_SysWork.playerWork.player.position.vy,
                              (int)g_SysWork.playerWork.player.position.vz,
                              (short)g_SysWork.playerWork.player.rotation.vy);
            SH_DBG("[NET] death marker placed on map %d", mapIdx);
        }
        else if (!dead)
        {
            s_deathLatched = 0;
        }
    }

    ShNet_PumpToGameThread();

    /* Ghost names come from the roster, which is fetched once on connect and on
     * each map change. A player who reaches this map after that shows as "?"
     * (the placeholder set when a ghost is first seen), which also leaves the
     * nameplate blank. Ask for the roster again -- throttled -- whenever a ghost
     * is still unnamed, so late arrivals get named without polling the roster
     * every frame. */
    {
        int n = ShNet_GhostCount();
        int i;
        int unnamed = 0;
        for (i = 0; i < n; i++)
        {
            const ShNetGhost* g = ShNet_Ghost(i);
            if (g && (g->name[0] == '\0' || g->name[0] == '?'))
            {
                unnamed = 1;
                break;
            }
        }
        if (unnamed)
        {
            unsigned int now = ShNetPlat_Millis();
            if (now - s_lastNameReqMs >= SHNET_NAME_REQ_MS)
            {
                s_lastNameReqMs = now;
                ShNet_RequestRoster();
            }
        }
    }
}
