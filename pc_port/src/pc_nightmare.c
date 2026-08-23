/* Nightmare mode toggle, configuration state, and survival mechanics. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_debug_draw.h"
#include "bodyprog/text/text_draw.h"
#include "main/fileinfo.h"
#include "map_registry.h"
#include "bodyprog/map/map.h"
#include "pc_config.h"
#include "pc_nightmare.h"
#include "sh_log.h"

/* --------------------------------------------------------------- mode state */

static int s_initDone; /* Pc_Nightmare_Init has run */
static int s_enabled;  /* config says the mode is on */
static int s_running;  /* a New Game has armed the run */

/* ------------------------------------------------------------- poison state */

static int    s_isPoisoned      = 0;
static q19_12 s_poisonTimer     = 0;
static q19_12 s_poisonHintTimer = 0;

int Pc_Nightmare_Enabled(void)
{
    return g_PcConfig.nightmare != 0;
}

int Pc_Nightmare_Active(void)
{
    return s_running && (g_PcConfig.nightmare != 0);
}

int Pc_Nightmare_IsPoisoned(void)
{
    return s_isPoisoned;
}

void Pc_Nightmare_SetPoisoned(int active)
{
    if (active)
    {
        if (!s_isPoisoned)
        {
            s_isPoisoned      = 1;
            s_poisonTimer     = 0;
            s_poisonHintTimer = Q12(6.0f); /* Show on-screen hint for 6 seconds */
            SH_LOG("[NIGHTMARE] Harry has been POISONED by Twinfeeler acid!");
        }
    }
    else
    {
        if (s_isPoisoned)
        {
            s_isPoisoned      = 0;
            s_poisonTimer     = 0;
            s_poisonHintTimer = 0;
            SH_LOG("[NIGHTMARE] Poison neutralized by First Aid Kit!");
        }
    }
}

void Pc_Nightmare_OnItemUsed(s32 itemId)
{
    /* First Aid Kit (or Ampoule) cures the poison; Health Drink only heals HP */
    if (itemId == InvItemId_FirstAidKit || itemId == InvItemId_Ampoule)
    {
        if (s_isPoisoned)
        {
            Pc_Nightmare_SetPoisoned(0);
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            SH_LOG("[NIGHTMARE] First Aid Kit cured Harry's poisoning.");
        }
    }
}

/* ------------------------------------------------------------------- toggle */

void Pc_Nightmare_Toggle(void)
{
    if (!s_initDone)
    {
        s_enabled  = g_PcConfig.nightmare != 0;
        s_initDone = 1;
    }

    s_enabled = !s_enabled;

    g_PcConfig.nightmare = s_enabled;
    PcConfig_SaveKeyValue("nightmare", s_enabled ? "1" : "0");

    if (!s_enabled)
    {
        s_running    = 0;
        s_isPoisoned = 0;
    }

    SH_LOG("[NIGHTMARE] %s", s_enabled ? "enabled" : "disabled");
}

/* --------------------------------------------------------------------- boot */

void Pc_Nightmare_Init(void)
{
    s_enabled  = g_PcConfig.nightmare != 0;
    s_initDone = 1;

    if (!s_enabled)
    {
        s_running    = 0;
        s_isPoisoned = 0;
        return;
    }

    SH_LOG("[NIGHTMARE] initialized");
}

void Pc_Nightmare_OnNewGame(void)
{
    if (!s_enabled)
    {
        s_running    = 0;
        s_isPoisoned = 0;
        return;
    }

    s_running    = 1;
    s_isPoisoned = 0;
    SH_LOG("[NIGHTMARE] run started");
}

static s_MapOverlayHdr s_hdr;

/* ----------------------------------------------------------------- map load */

void Pc_Nightmare_OnMapLoad(s32 mapIdx)
{
    /* Always clear active poison status on any map load / respawn / save game load */
    s_isPoisoned      = 0;
    s_poisonTimer     = 0;
    s_poisonHintTimer = 0;

    const char* mapName = MapRegistry_GetName((e_MapIdx)mapIdx);
    SH_LOG("[MAP_LOADER] Current Map ID: %d | Name: %s", mapIdx, mapName ? mapName : "UNKNOWN");

    if (!Pc_Nightmare_Active())
        return;

    if (g_pMapOverlayHeader == NULL)
        return;

    /* Copy current header and force Otherworld Night Ambient (field_16 = 2) & Rain weather (field_17 = 6) */
    s_hdr = *g_pMapOverlayHeader;
    s_hdr.field_16 = 2;
    s_hdr.field_17 = 6;
    g_pMapOverlayHeader = &s_hdr;

    SH_LOG("[NIGHTMARE] Applied map6_s00 dark ambient (field_16 = 2) & rain (field_17 = 6) to %s", mapName ? mapName : "map");
}

