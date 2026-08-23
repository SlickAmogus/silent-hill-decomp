#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "pc_config.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "map_registry.h"

#define SH_LOG(fmt, ...) printf("[NIGHTMARE] " fmt "\n", ##__VA_ARGS__)

#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#include "main/fileinfo.h"

static int s_running = 0;

static void ApplyShadowStalkerModelOverrides(void)
{
    /* In Nightmare Mode, redirect Grey Children & Mumblers to the Shadow Stalker assets (CLD2) */
    CHARA_FILE_INFOS[Chara_GreyChild].modelFileIdx       = FILE_CHARA_CLD2_ILM;
    CHARA_FILE_INFOS[Chara_GreyChild].textureFileIdx     = FILE_CHARA_CLD2_TIM;
    CHARA_FILE_INFOS[Chara_GreyChild].materialBlendMode = BlendMode_Subtractive;

    CHARA_FILE_INFOS[Chara_Mumbler].modelFileIdx         = FILE_CHARA_CLD2_ILM;
    CHARA_FILE_INFOS[Chara_Mumbler].textureFileIdx       = FILE_CHARA_CLD2_TIM;
    CHARA_FILE_INFOS[Chara_Mumbler].materialBlendMode   = BlendMode_Subtractive;
}

static void Patch_HideHealthStatus(void)
{
#ifdef _WIN32
    void* pFunc = (void*)GetProcAddress(GetModuleHandleA(NULL), "Gfx_Inventory_HealthStatusDraw");
    if (pFunc)
    {
        DWORD oldProtect;
        if (VirtualProtect(pFunc, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *(unsigned char*)pFunc = 0xC3; /* x86/x64 RET instruction to suppress health face/bar */
            VirtualProtect(pFunc, 1, oldProtect, &oldProtect);
            SH_LOG("[NIGHTMARE_PLUGIN] Successfully patched Gfx_Inventory_HealthStatusDraw to RET (health indicator hidden).");
        }
    }
#endif
}

PLUGIN_EXPORT const char* SH_Plugin_GetName(void)
{
    return "Nightmare Mode Overhaul";
}

PLUGIN_EXPORT s32 SH_Plugin_GetApiVersion(void)
{
    return 1;
}

PLUGIN_EXPORT void SH_Plugin_Init(void)
{
    SH_LOG("[NIGHTMARE_PLUGIN] Initialized Nightmare Overhaul Plugin.");
    g_PcConfig.nightmare = 1;
    g_PcConfig.revampedController = 1;
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();
}

PLUGIN_EXPORT void SH_Plugin_OnNewGame(void)
{
    g_PcConfig.nightmare = 1;
    g_PcConfig.revampedController = 1;
    s_running = 1;
    SH_LOG("[NIGHTMARE_PLUGIN] New Game started in Nightmare Mode.");
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();
}

PLUGIN_EXPORT void SH_Plugin_OnMapLoad(s32 mapIdx)
{
    g_PcConfig.nightmare = 1;
    ApplyShadowStalkerModelOverrides();
    Patch_HideHealthStatus();

    const char* mapName = MapRegistry_GetName((e_MapIdx)mapIdx);
    SH_LOG("[NIGHTMARE_PLUGIN] Map loaded: %d (%s)", mapIdx, mapName ? mapName : "map");

    if (g_pMapOverlayHeader != NULL)
    {
        g_pMapOverlayHeader->field_16 = 2;
        g_pMapOverlayHeader->field_17 = 6;
        if (g_pMapOverlayHeader->enviromentSet != NULL)
        {
            g_pMapOverlayHeader->enviromentSet(6, 2);
        }
        SH_LOG("[NIGHTMARE_PLUGIN] Applied dark ambient (field_16 = 2) & rain (field_17 = 6)");
    }
}

static s_CharaModel* s_lastChildModel = NULL;
static q19_12        s_prevHp         = 0;

PLUGIN_EXPORT void SH_Plugin_OnUpdate(void)
{
    /* Always ensure nightmare mode is active when plugin is present */
    g_PcConfig.nightmare = 1;

    /* Patch health indicator in inventory */
    static int s_patchedHealth = 0;
    if (!s_patchedHealth)
    {
        s_patchedHealth = 1;
        Patch_HideHealthStatus();
    }

    /* Enforce darkness & rain while in active gameplay across all maps */
    if (g_SysWork.sysState == SysState_Gameplay &&
        (g_GameWork.gameState == GameState_InGame || g_GameWork.gameState == GameState_MapEvent) &&
        g_pMapOverlayHeader != NULL)
    {
        if (g_pMapOverlayHeader->field_16 != 2 || g_pMapOverlayHeader->field_17 != 6)
        {
            g_pMapOverlayHeader->field_16 = 2;
            g_pMapOverlayHeader->field_17 = 6;
            if (g_pMapOverlayHeader->enviromentSet != NULL)
            {
                g_pMapOverlayHeader->enviromentSet(6, 2);
            }
        }

        /* Nightmare Mode: Grey Children / Mumblers / Stalkers are rendered as translucent shadow stalkers */
        if (WorldGfx_IsCharaModelPresent(Chara_GreyChild))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_GreyChild];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_GreyChild, true);
                WorldGfx_CharaModelMaterialSet(Chara_GreyChild, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else if (WorldGfx_IsCharaModelPresent(Chara_Mumbler))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_Mumbler];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_Mumbler, true);
                WorldGfx_CharaModelMaterialSet(Chara_Mumbler, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else if (WorldGfx_IsCharaModelPresent(Chara_Stalker))
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[Chara_Stalker];
            if (m != NULL && m != s_lastChildModel)
            {
                WorldGfx_CharaModelTransparentSet(Chara_Stalker, true);
                WorldGfx_CharaModelMaterialSet(Chara_Stalker, BlendMode_Subtractive);
                s_lastChildModel = m;
            }
        }
        else
        {
            s_lastChildModel = NULL;
        }

        /* 2x Lethal Combat Damage in any binary build:
         * Double incoming damage buffer and monitor direct health loss */
        if (g_SysWork.playerWork.player.damage.amount > 0)
        {
            g_SysWork.playerWork.player.damage.amount *= 2;
        }

        q19_12 curHp = g_SysWork.playerWork.player.health;
        if (s_prevHp > curHp && s_prevHp > 0 && curHp > 0)
        {
            q19_12 damageDelta = s_prevHp - curHp;
            curHp -= damageDelta; /* Extra 1x delta -> total 2x damage */
            if (curHp < 0) curHp = 0;
            g_SysWork.playerWork.player.health = curHp;
        }
        s_prevHp = g_SysWork.playerWork.player.health;
    }
    else
    {
        s_prevHp = g_SysWork.playerWork.player.health;
    }

    /* Live Real-Time World Simulation:
     * While in Inventory or Map screens, continue running
     * NPC AI updates so enemies advance and stalk Harry in real time. */
    if (g_PcConfig.liveInventory)
    {
        if (g_GameWork.gameState == GameState_InventoryScreen ||
            g_GameWork.gameState == GameState_PaperMapScreen)
        {
            extern void Game_NpcUpdate(void);
            Game_NpcUpdate();
        }
    }
}