/* ---------------------------------------------------------------- per-frame */

void Pc_Nightmare_Update(void)
{
    if (!Pc_Nightmare_Active())
        return;

    /* Clear poison immediately if player is dead or not in active gameplay */
    if (g_SysWork.playerWork.player.health <= 0 ||
        g_SysWork.sysState == SysState_GameOver ||
        (g_GameWork.gameState != GameState_InGame && g_GameWork.gameState != GameState_MapEvent))
    {
        s_isPoisoned      = 0;
        s_poisonTimer     = 0;
        s_poisonHintTimer = 0;
        return;
    }

    /* Force field_16 = 2 and field_17 = 6 every frame, and call Gfx_MapEffectsAssign when map scripts reset environment */
    if (g_pMapOverlayHeader != NULL)
    {
        if (g_pMapOverlayHeader->field_16 != 2 || g_pMapOverlayHeader->field_17 != 6)
        {
            extern void Gfx_MapEffectsAssign(s_MapOverlayHdr* mapHdr);
            g_pMapOverlayHeader->field_16 = 2;
            g_pMapOverlayHeader->field_17 = 6;
            Gfx_MapEffectsAssign(g_pMapOverlayHeader);
        }
    }

    /* Poison Damage Over Time (DoT): 15 HP every 10 seconds, capped at 1.0 HP minimum */
    if (s_isPoisoned && g_SysWork.sysState == SysState_Gameplay && g_GameWork.gameState == GameState_InGame)
    {
        if (s_poisonHintTimer > 0)
        {
            s_poisonHintTimer -= g_DeltaTime;
        }

        s_poisonTimer += g_DeltaTime;
        if (s_poisonTimer >= Q12(10.0f))
        {
            s_poisonTimer = 0;
            if (g_SysWork.playerWork.player.health > Q12(1.0f))
            {
                g_SysWork.playerWork.player.health -= Q12(15.0f);
                if (g_SysWork.playerWork.player.health < Q12(1.0f))
                {
                    g_SysWork.playerWork.player.health = Q12(1.0f);
                }
                SH_LOG("[NIGHTMARE] Poison DoT tick! Harry HP: %d", g_SysWork.playerWork.player.health >> 12);
                Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
            }
        }
    }
}

void Pc_Nightmare_DrawPoisonFeedback(void)
{
    if (!s_isPoisoned)
        return;

    /* Do not draw in death, title, menu, or non-active gameplay */
    if (g_SysWork.playerWork.player.health <= 0 ||
        g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState != SysState_Gameplay)
    {
        return;
    }

    /* Real-Time Toxic Purple Pulsing Vignette */
    static q19_12 s_poisonPulseTime = 0;
    q19_12 dt = (g_DeltaTime > 0) ? g_DeltaTime : Q12(1.0f / 60.0f);
    s_poisonPulseTime += dt;

    /* 1.8 second breathing pulse cycle (0..4096 sine) */
    u16 angle = (u16)(((s_poisonPulseTime % Q12(1.8f)) * 4096) / Q12(1.8f));
    s32 pulse = (Math_Sin(angle) + 4096) / 2; /* 0..4096 */

    /* Toxic purple: Red = 60..130, Green = 0, Blue = 110..200 */
    u8 r = (u8)(50 + ((75 * pulse) >> 12));
    u8 g = 0;
    u8 b = (u8)(90 + ((110 * pulse) >> 12));

    extern void Pc_DrawColoredVignette(u8 r, u8 g, u8 b);
    Pc_DrawColoredVignette(r, g, b);

    /* On-screen banner while poison hint timer is active */
    if (s_poisonHintTimer > 0)
    {
        Text_Debug_PositionSet(22, 18);
        Text_Debug_Draw("! POISONED: USE A FIRST AID KIT !");
    }
}