PLUGIN_EXPORT void SH_Plugin_OnPlayerDamage(s32* damage)
{
    if (!g_PcConfig.nightmare)
        return;

    if (damage && *damage > 0)
    {
        /* 2x damage multiplier in Nightmare Mode (heavy punishing hit, fair challenge) */
        *damage *= 2;
        if (*damage < Q12(10.0f))
        {
            *damage = Q12(10.0f);
        }
    }
}

PLUGIN_EXPORT int SH_Plugin_ShouldHideHealth(void)
{
    if (!g_PcConfig.nightmare)
        return 0;

    /* Hide health indicator in pause/inventory screens */
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_OverrideWeather(s32* ambient, s32* rain)
{
    if (!g_PcConfig.nightmare)
        return 0;

    if (ambient) *ambient = 2; // Dark Otherworld ambient
    if (rain)    *rain    = 6; // Heavy rain
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_IsNightmarePlugin(void)
{
    return 1;
}

PLUGIN_EXPORT int SH_Plugin_IsLiveInventory(void)
{
    if (!g_PcConfig.nightmare)
        return 0;

    return (g_PcConfig.liveInventory != 0);
}

/* Standalone PSX Vignette Renderer for standalone beta & dev builds */
static POLY_G4 s_vignetteQuads[2][4];
static DR_MODE s_vignetteDrMode[2];
static int     s_vignetteStaticsInited = 0;

static void Plugin_DrawDirectVignette(u8 r, u8 g, u8 b, u8 alpha)
{
    if (alpha == 0) return;
    int buf = g_ActiveBufferIdx;
    if (!s_vignetteStaticsInited)
    {
        s_vignetteStaticsInited = 1;
        for (int b_idx = 0; b_idx < 2; b_idx++)
        {
            SetDrawMode(&s_vignetteDrMode[b_idx], 0, 0, GetTPage(0, 1, 0, 0), 0);
            for (int q = 0; q < 4; q++)
            {
                SetPolyG4(&s_vignetteQuads[b_idx][q]);
                SetSemiTrans(&s_vignetteQuads[b_idx][q], 1);
            }
        }
    }

    short halfH = 120;
    float aspect = 1.333333f;
    if (g_PcConfig.windowWidth > 0 && g_PcConfig.windowHeight > 0)
    {
        aspect = (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight;
    }
    if (aspect < 1.333333f) aspect = 1.333333f;
    short halfW = (short)(120.0f * aspect + 16.0f);

    short borderX = (short)(28.0f * (aspect / 1.333333f));
    short borderY = 30;

    short xL = -halfW;
    short xR =  halfW;
    short yT = -halfH;
    short yB =  halfH;

    short inXL = xL + borderX;
    short inXR = xR - borderX;
    short inYT = yT + borderY;
    short inYB = yB - borderY;

    u8 oR = (u8)((r * alpha) / 255);
    u8 oG = (u8)((g * alpha) / 255);
    u8 oB = (u8)((b * alpha) / 255);

    /* Top Quad */
    POLY_G4* q0 = &s_vignetteQuads[buf][0];
    q0->x0 = xL;   q0->y0 = yT;   q0->r0 = oR; q0->g0 = oG; q0->b0 = oB;
    q0->x1 = xR;   q0->y1 = yT;   q0->r1 = oR; q0->g1 = oG; q0->b1 = oB;
    q0->x2 = inXL; q0->y2 = inYT; q0->r2 = 0;  q0->g2 = 0;  q0->b2 = 0;
    q0->x3 = inXR; q0->y3 = inYT; q0->r3 = 0;  q0->g3 = 0;  q0->b3 = 0;

    /* Bottom Quad */
    POLY_G4* q1 = &s_vignetteQuads[buf][1];
    q1->x0 = inXL; q1->y0 = inYB; q1->r0 = 0;  q1->g0 = 0;  q1->b0 = 0;
    q1->x1 = inXR; q1->y1 = inYB; q1->r1 = 0;  q1->g1 = 0;  q1->b1 = 0;
    q1->x2 = xL;   q1->y2 = yB;   q1->r2 = oR; q1->g2 = oG; q1->b2 = oB;
    q1->x3 = xR;   q1->y3 = yB;   q1->r3 = oR; q1->g3 = oG; q1->b3 = oB;

    /* Left Quad */
    POLY_G4* q2 = &s_vignetteQuads[buf][2];
    q2->x0 = xL;   q2->y0 = yT;   q2->r0 = oR; q2->g0 = oG; q2->b0 = oB;
    q2->x1 = inXL; q2->y1 = inYT; q2->r1 = 0;  q2->g1 = 0;  q2->b1 = 0;
    q2->x2 = xL;   q2->y2 = yB;   q2->r2 = oR; q2->g2 = oG; q2->b2 = oB;
    q2->x3 = inXL; q2->y3 = inYB; q2->r3 = 0;  q2->g3 = 0;  q2->b3 = 0;

    /* Right Quad */
    POLY_G4* q3 = &s_vignetteQuads[buf][3];
    q3->x0 = inXR; q3->y0 = inYT; q3->r0 = 0;  q3->g0 = 0;  q3->b0 = 0;
    q3->x1 = xR;   q3->y1 = yT;   q3->r1 = oR; q3->g1 = oG; q3->b1 = oB;
    q3->x2 = inXR; q3->y2 = inYB; q3->r2 = 0;  q3->g2 = 0;  q3->b2 = 0;
    q3->x3 = xR;   q3->y3 = yB;   q3->r3 = oR; q3->g3 = oG; q3->b3 = oB;

    void* ot = (void*)&g_OtTags0[buf][5];
    AddPrim(ot, &s_vignetteDrMode[buf]);
    AddPrim(ot, q0);
    AddPrim(ot, q1);
    AddPrim(ot, q2);
    AddPrim(ot, q3);
}

static void Plugin_RenderLowHealthEffects(void)
{
    if (!g_PcConfig.nightmare || !g_PcConfig.nightmareVignette)
        return;

    /* Only render the 3D world low-health vignette during active gameplay (never over menus) */
    if (g_GameWork.gameState != GameState_InGame && g_GameWork.gameState != GameState_MapEvent)
        return;

    /* Silent Hill 2 Remake Real-Time Delta Heartbeat Low-Health Edge Vignette */
    if (g_SysWork.playerWork.player.health > Q12(0.0f) && g_SysWork.playerWork.player.health <= Q12(35.0f))
    {
        static q19_12 s_cardiacTimeQ12 = 0;

        q19_12 hp = g_SysWork.playerWork.player.health;
        /* Cardiac Cycle Period in real time (Q12 fixed-point seconds):
         * 35 HP -> 1.15 seconds (~52 BPM, resting dread)
         * 10 HP -> 0.75 seconds (~80 BPM, panic rush) */
        q19_12 periodQ12 = (hp <= Q12(15.0f)) ? Q12(0.75f) : Q12(1.15f);

        /* Advance by real frame delta time (framerate-independent on 60/120/144/240Hz monitors) */
        q19_12 dt = (g_DeltaTime > 0) ? g_DeltaTime : Q12(1.0f / 60.0f);
        s_cardiacTimeQ12 += dt;
        if (s_cardiacTimeQ12 >= periodQ12)
        {
            s_cardiacTimeQ12 %= periodQ12;
        }

        /* Phase in cycle mapped to 0..1000 */
        u32 phase = (u32)((s_cardiacTimeQ12 * 1000) / periodQ12);

        s32 pulse = 0; // 0..4096
        if (phase < 200) // 0% - 20%: Primary systolic beat ("LUB")
        {
            u16 a = (u16)((phase * 2048) / 200); // 0..180 deg
            pulse = Math_Sin(a);
        }
        else if (phase >= 260 && phase < 440) // 26% - 44%: Secondary diastolic beat ("DUB")
        {
            u16 a = (u16)(((phase - 260) * 2048) / 180);
            pulse = (Math_Sin(a) * 6) / 10; // 60% amplitude of primary beat
        }
        else // 44% - 100%: Diastolic Rest Interval (calm pause)
        {
            pulse = 0;
        }

        /* Vivid, atmospheric blood perimeter intensity */
        int baseIntensity  = (hp <= Q12(15.0f)) ? 45 : 28;
        int peakAdd        = (hp <= Q12(15.0f)) ? 115 : 75;
        int finalIntensity = baseIntensity + ((peakAdd * pulse) >> 12);
        finalIntensity     = CLAMP(finalIntensity, 20, 175);

        /* Blood red: Red = finalIntensity, Green = 0, Blue = 0 */
        Plugin_DrawDirectVignette(220, 10, 10, (u8)finalIntensity);
    }
}

/* Primary render callback called every frame by both Beta and dev builds */
PLUGIN_EXPORT void SH_Plugin_OnRender(void)
{
    Plugin_RenderLowHealthEffects();
}

PLUGIN_EXPORT void SH_Plugin_OnScreenFadeDraw(void)
{
    Plugin_RenderLowHealthEffects();
}

PLUGIN_EXPORT int SH_Plugin_OverrideNpcSpawn(e_CharaId* charaId)
{
    if (!charaId) return 0;
    /* Swap street enemies to their Otherworld counterparts */
    if (*charaId == Chara_AirScreamer)
    {
        *charaId = Chara_NightFlutter;
        return 1;
    }
    if (*charaId == Chara_Groaner)
    {
        *charaId = Chara_Wormhead;
        return 1;
    }
    return 0;
}

PLUGIN_EXPORT void SH_Plugin_ModifyRadioVolume(s32* volume)
{
    if (!volume || !g_PcConfig.nightmare) return;
    if (g_SysWork.playerWork.player.health <= Q12(35.0f))
    {
        /* PSX Audio Driver uses Attenuation (0 = Max Loudness, 255 = Silent).
         * Calculate true perceived loudness (255 - volume), attenuate by 60%,
         * and convert back to attenuation byte. */
        s32 loudness = 255 - *volume;
        if (loudness > 0)
        {
            loudness = (loudness * 4) / 10;
            *volume  = 255 - loudness;
        }
    }
}

PLUGIN_EXPORT void SH_Plugin_ModifyRadioAttributes(s32* volume, s32* pitch)
{
    if (!g_PcConfig.nightmare) return;

    q19_12 hp = g_SysWork.playerWork.player.health;
    if (hp <= Q12(35.0f))
    {
        /* 1. Attenuate perceived loudness */
        if (volume)
        {
            s32 loudness = 255 - *volume;
            if (loudness > 0)
            {
                loudness = (loudness * 4) / 10;
                *volume  = 255 - loudness;
            }
        }

        /* 2. Unstable distorted radio static pitch wobble */
        if (pitch)
        {
            static q19_12 s_radioPhase = 0;
            s_radioPhase += Q12(0.08f);
            s32 wobble = (Math_Sin(s_radioPhase) >> 9); /* -8..+8 wobble */
            *pitch = (hp <= Q12(15.0f)) ? (wobble * 3) : (wobble * 2);
        }
    }
}

PLUGIN_EXPORT struct _VECTOR3 SH_Plugin_PredictTargetPos(struct _VECTOR3 from, struct _VECTOR3 to)
{
    /* Return actual true target position without distorting combat angles */
    return to;
}
