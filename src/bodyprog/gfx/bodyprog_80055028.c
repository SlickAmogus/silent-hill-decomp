#include "game.h"
#include "inline_no_dmpsx.h"
#ifdef SH_PC_PORT
#include "pc_config.h"
/* PsyCross runtime horizontal PAR. main_pc.c bakes this to 15/14 (320x224 -> 4:3)
 * at startup; the per-poly cull bounds here must track the same factor PsyCross's
 * Hor+ ortho uses. */
extern float g_PsxPixelAspect;
#endif

#include <psyq/gtemac.h>
#include <psyq/libapi.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/player.h"
#include "bodyprog/sound/sound_system.h"
#include "main/rng.h"
#ifdef SH_PC_PORT
#include <stdio.h>
#include "sh_log.h"
#include "pc_config.h"
#include "hires_override.h"
/* When culling is disabled, ignore fog-based draw distance clamp.
 * PSX uses fogFarDistance as a draw distance optimization (don't render
 * what fog fully hides). On PC we want everything to render and let
 * fog visually obscure it instead of culling geometry. */
#define FOG_FAR_DIST() (g_PcConfig.disableCulling ? 0x7FFFFFFF : g_WorldEnvWork.fog.farDistance)

/* On PC, zero the fog depth parameter for dpcl/dpcs vertex color
 * computation. Shader fog via p1/p2/p3 handles all distance fog.
 * This prevents double-fogging and seam lines at face boundaries. */
#define VTXCOL_LDDP(dp) gte_lddp(0)

/* Clamp a polygon's OT depth so the bucket index (depth>>shift>>2) can't overrun
 * org[ORDERING_TABLE_SIZE]; the emulated GTE can return an SZ outside the range
 * real hardware's OTZ register saturates to, and an out-of-range addPrim writes
 * the packet pointer into adjacent memory -> split-pointer cutscene crashes. */
#ifdef SH_PC_PORT
#define SH_CLAMP_OT_DEPTH(depth, shift)                                         \
    do {                                                                        \
        if ((((depth) >> (shift)) >> 2) >= ORDERING_TABLE_SIZE)                 \
            (depth) = (ORDERING_TABLE_SIZE - 1) << ((shift) + 2);               \
    } while (0)

/* Whole-town mode (Pc_WholeMapDrawActive): the visible-square radius is set
 * by the per-poly far drop (min(fog.farDistance, 0x79C<<(shift+2) ≈ 61u)),
 * NOT by shader fog — lift it so distant blocks submit. Polys past 128u wrap
 * the s16 view-Z scratch negative and would hit the `<= 0` drop; rescue them
 * into the last OT bucket. Per-pixel GL depth reads the same slots as u16
 * (PsyX_SetNextPrimSz), so ordering stays correct — only the PSX OT bucket
 * is approximated. Both are no-ops when the mode is off. */
#define SH_WHOLEMAP_FARCAP(cap)                                                 \
    do {                                                                        \
        if (Pc_WholeMapDrawActive())                                            \
            (cap) = 0x7FFFFFFF;                                                 \
    } while (0)
/* STRICT `< 0`, not `<= 0`: wrapped far depths (>=128u) are always negative
 * (SZ3 saturates at 0xFFFF before a second s16 wrap), while a poly fully
 * BEHIND the camera has SZ3 min-clamped to exactly 0 on every vertex — the
 * vanilla `<= 0` drop IS the behind-camera cull. Rescuing those projected
 * them through gte_divide(H,0) into +-1024-clamped garbage quads (the
 * "random walls all over the street" of the first whole-map test). */
#define SH_WHOLEMAP_DEPTH_RESCUE(depth, shift)                                  \
    do {                                                                        \
        if ((depth) < 0 && Pc_WholeMapDrawActive())                             \
            (depth) = (ORDERING_TABLE_SIZE - 1) << ((shift) + 2);               \
    } while (0)
/* True iff this poly was depth-rescued/clamped into the last OT bucket — the
 * far band the whole-map mode adds. Gates the far-only render tweaks below so
 * the near scene keeps stock PSX behavior exactly. Depth compare FIRST: this
 * runs per poly in the drawers, and the gate is a cross-TU call. */
#define SH_WHOLEMAP_FAR_POLY(depth, shift)                                      \
    ((depth) == ((ORDERING_TABLE_SIZE - 1) << ((shift) + 2)) && Pc_WholeMapDrawActive())
#else
#define SH_CLAMP_OT_DEPTH(depth, shift) ((void)0)
#define SH_WHOLEMAP_FARCAP(cap) ((void)0)
#define SH_WHOLEMAP_DEPTH_RESCUE(depth, shift) ((void)0)
#define SH_WHOLEMAP_FAR_POLY(depth, shift) (0)
#endif

#ifdef SH_PC_PORT
/* PGXP shadow-memory propagation (Step 3): after the drawer copies a vertex word
 * from a GTE scratch slot (screenXy_0[idx], shadow-stored at gte_stsxy3c time)
 * into a prim field, propagate the precise GTE projection along the same path so
 * MakeVertex resolves the prim field BY ADDRESS at draw. Address-exact and
 * deterministic — no slot/ring/closest matching. No-op when PGXP is off or the
 * source slot is untracked (-> clean affine). */
#define SH_PGXP_PROP4(sd, p, i0, i1, i2, i3) do {        \
    Shadow_Copy(&(p)->x0, &(sd)->screenXy_0[(i0)]);      \
    Shadow_Copy(&(p)->x1, &(sd)->screenXy_0[(i1)]);      \
    Shadow_Copy(&(p)->x2, &(sd)->screenXy_0[(i2)]);      \
    Shadow_Copy(&(p)->x3, &(sd)->screenXy_0[(i3)]);      \
} while (0)
#define SH_PGXP_PROP3(sd, p, i0, i1, i2) do {            \
    Shadow_Copy(&(p)->x0, &(sd)->screenXy_0[(i0)]);      \
    Shadow_Copy(&(p)->x1, &(sd)->screenXy_0[(i1)]);      \
    Shadow_Copy(&(p)->x2, &(sd)->screenXy_0[(i2)]);      \
} while (0)
#endif


/* Compute per-vertex fog factors (0-127) from each vertex's fog ramp.
 * p1=vertex1, p2=vertex2, p3=vertex3. Vertex 0's pad byte in the color
 * word is the GPU command code, so v0's fog can't ride there; carry it
 * in the otherwise-unused pad2 texcoord short instead (PsyCross's GT4
 * parser reads it into v0's fog). Without this, v0 borrowed v1's fog,
 * giving every floor quad one wrong corner -> visible grid seams. */
#define PC_FACE_FOG_VERTS(sd) do { \
    s32 _fa; \
    _fa = (sd)->field_252[(sd)->field_380.s_0.field_10] * 16 + (sd)->field_380.s_0.field_4; \
    if (_fa > 0x1000) _fa = 0x1000; if (_fa < 0) _fa = 0; \
    poly3->pad2 = (u8)((_fa * 127) >> 12); \
    _fa = (sd)->field_252[(sd)->field_380.s_0.field_11] * 16 + (sd)->field_380.s_0.field_4; \
    if (_fa > 0x1000) _fa = 0x1000; if (_fa < 0) _fa = 0; \
    poly3->p1 = (u8)((_fa * 127) >> 12); \
    _fa = (sd)->field_252[(sd)->field_380.s_0.field_12] * 16 + (sd)->field_380.s_0.field_4; \
    if (_fa > 0x1000) _fa = 0x1000; if (_fa < 0) _fa = 0; \
    poly3->p2 = (u8)((_fa * 127) >> 12); \
    _fa = (sd)->field_252[(sd)->field_380.s_0.field_13] * 16 + (sd)->field_380.s_0.field_4; \
    if (_fa > 0x1000) _fa = 0x1000; if (_fa < 0) _fa = 0; \
    poly3->p3 = (u8)((_fa * 127) >> 12); \
} while(0)

/* Compute fog factor (0-127) from a single screenZ value. Used for character
 * prims which don't have a precomputed field_252[] fog ramp array. Reads the
 * world's fog ramp at the index derived from screenZ. */
#define PC_SCREEN_Z_TO_FOG(z) ({ \
    s32 _z = (s32)(z); s32 _fb; \
    if (!g_WorldEnvWork.isFogEnabled) { _fb = 0; } \
    else if (_z < (1 << g_WorldEnvWork.fog.depthShift)) { \
        s32 _idx = (_z << 7) >> g_WorldEnvWork.fog.depthShift; \
        if (_idx < 0) _idx = 0; if (_idx > 127) _idx = 127; \
        _fb = g_WorldEnvWork.fogRamp[_idx]; \
    } else { _fb = 255; } \
    (u8)((_fb * 127) >> 8); \
})
#else
#define FOG_FAR_DIST() (g_WorldEnvWork.fog.farDistance)
#define VTXCOL_LDDP(dp) gte_lddp(dp)
#endif

// ========================================
// ENVIRONMENT AND SCREEN GFX 1
// ========================================

extern s_WorldEnvWork g_WorldEnvWork;

void WorldEnv_Init(void) // 0x80055028
{
    func_80040BAC();
    func_8008D41C();

    g_WorldEnvWork.field_0  = 0;
    g_WorldEnvWork.field_20 = Q12(1.0f);

    g_WorldEnvWork.worldTintColor.r = 128;
    g_WorldEnvWork.worldTintColor.g = 128;
    g_WorldEnvWork.worldTintColor.b = 128;

    g_WorldEnvWork.isFogEnabled = false;
    g_WorldEnvWork.field_2        = 0;

    g_WorldEnvWork.fog.color.r = 255;
    g_WorldEnvWork.fog.color.g = 255;
    g_WorldEnvWork.fog.color.b = 255;

    g_WorldEnvWork.field_4C      = 0;
    g_WorldEnvWork.field_50      = 0;
    g_WorldEnvWork.waterZones  = 0;
    g_WorldEnvWork.fog.intensity = 0;

    gte_SetFarColor(0, 0, 0);

    WorldEnv_FogDistanceSet(Q12(32.0f), Q12(34.0f));
}

void Gfx_2dEffectsDraw(void) // 0x800550D0
{
    s32      color0;
    s32      color2;
    PACKET*  packet;
    PACKET*  packet2;
    DR_MODE* mode;
    POLY_G4* poly;
    GsOT*    ot;

    ot = &g_OrderingTable0[g_ActiveBufferIdx];

#ifdef SH_PC_PORT
    /* Per-pixel flashlight (PC port): once per frame, push the world point light
     * into VIEW space for the fragment-shader cone. GsWSMATRIX is the world->view
     * matrix (Q8 t, Q12 rotation); transforming the Q8 world light position
     * through it yields the same Q8 camera-space units the GTE RTPS captures into
     * GrVertex.vsx/vsy/vsz (the fog code transforms world positions identically,
     * see bodyprog_bone_80044F14.c). Off path: g_PsyX_FlashlightActive stays 0
     * (or the master flag is 0), so the shader cone is fully inert. */
    {
        extern int     g_PsyX_UsePerPixelFlashlight;
        extern int     g_PsyX_FlashlightActive;
        extern float   g_PsyX_FlashlightPos[3];
        extern float   g_PsyX_FlashlightShadowPos[3];
        extern float   g_PsyX_FlashlightDir[3];
        extern float   g_PsyX_FlashlightColor[3];
        extern VECTOR3 g_PcFlashlightShadowWorld;

        /* Gate on Harry's actual flashlight state (== what Game_FlashlightIsOn
         * returns). field_0==1 is only the room's dynamic-light mode (on even
         * with the flashlight stowed, so the cone got stuck on) and field_2 is
         * only the per-room glow-halo enable (0 in rooms like map3_s05 even with
         * the flashlight on, so the cone never showed) — neither tracks the
         * flashlight itself. field_60 (light pos) is refreshed every frame by
         * Gfx_FlashlightUpdate regardless, so it's valid whenever this is true.
         *
         * Cutscene gate: the active cone dims the whole scene to a dark base
         * (fragment shader *= 0.15) so the beam reads as the only light. That's
         * right for dark gameplay rooms but WRONG during cutscenes, which have
         * their own scripted lighting/framing — the first map0_s00 cutscene came
         * out looking like a pitch-dark flashlight area. Fall back to the game's
         * normal PSX flashlight rendering (glow halo below) during cutscenes,
         * same as the FPS camera + head-hide do. */
        if (g_PsyX_UsePerPixelFlashlight && g_SysWork.field_2388.isFlashlightOn_15
            && !(g_SysWork.sysFlags & SysFlag_CutsceneActive)
            && g_SysWork.cutsceneBorderState == CutsceneBorderState_None)
        {
            s32 lx = Q12_TO_Q8(g_WorldEnvWork.field_60.vx);
            s32 ly = Q12_TO_Q8(g_WorldEnvWork.field_60.vy);
            s32 lz = Q12_TO_Q8(g_WorldEnvWork.field_60.vz);

            s32 vx = (s32)(((s64)GsWSMATRIX.m[0][0] * lx + (s64)GsWSMATRIX.m[0][1] * ly + (s64)GsWSMATRIX.m[0][2] * lz) >> 12) + GsWSMATRIX.t[0];
            s32 vy = (s32)(((s64)GsWSMATRIX.m[1][0] * lx + (s64)GsWSMATRIX.m[1][1] * ly + (s64)GsWSMATRIX.m[1][2] * lz) >> 12) + GsWSMATRIX.t[1];
            s32 vz = (s32)(((s64)GsWSMATRIX.m[2][0] * lx + (s64)GsWSMATRIX.m[2][1] * ly + (s64)GsWSMATRIX.m[2][2] * lz) >> 12) + GsWSMATRIX.t[2];

            g_PsyX_FlashlightPos[0] = (float)vx;
            g_PsyX_FlashlightPos[1] = (float)vy;
            g_PsyX_FlashlightPos[2] = (float)vz;

            /* Shadow-map light origin: the PHYSICAL flashlight (chest/hand),
             * snapshotted before the FPS eye-override, pushed to view space the
             * same way. In TPS this equals FlashlightPos; in FPS it stays at the
             * real light so the shadow lands where third person shows it (a shadow
             * depends on light+occluder, not the camera). */
            {
                s32 slx = Q12_TO_Q8(g_PcFlashlightShadowWorld.vx);
                s32 sly = Q12_TO_Q8(g_PcFlashlightShadowWorld.vy);
                s32 slz = Q12_TO_Q8(g_PcFlashlightShadowWorld.vz);
                s32 svx = (s32)(((s64)GsWSMATRIX.m[0][0] * slx + (s64)GsWSMATRIX.m[0][1] * sly + (s64)GsWSMATRIX.m[0][2] * slz) >> 12) + GsWSMATRIX.t[0];
                s32 svy = (s32)(((s64)GsWSMATRIX.m[1][0] * slx + (s64)GsWSMATRIX.m[1][1] * sly + (s64)GsWSMATRIX.m[1][2] * slz) >> 12) + GsWSMATRIX.t[1];
                s32 svz = (s32)(((s64)GsWSMATRIX.m[2][0] * slx + (s64)GsWSMATRIX.m[2][1] * sly + (s64)GsWSMATRIX.m[2][2] * slz) >> 12) + GsWSMATRIX.t[2];
                g_PsyX_FlashlightShadowPos[0] = (float)svx;
                g_PsyX_FlashlightShadowPos[1] = (float)svy;
                g_PsyX_FlashlightShadowPos[2] = (float)svz;
            }

            /* Beam direction = the flashlight's world direction (field_58 — the same
             * vector the per-vertex "ambient" flashlight uses, so it turns with Harry's
             * facing) rotated into view space. Camera +Z only tracked his position, so
             * the cone never turned when he did. Rotation only (it's a direction). */
            s32 fdx = g_WorldEnvWork.field_58.vx;
            s32 fdy = g_WorldEnvWork.field_58.vy;
            s32 fdz = g_WorldEnvWork.field_58.vz;
            g_PsyX_FlashlightDir[0] = (float)(s32)(((s64)GsWSMATRIX.m[0][0] * fdx + (s64)GsWSMATRIX.m[0][1] * fdy + (s64)GsWSMATRIX.m[0][2] * fdz) >> 12);
            g_PsyX_FlashlightDir[1] = (float)(s32)(((s64)GsWSMATRIX.m[1][0] * fdx + (s64)GsWSMATRIX.m[1][1] * fdy + (s64)GsWSMATRIX.m[1][2] * fdz) >> 12);
            g_PsyX_FlashlightDir[2] = (float)(s32)(((s64)GsWSMATRIX.m[2][0] * fdx + (s64)GsWSMATRIX.m[2][1] * fdy + (s64)GsWSMATRIX.m[2][2] * fdz) >> 12);

            /* Classic style matches the room-specific RGB matrix used by the
             * original flashlight (so console `fl` tints reach the cone too);
             * Modern keeps its own warm-white beam character. */
            {
                extern int g_PsyX_FlashlightStyle;
                if (g_PsyX_FlashlightStyle)
                {
                    g_PsyX_FlashlightColor[0] = (float)g_WorldEnvWork.field_2C.m[0][0] / 4096.0f;
                    g_PsyX_FlashlightColor[1] = (float)g_WorldEnvWork.field_2C.m[1][0] / 4096.0f;
                    g_PsyX_FlashlightColor[2] = (float)g_WorldEnvWork.field_2C.m[2][0] / 4096.0f;
                }
                else
                {
                    g_PsyX_FlashlightColor[0] = 1.00f;
                    g_PsyX_FlashlightColor[1] = 0.95f;
                    g_PsyX_FlashlightColor[2] = 0.85f;
                }
            }

            g_PsyX_FlashlightActive = 1;
        }
        else
        {
            g_PsyX_FlashlightActive = 0;
        }
    }
#endif

    if (g_WorldEnvWork.field_2 != 0)
    {
#ifdef SH_PC_PORT
        /* Classic per-pixel style still needs the original subtractive outer
         * mask (func_800414E0 suppresses only its additive center while the
         * cone is active). Modern replaces the whole PSX glow compositor —
         * drawing any of it under the stylized cone double-lights into
         * blown-out highlights, so skip it entirely like the pre-calibration
         * builds did. */
        extern int g_PsyX_FlashlightActive, g_PsyX_FlashlightStyle;
        if (!(g_PsyX_FlashlightActive && g_PsyX_FlashlightStyle == 0))
#endif
        func_80041074(ot, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
    }

    /* Flashlight lens flare (Harry's chest glare) + water reflections.
     * Previously skipped on PC as "water zone rendering" that crashed —
     * the crash was the DR_MOVE occlusion seeding in func_8008D5A0
     * (hardcoded PSX packet offsets); that part is now PC-skipped inside
     * func_8008D470 and the occlusion readback short-circuits to visible. */
    if (g_WorldEnvWork.field_0 == 1 && g_WorldEnvWork.field_50 != 0)
    {
        func_8008D470(g_WorldEnvWork.field_50, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60, g_WorldEnvWork.waterZones);
    }

    if (g_WorldEnvWork.screenBrightness > 0)
    {
        poly            = (POLY_G4*)GsOUT_PACKET_P;
        mode            = (DR_MODE*)(GsOUT_PACKET_P + sizeof(POLY_G4));
        GsOUT_PACKET_P += sizeof(POLY_G4) + sizeof(DR_MODE);

        color0           = (g_WorldEnvWork.screenBrightness + (g_WorldEnvWork.screenBrightness << 8)) + (g_WorldEnvWork.screenBrightness << 16);
        *(s32*)&poly->r3 = color0;
        *(s32*)&poly->r2 = color0;
        *(s32*)&poly->r1 = color0;
        *(s32*)&poly->r0 = color0;

        setPolyG4(poly);
        setSemiTrans(poly, true);
#ifdef SH_PC_PORT
        {
            const float psxAspect = 320.0f / 240.0f;
            const float winAspect = g_PcConfig.windowHeight > 0
                ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight
                : psxAspect;
            const float horScale  = winAspect / psxAspect;
            const s16 halfW = (s16)(160.0f * horScale + 10.0f);
            setXY4(poly, -halfW, -120, halfW, -120, -halfW, 120, halfW, 120);
        }
#else
        setXY4(poly,
               -180, -120,
                180, -120,
               -180,  120,
                180,  120);
#endif

        AddPrim(ot->org, poly);
        SetDrawMode(mode, 0, 1, 0x20, NULL);
        AddPrim(ot->org, mode);
    }

#ifdef SH_PC_PORT
    /* PC port: simplified fog color quad. SetPriority is a no-op on PC,
     * and DR_MODE gets stripped by the OT0 sanitizer, so just draw the
     * POLY_G4 directly. Semi-transparency is set on the poly itself. */
    {
        poly            = (POLY_G4*)GsOUT_PACKET_P;
        GsOUT_PACKET_P += sizeof(POLY_G4);

        color2           = *(s32*)&g_WorldEnvWork.fog.color;
        *(s32*)&poly->r3 = color2;
        *(s32*)&poly->r2 = color2;
        *(s32*)&poly->r1 = color2;
        *(s32*)&poly->r0 = color2;

        SetPolyG4(poly);
        setSemiTrans(poly, true);
#ifdef SH_PC_PORT
        /* Extend fog quad to cover widescreen margins so the overlay is
         * uniform across the full window.  At 1920x1080 margin ≈ 53 PSX
         * units, requiring halfW ≈ 213 instead of the original 180. */
        {
            const float psxAspect = 320.0f / 240.0f;
            const float winAspect = g_PcConfig.windowHeight > 0
                ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight
                : psxAspect;
            const float horScale  = winAspect / psxAspect;
            const s16 halfW = (s16)(160.0f * horScale + 10.0f); /* +10 = PSX edge pad */
            setXY4(poly, -halfW, -120, halfW, -120, -halfW, 120, halfW, 120);
        }
#else
        setXY4(poly,
               -180, -120,
                180, -120,
               -180,  120,
                180,  120);
#endif

        AddPrim(&ot->org[ORDERING_TABLE_SIZE - 1], poly);
    }
#else
    packet2 = GsOUT_PACKET_P;
    packet  = packet2;

    SetPriority(packet2, 0, 0);
    AddPrim(&ot->org[ORDERING_TABLE_SIZE - 1], packet2);

    poly           = (POLY_G4*)(packet + 0xC);
    GsOUT_PACKET_P = packet + 0x30;

    color2           = *(s32*)&g_WorldEnvWork.fog.color;
    *(s32*)&poly->r3 = color2;
    *(s32*)&poly->r2 = color2;
    *(s32*)&poly->r1 = color2;
    *(s32*)&poly->r0 = color2;

    SetPolyG4(poly);
    setXY4(poly,
           -180, -120,
            180, -120,
           -180,  120,
            180,  120);

    AddPrim(&ot->org[ORDERING_TABLE_SIZE - 1], poly);
    packet = GsOUT_PACKET_P;
    SetPriority(packet, 0, 1);
    AddPrim(&ot->org[ORDERING_TABLE_SIZE - 1], packet);

    GsOUT_PACKET_P = packet + sizeof(DR_MODE);
    mode           = (DR_MODE*)GsOUT_PACKET_P;

    SetDrawMode(mode, 0, 1, 32, NULL);
    AddPrim(&ot->org[ORDERING_TABLE_SIZE - 1], mode);

    GsOUT_PACKET_P = packet + 24;
#endif
}

void func_80055330(u8 arg0, s32 arg1, u8 arg2, s32 tintR, s32 tintG, s32 tintB, q23_8 brightness) // 0x80055330
{
    g_WorldEnvWork.field_0             = arg0;
    g_WorldEnvWork.field_20            = arg1;
    g_WorldEnvWork.field_3             = arg2;
    g_WorldEnvWork.worldTintColor.r = tintR >> 5;
    g_WorldEnvWork.field_2C.m[0][2]    = tintR;
    g_WorldEnvWork.field_2C.m[0][1]    = tintR;
    g_WorldEnvWork.field_2C.m[0][0]    = tintR;
    g_WorldEnvWork.worldTintColor.g = tintG >> 5;
    g_WorldEnvWork.screenBrightness  = brightness;
    g_WorldEnvWork.worldTintColor.b = tintB >> 5;
    g_WorldEnvWork.field_2C.m[1][2]    = (s16)tintG;
    g_WorldEnvWork.field_2C.m[1][1]    = (s16)tintG;
    g_WorldEnvWork.field_2C.m[1][0]    = (s16)tintG;
    g_WorldEnvWork.field_2C.m[2][2]    = (s16)tintB;
    g_WorldEnvWork.field_2C.m[2][1]    = (s16)tintB;
    g_WorldEnvWork.field_2C.m[2][0]    = (s16)tintB;
    g_WorldEnvWork.field_24            = (tintR * arg1) >> 17;
    g_WorldEnvWork.field_25            = (tintG * arg1) >> 17;
    g_WorldEnvWork.field_26            = (tintB * arg1) >> 17;
#ifdef SH_PC_PORT
    /* Console color overrides, applied per-output so they're independent:
     *   `fl` tints field_2C (the per-vertex directional/point light — what the
     *        flashlight casts on surfaces),
     *   `wl` tints worldTintColor + field_24..26 (the flat ambient world tint).
     * Each scales the map's value by the chosen color so brightness is kept. */
    {
        extern int g_PcFlashlightColorActive, g_PcWorldLightColorActive;
        extern unsigned char g_PcFlashlightColorR, g_PcFlashlightColorG, g_PcFlashlightColorB;
        extern unsigned char g_PcWorldLightColorR, g_PcWorldLightColorG, g_PcWorldLightColorB;
        s32 c;
        if (g_PcFlashlightColorActive)
        {
            for (c = 0; c < 3; c++)
            {
                g_WorldEnvWork.field_2C.m[0][c] = (s16)((s32)g_WorldEnvWork.field_2C.m[0][c] * g_PcFlashlightColorR / 255);
                g_WorldEnvWork.field_2C.m[1][c] = (s16)((s32)g_WorldEnvWork.field_2C.m[1][c] * g_PcFlashlightColorG / 255);
                g_WorldEnvWork.field_2C.m[2][c] = (s16)((s32)g_WorldEnvWork.field_2C.m[2][c] * g_PcFlashlightColorB / 255);
            }
        }
        if (g_PcWorldLightColorActive)
        {
            g_WorldEnvWork.worldTintColor.r = (u8)((s32)g_WorldEnvWork.worldTintColor.r * g_PcWorldLightColorR / 255);
            g_WorldEnvWork.worldTintColor.g = (u8)((s32)g_WorldEnvWork.worldTintColor.g * g_PcWorldLightColorG / 255);
            g_WorldEnvWork.worldTintColor.b = (u8)((s32)g_WorldEnvWork.worldTintColor.b * g_PcWorldLightColorB / 255);
            g_WorldEnvWork.field_24 = (s16)((s32)g_WorldEnvWork.field_24 * g_PcWorldLightColorR / 255);
            g_WorldEnvWork.field_25 = (s16)((s32)g_WorldEnvWork.field_25 * g_PcWorldLightColorG / 255);
            g_WorldEnvWork.field_26 = (s16)((s32)g_WorldEnvWork.field_26 * g_PcWorldLightColorB / 255);
        }
    }

    /* NOTE: do NOT zero field_2C here for the per-pixel cone. The shader already
     * dims the whole per-vertex-lit result to a dark base, so zeroing the directional
     * light is redundant AND harmful: the character draw forces field_0=1 (point-light
     * path) and reads field_2C, so a zeroed matrix renders Harry solid black. */
#endif
}

void WorldEnv_FogParamsSet(u8 isFogEnabled, u8 fogColorR, u8 fogColorG, u8 fogColorB) // 0x800553C4
{
    g_WorldEnvWork.isFogEnabled = isFogEnabled;
    g_WorldEnvWork.fog.color.r  = fogColorR;
    g_WorldEnvWork.fog.color.g  = fogColorG;
    g_WorldEnvWork.fog.color.b  = fogColorB;
}

void func_800553E0(u32 arg0, u8 arg1, u8 arg2, u8 arg3, u8 arg4, u8 arg5, u8 arg6) // 0x800553E0
{
    g_WorldEnvWork.field_2 = arg0;

    if (arg0 != 0)
    {
        func_80040E7C(arg1, arg2, arg3, arg4, arg5, arg6);
    }
}

void func_80055434(VECTOR3* vec) // 0x80055434
{
    *vec = g_WorldEnvWork.field_60;
}

s32 func_8005545C(SVECTOR* vec) // 0x8005545C
{
    *vec = g_WorldEnvWork.field_6C;
    return g_WorldEnvWork.field_54;
}

s32 func_80055490(SVECTOR* arg0) // 0x80055490
{
    *arg0 = g_WorldEnvWork.field_58;
    return g_WorldEnvWork.field_54;
}

void func_800554C4(s32 arg0, s16 arg1, GsCOORDINATE2* coord0, GsCOORDINATE2* coord1, SVECTOR* rot, q19_12 x, q19_12 y, q19_12 z, s_WaterZone* waterZones) // 0x800554C4
{
    MATRIX   mat;
    SVECTOR  tempSvec;
    VECTOR   vec;
    VECTOR3* pos0; // Q19.12
    VECTOR3* pos1; // Q19.12

    g_WorldEnvWork.field_54 = arg0;
    g_WorldEnvWork.field_50 = arg1;
    g_WorldEnvWork.waterZones  = waterZones;

    if (coord0 == NULL)
    {
        g_WorldEnvWork.field_58 = *rot;
    }
    else
    {
        Vw_CoordHierarchyMatrixCompute(coord0, &mat);
        ApplyMatrixSV(&mat, rot, &g_WorldEnvWork.field_58);
    }

    if (coord1 == NULL)
    {
        pos0     = &g_WorldEnvWork.field_60;
        pos0->vx = x;
        pos0->vy = y;
        pos0->vz = z;
    }
    else
    {
        Vw_CoordHierarchyMatrixCompute(coord1, &mat);

        tempSvec.vx = Q12_TO_Q8(x);
        tempSvec.vy = Q12_TO_Q8(y);
        tempSvec.vz = Q12_TO_Q8(z);

        ApplyMatrix(&mat, &tempSvec, &vec);

        pos1     = &g_WorldEnvWork.field_60;
        pos1->vx = Q8_TO_Q12(vec.vx + mat.t[0]);
        pos1->vy = Q8_TO_Q12(vec.vy + mat.t[1]);
        pos1->vz = Q8_TO_Q12(vec.vz + mat.t[2]);
    }

#ifdef SH_PC_PORT
    /* FPS: aim the flashlight where the player looks, from the eye. Override the
     * Harry-facing light dir (field_58) + pos (field_60) with the FPS view forward
     * + eye so BOTH the per-pixel cone (pushed from field_58/60) and the PSX
     * per-vertex lighting follow the view. Done here, before field_6C and the
     * per-region setup below consume field_58, so they inherit the FPS direction. */
    {
        extern int     g_PcFpsCam;
        extern int     g_PsyX_FlashlightFpsMode;
        extern VECTOR3 g_PcFpsViewFwd;
        extern VECTOR3 g_PcFpsEyePos;
        extern VECTOR3 g_PcFlashlightShadowWorld;
        g_PsyX_FlashlightFpsMode = g_PcFpsCam; /* select the FPS cone size/brightness in the shader */
        /* Snapshot the REAL light position before the FPS block overrides field_60
         * with the eye. The shadow map uses this (not the eye) so FPS shadows land
         * where third person shows them; in TPS it just mirrors field_60. */
        g_PcFlashlightShadowWorld = g_WorldEnvWork.field_60;
        if (g_PcFpsCam && g_SysWork.field_2388.isFlashlightOn_15)
        {
            g_WorldEnvWork.field_58.vx = g_PcFpsViewFwd.vx;
            g_WorldEnvWork.field_58.vy = g_PcFpsViewFwd.vy;
            g_WorldEnvWork.field_58.vz = g_PcFpsViewFwd.vz;
            g_WorldEnvWork.field_60    = g_PcFpsEyePos;
        }
    }
#endif

    vwVectorToAngle(&g_WorldEnvWork.field_6C, &g_WorldEnvWork.field_58);
    g_WorldEnvWork.field_4C = arg0 >> 8;
    func_80055648(arg0, &g_WorldEnvWork.field_58);
}

void func_80055648(s32 arg0, const SVECTOR* arg1) // 0x80055648
{
    s32            var_a2;
    s32            temp_lo;
    s32            temp_t1;
    s32            temp_v0;
    s32            temp_v1;
    s32            j;
    s32            i;
    s_WorldEnvWork_84* ptr;

    for (i = 0, ptr = g_WorldEnvWork.field_84; i < 3; i++, ptr++)
    {
        switch (i)
        {
            case 0:
                var_a2 = arg1->vx;
                break;

            case 1:
                var_a2 = arg1->vy;
                break;

            case 2:
                var_a2 = arg1->vz;
                break;

            default:
                var_a2 = 0;
                break;
        }

        temp_t1 = Q12(var_a2 + Q12(1.3f)) / Q12(2.3f);
        temp_v1 = Q12(Q12(1.3f) - var_a2) / Q12(2.3f);
        temp_v0 = Q12(1024.0f) / temp_t1;
        temp_lo = Q12(1024.0f) / temp_v1;

        for (j = 0; j < ARRAY_SIZE(D_800AE1B4); j++)
        {
            ptr->field_0[0][j].vy = ptr->field_0[1][j].vy = Q12_MULT(D_800AE1B4[j].vy, arg0);
            ptr->field_0[0][j].vx                         = Q12_MULT(D_800AE1B4[j].vx, temp_t1);
            ptr->field_0[1][j].vx                         = Q12_MULT(D_800AE1B4[j].vx, temp_v1);
            ptr->field_0[0][j].vz                         = Q12_MULT(FP_MULTIPLY(D_800AE1B4[j].vz, temp_v0, Q12_SHIFT - 2), arg0);
            ptr->field_0[1][j].vz                         = Q12_MULT(FP_MULTIPLY(D_800AE1B4[j].vz, temp_lo, Q12_SHIFT - 2), arg0);
        }
    }
}

s32 func_800557DC(void) // 0x800557DC
{
    MATRIX mat;

    Vw_WorldScreenMatrixAtPositionGet(&mat, g_WorldEnvWork.field_60.vx, g_WorldEnvWork.field_60.vy, g_WorldEnvWork.field_60.vz);
    return Q8_TO_Q12(mat.t[2]);
}

void func_80055814(s32 arg0) // 0x80055814
{
    g_WorldEnvWork.fog.intensity = Q12(1.0f) - func_800559A8(arg0);
}

void WorldEnv_FogDistanceSet(q19_12 nearDist, q19_12 farDist) // 0x80055840
{
    s32 temp_lo;
    s32 temp_v1_2;
    s32 var_a0;
    s32 var_a1;
    s32 var_a2;
    s32 var_a3;
    s32 var_t0;
    s32 var_v1;
    s32 temp;

    nearDist = Q12_TO_Q8(nearDist);

    g_WorldEnvWork.fog.nearDistance = nearDist;
    g_WorldEnvWork.fog.farDistance = Q12_TO_Q8(farDist);
    g_WorldEnvWork.fog.depthShift  = 0x20 - Lzc(nearDist - 1);

    temp = (0x10 << (g_WorldEnvWork.fog.depthShift + 1)) / nearDist;
    for (var_t0 = 0, var_a3 = 0; var_t0 < Q12(1.0f) && var_a3 < 0x80; var_t0 += temp, var_a3++)
    {
        temp_v1_2 = var_t0 >> 8;

        if (temp_v1_2 == 0)
        {
            var_a1 = 0x80;
            var_a2 = var_t0 & 0x7F;
            if (var_t0 & 0x80)
            {
                var_v1 = D_800AE1C0[0];
                var_a0 = 0;
            }
            else
            {
                var_a0 = D_800AE1C0[0];
                var_v1 = D_800AE1C0[1];
            }
        }
        else
        {
            var_a1 = 0x100;
            var_a2 = var_t0 & 0xFF;
            var_a0 = D_800AE1C0[temp_v1_2];
            var_v1 = D_800AE1C0[temp_v1_2 + 1];
        }

        temp_lo = (var_a0 * (var_a1 - var_a2) + var_v1 * var_a2) / var_a1;

        limitRange(temp_lo, 0, 0xFF);

        g_WorldEnvWork.fogRamp[var_a3] = (u8)temp_lo;
    }

    for (; var_a3 < 0x80; var_a3++)
    {
        g_WorldEnvWork.fogRamp[var_a3] = 0xFF;
    }
}

s32 func_800559A8(s32 arg0) // 0x800559A8
{
    s32 temp_a2;
    s32 temp_lo;
    s32 temp_v0;
    s32 var_a1;
    s32 var_v0;

    temp_v0 = 1 << g_WorldEnvWork.fog.depthShift;
    var_a1  = temp_v0 >> 7;

    if (temp_v0 < 0)
    {
        var_a1 = (temp_v0 + 0x7F) >> 7;
    }

    temp_v0 = arg0 >> 4;
    temp_lo = temp_v0 / var_a1;

    if (temp_lo < 0)
    {
        return 0;
    }

    if (temp_lo >= 0x80)
    {
        return Q12(1.0f);
    }

    temp_a2 = g_WorldEnvWork.fogRamp[temp_lo] * 16;
    var_v0  = temp_v0 % var_a1;

    if (temp_lo == 0x7F)
    {
        temp_v0 = Q12(1.0f);
    }
    else
    {
        temp_v0 = g_WorldEnvWork.fogRamp[temp_lo + 1] * 16;
    }

    return temp_a2 + ((temp_v0 - temp_a2) * var_v0) / var_a1;
}

u8 func_80055A50(s32 arg0) // 0x80055A50
{
    s32 temp;

    temp = arg0 >> 4;

    if (temp >= (1 << g_WorldEnvWork.fog.depthShift))
    {
        return 255;
    }

    return g_WorldEnvWork.fogRamp[(temp << 7) >> g_WorldEnvWork.fog.depthShift];
}

#ifdef SH_PC_PORT
/* World-fog "keep" factor in [0..256] for a blood prim at screen-depth z: 256 near
 * (no fog), 0 far (full fog). The layered ADDITIVE blood layers (GL_ONE,GL_ONE) keep
 * adding red into the distance regardless of depth, visually overpowering the color fog
 * func_80055A90 already applies — so distant blood stays vivid while the world around it
 * grays out. Multiplying the additive layer color by this (>>8) fades it toward black at
 * the SAME rate as world geometry (same fogRamp), so it disappears into the fog. */
int Pc_BloodFogKeep(s32 z)
{
    s32 idx;
    if (!g_WorldEnvWork.isFogEnabled) return 256;
    if (z < 0) z = 0;
    if (z >= (1 << g_WorldEnvWork.fog.depthShift)) return 0;
    idx = (z << 7) >> g_WorldEnvWork.fog.depthShift;
    if (idx > 127) idx = 127;
    return 256 - g_WorldEnvWork.fogRamp[idx];
}
#endif

void func_80055A90(CVECTOR* arg0, CVECTOR* arg1, u8 arg2, s32 arg3) // 0x80055A90
{
    s32 var_v1;

    // TODO: Maybe Q8 to Q12 and back again.
    arg3 = arg3 >> 4;
    if (arg3 < 0)
    {
        arg3 = 0;
    }

    if (arg3 < (1 << g_WorldEnvWork.fog.depthShift))
    {
        arg3 = g_WorldEnvWork.fogRamp[(arg3 << 7) >> g_WorldEnvWork.fog.depthShift] << 4;
    }
    else
    {
        arg3 = 0xFF << 4;
    }

    var_v1 = Q12(1.0f) - (g_WorldEnvWork.fog.intensity + arg3);
    if (var_v1 < 0)
    {
        var_v1 = 0;
    }

    gte_lddp(var_v1);
    gte_ldrgb(&g_WorldEnvWork.fog.color);

    gte_SetFarColor(0, 0, 0);

    gte_dpcs();

    gte_strgb(arg0);

    gte_lddp(arg3);
    gte_ldrgb(&g_WorldEnvWork.worldTintColor);

    gte_ldsv_(arg2 << 5);

    gte_dpcl();

    gte_strgb(arg1);
}

void func_80055B74(CVECTOR* result, CVECTOR* color, s32 arg2) // 0x80055B74
{
    s32 var_v0;
    s32 var_t0;

    arg2 >>= 4;

#ifdef SH_PC_PORT
    /* Same signedness hazard as the per-vertex ramp above: a negative
     * depth passes the `<` test and indexes fogRamp[] out of bounds. */
    if (arg2 < 0)
    {
        arg2 = 0;
    }
#endif

    var_t0 = g_WorldEnvWork.field_20 >> 5;

    if (arg2 < (1 << g_WorldEnvWork.fog.depthShift))
    {
        var_v0 = g_WorldEnvWork.fogRamp[(arg2 << 7) >> g_WorldEnvWork.fog.depthShift];
    }
    else
    {
        var_v0 = 255;
    }

    var_v0 <<= 4;

    gte_lddp(var_v0);
    gte_ldrgb(color);
    gte_SetFarColor(g_WorldEnvWork.fog.color.r, g_WorldEnvWork.fog.color.g, g_WorldEnvWork.fog.color.b);
    gte_ldsv_(var_t0 << 5);
    gte_dpcl();
    gte_strgb(result);
    gte_SetFarColor(0, 0, 0);
}

void func_80055C3C(CVECTOR* result, CVECTOR* color, s32 arg2, s32 arg3, s32 arg4, s32 arg5) // 0x80055C3C
{
    s32 temp_a1;
    s32 var_v0;
    s32 var_s0;

    var_s0  = arg5 >> 4;
    temp_a1 = func_80055D78(arg2, arg3, arg4);

    if (g_WorldEnvWork.isFogEnabled)
    {
        if (var_s0 < (1 << g_WorldEnvWork.fog.depthShift))
        {
            var_v0 = g_WorldEnvWork.fogRamp[(var_s0 << 7) >> g_WorldEnvWork.fog.depthShift];
        }
        else
        {
            var_v0 = 255;
        }

        gte_lddp(var_v0 << 4);
        gte_ldrgb(color);
        gte_SetFarColor(g_WorldEnvWork.fog.color.r, g_WorldEnvWork.fog.color.g, g_WorldEnvWork.fog.color.b);
        gte_ldsv_(temp_a1 << 5);

        gte_dpcl();
        gte_strgb(result);

        gte_SetFarColor(0, 0, 0);
    }
    else
    {
        gte_lddp(Q12(1.0f) - (temp_a1 << 5));
        gte_ldrgb(color);
        gte_dpcs();
        gte_strgb(result);
    }
}

u8 func_80055D78(q19_12 posX, q19_12 posY, q19_12 posZ) // 0x80055D78
{
    q23_8    pos[3];
    s32      temp_v1;
    q23_8    tempX;
    s32      var_a3;
    s32      i;
    VECTOR3* ptr0;
    VECTOR3* ptr1;

    pos[0] = Q12_TO_Q8(posX) - Q12_TO_Q8(g_WorldEnvWork.field_60.vx);
    pos[1] = Q12_TO_Q8(posY) - Q12_TO_Q8(g_WorldEnvWork.field_60.vy);
    pos[2] = Q12_TO_Q8(posZ) - Q12_TO_Q8(g_WorldEnvWork.field_60.vz);

    if (g_WorldEnvWork.field_0 != 0)
    {
        ptr1 = &g_WorldEnvWork.field_84;
        for (i = 0, ptr0 = ptr1, var_a3 = 0xFF;
             i < ARRAY_SIZE(pos);
             i++, ptr0 += 2)
        {
            tempX = pos[i];
            ptr1  = ptr0;
            if (tempX < 0)
            {
                tempX = -tempX;
                ptr1++;
            }

            temp_v1 = ptr1->vy + Q12_MULT(ptr1->vz, tempX - ptr1->vx);
            if (temp_v1 < var_a3)
            {
                var_a3 = temp_v1;
            }
        }
    }
    else
    {
        var_a3 = 0;
    }

    var_a3 += g_WorldEnvWork.field_20 >> 5;

    limitRange(var_a3, 0, g_WorldEnvWork.field_3);
    return var_a3;
}

void func_80055E90(CVECTOR* color, u8 fadeAmount) // 0x80055E90
{
    q19_12 alpha;
    u8     prev_cd;

    alpha = Q12(1.0f) - (fadeAmount * 32);

    // Works similar to `gte_DpqColor` macro, but `gte_lddp`/`gte_ldrgb` are in wrong order?

    gte_lddp(alpha);
    gte_ldrgb(&g_WorldEnvWork.worldTintColor);
    gte_dpcs();

    prev_cd = color->cd;
    gte_strgb(color);
    color->cd = prev_cd;
}

void func_80055ECC(CVECTOR* color, SVECTOR3* arg1, SVECTOR3* arg2, MATRIX* mat) // 0x80055ECC
{
    func_80055E90(color, func_80055F08(arg1, arg2, mat));
}

u8 func_80055F08(SVECTOR3* arg0, SVECTOR3* arg1, MATRIX* mat) // 0x80055F08
{
    MATRIX  rotMat;
    DVECTOR screenPos;
    s32     geomOffsetX;
    s32     geomOffsetY;
    s32     depthP;
    s32     geomScreen;
    s32     ret;
    s32     var_v1;
    u8      field_3;
    s32     field_20;

    func_80057228(mat, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);

    gte_lddqa(g_WorldEnvWork.field_4C);
    gte_lddqb_0();
    gte_ldtr_0();

    ReadGeomOffset(&geomOffsetX, &geomOffsetY);
    geomScreen = ReadGeomScreen();
    SetGeomOffset(-1024, -1024);
    SetGeomScreen(16);

    rotMat.m[0][0] = g_WorldEnvWork.field_74.vx;
    rotMat.m[0][1] = g_WorldEnvWork.field_74.vy;
    rotMat.m[0][2] = g_WorldEnvWork.field_74.vz;
    rotMat.m[1][0] = -arg1->vx;
    rotMat.m[1][1] = -arg1->vy;
    rotMat.m[1][2] = -arg1->vz;
    rotMat.m[2][0] = arg0->vx - g_WorldEnvWork.field_7C.vx;
    rotMat.m[2][1] = arg0->vy - g_WorldEnvWork.field_7C.vy;
    rotMat.m[2][2] = arg0->vz - g_WorldEnvWork.field_7C.vz;

    field_3  = g_WorldEnvWork.field_3;
    field_20 = g_WorldEnvWork.field_20 >> 5;

    SetRotMatrix(&rotMat);

    gte_ldv0(&rotMat.m[2][0]);
    gte_rtps();
    gte_stsxy(&screenPos);
    gte_stdp(&depthP);

    screenPos.vx += 1024;
    screenPos.vy += 1024;

    ret   = (screenPos.vx * screenPos.vy) + (screenPos.vy * (depthP >> 4));
    ret >>= 5;
    ret  -= 16;
    if (ret < 0)
    {
        ret = 0;
    }

    ret += field_20;

    var_v1 = (screenPos.vx >> 1) + (depthP >> 2);
    if (var_v1 > 48)
    {
        var_v1 = 48;
    }

    ret = ret + var_v1;
    if (field_3 < ret)
    {
        ret = field_3;
    }

    SetGeomOffset(geomOffsetX, geomOffsetY);
    SetGeomScreen(geomScreen);

    return ret;
}

// ========================================
// WORLD INITIALIZATION 2
// ========================================

#ifdef SH_PC_PORT
/* PC: use LmHeader_FixOffsets_PC from lm_reformat.c which handles 32→64-bit struct conversion */
extern void LmHeader_FixOffsets_PC(s_LmHeader* lmHdr);

void LmHeader_FixOffsets(s_LmHeader* lmHdr) // 0x800560FC
{
    LmHeader_FixOffsets_PC(lmHdr);
}

void ModelHeader_FixOffsets(s_ModelHeader* modelHdr, s_LmHeader* lmHdr) // 0x800561A4
{
    /* On PC, model headers are already fully parsed by LmHeader_FixOffsets_PC */
    (void)modelHdr;
    (void)lmHdr;
}
#else
void LmHeader_FixOffsets(s_LmHeader* lmHdr) // 0x800560FC
{
    s32 i;

    if (lmHdr->isLoaded == true)
    {
        return;
    }
    lmHdr->isLoaded = true;

    // Add memory address of header to pointer fields.
    lmHdr->materials   = (u8*)lmHdr->materials   + (u32)lmHdr;
    lmHdr->modelHdrs   = (u8*)lmHdr->modelHdrs   + (u32)lmHdr;
    lmHdr->modelOrder = (u8*)lmHdr->modelOrder + (u32)lmHdr;

    for (i = 0; i < lmHdr->modelCount; i++)
    {
        if (lmHdr->magic == LM_HEADER_MAGIC)
        {
            ModelHeader_FixOffsets(&lmHdr->modelHdrs[i], lmHdr);
        }
    }
}

void ModelHeader_FixOffsets(s_ModelHeader* modelHdr, s_LmHeader* lmHdr) // 0x800561A4
{
    s_MeshHeader* curMeshHdr;

    modelHdr->meshHdrs = (u8*)modelHdr->meshHdrs + (uintptr_t)lmHdr;

    for (curMeshHdr = &modelHdr->meshHdrs[0]; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        curMeshHdr->primitives = (u8*)curMeshHdr->primitives + (uintptr_t)lmHdr;
        curMeshHdr->verticesXy = (u8*)curMeshHdr->verticesXy + (uintptr_t)lmHdr;
        curMeshHdr->verticesZ  = (u8*)curMeshHdr->verticesZ  + (uintptr_t)lmHdr;
        curMeshHdr->normals   = (u8*)curMeshHdr->normals   + (uintptr_t)lmHdr;
        curMeshHdr->unkPtr_14    = (u8*)curMeshHdr->unkPtr_14    + (uintptr_t)lmHdr;
    }
}
#endif

void Lm_TransparentPrimSet(s_LmHeader* lmHdr, bool transparency) // 0x80056244
{
    s_ModelHeader* modelHdrs;
    s_ModelHeader* curModelHdr;
    s_MeshHeader*  curMeshHdr;
    s_Primitive*   prim;

    modelHdrs = lmHdr->modelHdrs;

    for (curModelHdr = &modelHdrs[0]; curModelHdr < &modelHdrs[lmHdr->modelCount]; curModelHdr++)
    {
        for (curMeshHdr = &curModelHdr->meshHdrs[0]; curMeshHdr < &curModelHdr->meshHdrs[curModelHdr->meshCount]; curMeshHdr++)
        {
            for (prim = &curMeshHdr->primitives[0]; prim < &curMeshHdr->primitives[curMeshHdr->primitiveCount]; prim++)
            {
                prim->field_6.bits.isTransparent = transparency;
            }
        }
    }
}

s32 Lm_MaterialCountGet(bool (*filterFunc)(s_Material* mat), s_LmHeader* lmHdr) // 0x80056348
{
    s32         count;
    s_Material* curMat;

    count = 0;
    for (curMat = lmHdr->materials; curMat < (lmHdr->materials + lmHdr->materialCount); curMat++)
    {
        if (filterFunc(curMat))
        {
            count++;
        }
    }

    return count;
}

void func_800563E8(s_LmHeader* lmHdr, s32 arg1, s32 arg2, s32 arg3) // 0x800563E8
{
    s32         i;
    s_Material* curMat;

    if (arg2 < 0)
    {
        arg2 += 15;
    }

    for (i = 0, curMat = &lmHdr->materials[0];
         i < lmHdr->materialCount;
         i++, curMat++)
    {
        // TODO: Bitfield stuff? Doesn't seem to match other uses of `field_E`/`field_10` we've seen though.
        u8  temp_a0 = curMat->field_E;
        u16 temp_v1 = curMat->field_10;

        curMat->field_E  = ((temp_a0 + arg1) & 0x1F) | (temp_a0 & 0xE0);
        curMat->field_10 = ((temp_v1 + (arg2 >> 4)) & 0x3F) | ((temp_v1 + (arg3 << 6)) & 0x7FC0);
    }
}

void Lm_MaterialFileIdxApply(s_LmHeader* lmHdr, e_FsFile fileIdx, s_FsImageDesc* image, s32 blendMode) // 0x80056464
{
    char  sp10[8];
    char  sp18[16];
    char* sp10Ptr;
    char* sp18Ptr;

    // Probably a `memset`.
    *(int*)sp10 = *(int*)(sp10 + 4) = 0;

    Fs_GetFileName(sp18, fileIdx);

    sp10Ptr = sp10;
    sp18Ptr = sp18;

    while (sp10Ptr < &sp10[ARRAY_SIZE(sp10)] && *sp18Ptr != '.')
    {
        *sp10Ptr++ = *sp18Ptr++;
    }

    Lm_MaterialFsImageApply(lmHdr, sp10, image, blendMode);
}

void Lm_MaterialFsImageApply1(s_LmHeader* lmHdr, char* newStr, s_FsImageDesc* image, s32 blendMode) // 0x80056504
{
    char strCpy[8];

    StringCopy(strCpy, newStr);
    Lm_MaterialFsImageApply(lmHdr, strCpy, image, blendMode);
}

bool Lm_MaterialFsImageApply(s_LmHeader* lmHdr, char* fileName, s_FsImageDesc* image, s32 blendMode) // 0x80056558
{
    s_Material* curMat;

    for (curMat = &lmHdr->materials[0];
         curMat < &lmHdr->materials[lmHdr->materialCount];
         curMat++)
    {
        if (!COMPARE_FILENAMES(&curMat->name, fileName))
        {
            curMat->field_C = 1;
            Material_FsImageApply(curMat, image, blendMode);
            return true;
        }
    }

    return false;
}

void Material_FsImageApply(s_Material* mat, s_FsImageDesc* image, s32 blendMode) // 0x8005660C
{
    s32 coeff;

    coeff = 4;
    switch (image->tPage[0])
    {
        default:
        case 0:
            break;

        case 1:
            coeff = 2;
            break;

        case 2:
            coeff = 1;
            break;
    }

    mat->field_14.u8[0] = image->u * coeff;
    mat->field_14.u8[1] = image->v;

    // Set GPU flags.
    mat->field_E  = ((image->tPage[0] & 0x3) << 7) | // X bits of texture page.
                    ((blendMode & 0x3) << 5) |       // Semi-transparency blend mode.
                    (image->tPage[1] & (1 << 4)) |   // dither/texture-depth flag.
                    (image->tPage[1] & 0xF);         // Y bits + color depth.
    mat->field_10 = (image->clutY << 6) |
                    ((image->clutX >> 4) & 0x3F);
}

void func_800566B4(s_LmHeader* lmHdr, s_FsImageDesc* images, s8 unused, s32 startIdx, s32 blendMode) // 0x800566B4
{
    char           filename[16];
    s32            i;
    s_Material*    curMat;
    s_FsImageDesc* curImage;

    // Loop could be using `&image[i]`/`&arg0->field_4[i]` instead? Wasn't able to make that match though.
    for (i = 0, curImage = images, curMat = lmHdr->materials;
         i < lmHdr->materialCount;
         i++, curMat++, curImage++)
    {
        Material_TimFileNameGet(filename, curMat);
#ifdef SH_PC_PORT
        /* Fs_FindNextFile returns NO_VALUE for a missing TIM name; enqueuing
         * that gave info = &g_FileTable[0xFFFFFFFF], a non-canonical pointer
         * Windows reports as a read at 0xFFFFFFFFFFFFFFFF — the whole
         * sewer/save-load crash family (SewerCrash*.log, SaveLoad.log). PSX
         * read garbage table bytes and limped on; skip the file instead. */
        {
            s32 timFileIdx = Fs_FindNextFile(filename, 0, startIdx);
            if (timFileIdx == NO_VALUE)
            {
                SH_DBG("[FSQ] material TIM '%.8s' not in file table — skipping load", filename);
            }
            else
            {
                Fs_QueueStartReadTim(timFileIdx, FS_BUFFER_9, curImage);
            }
        }
#else
        Fs_QueueStartReadTim(Fs_FindNextFile(filename, 0, startIdx), FS_BUFFER_9, curImage);
#endif
        Material_FsImageApply(curMat, curImage, blendMode);
    }
}

void Lm_MaterialsLoadWithFilter(s_LmHeader* lmHdr, s_ActiveChunkTextures* activeTexs, bool (*filterFunc)(s_Material* mat), e_FsFile fileIdx, s32 blendMode) // 0x80056774
{
    s_Material* curMat;

    for (curMat = &lmHdr->materials[0]; curMat < &lmHdr->materials[lmHdr->materialCount]; curMat++)
    {
        if (curMat->field_C == 0 && curMat->texture == NULL &&
            (filterFunc == NULL || filterFunc(curMat)))
        {
            curMat->texture = Texture_Get(curMat, activeTexs, FS_BUFFER_9, fileIdx, blendMode);
            if (curMat->texture != NULL)
            {
                Material_FsImageApply(curMat, &curMat->texture->imageDesc, blendMode);
            }
        }
    }
}

bool Lm_IsTextureLoaded(s_LmHeader* lmHdr) // 0x80056888
{
    s_Material* curMat;

    if (!lmHdr->isLoaded)
    {
        return false;
    }

    for (curMat = &lmHdr->materials[0]; curMat < &lmHdr->materials[lmHdr->materialCount]; curMat++)
    {
        if (curMat->field_C != 0)
        {
            continue;
        }

        if (curMat->texture == NULL)
        {
            return false;
        }

        if (!Fs_QueueIsEntryLoaded(curMat->texture->queueIdx))
        {
            return false;
        }

#ifdef SH_PC_PORT
        /* [TEXVRAM] otherworld lighthouse rainbow (map6_s02): a chunk material's
         * texture slot is loaded but holds wrong VRAM content. Log each resident
         * chunk texture's name + the VRAM rect it claims (imageDesc), once per
         * distinct name. Two DIFFERENT names reporting the SAME tPage/clutXY =
         * a VRAM page collision (one poly samples the other's texture = rainbow).
         * Ruled out: missing-TIM (only empty-name [FSQ] hits), world objects
         * (no [WOBJ] for the tower), pending-read race (this gate already waits
         * on Fs_QueueIsEntryLoaded), pool-exhaustion (yields black not rainbow). */
        {
            s_FsImageDesc* d = &curMat->texture->imageDesc;
            static u_Filename s_seen[48];
            static int s_seenCount = 0;
            int _k, _dup = 0;
            for (_k = 0; _k < s_seenCount; _k++)
                if (s_seen[_k].u32[0] == curMat->texture->name.u32[0] &&
                    s_seen[_k].u32[1] == curMat->texture->name.u32[1]) { _dup = 1; break; }
            if (!_dup && s_seenCount < 48)
            {
                s_seen[s_seenCount++] = curMat->texture->name;
                SH_DBG("[TEXVRAM] '%.8s' tPage=[%d,%d] clutXY=(%d,%d) uv=(%d,%d)",
                       curMat->texture->name.str, d->tPage[0], d->tPage[1],
                       d->clutX, d->clutY, d->u, d->v);
            }
        }
#endif
    }

    return true;
}

#ifdef SH_PC_PORT
/* Set while the interior chunk-texture pool is being (re)synced
 * (Ipd_ChunkMaterialsApply steal loop). Scopes the NULL-texture untexture below
 * to that path only, so other maps/draws are unaffected. */
int g_PcInteriorMatSync = 0;

/* Force every prim bound to material `matIdx` to the untextured sentinel (32,
 * same value the no-material case uses). A material whose VRAM page was stolen
 * for a nearer chunk has texture==NULL but its field_E still points at that
 * page, which now holds another chunk's 4bpp texture+CLUT — drawing it sampled
 * the wrong page (the interior "rainbow"). Rendering it flat instead matches
 * vanilla's out-of-pool chunks (which simply go untextured/black). */
static void Model_MaterialUntexture(s_ModelHeader* modelHdr, s32 matIdx)
{
    s_MeshHeader* curMeshHdr;
    s_Primitive*  curPrim;

    for (curMeshHdr = modelHdr->meshHdrs; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        for (curPrim = curMeshHdr->primitives; curPrim < &curMeshHdr->primitives[curMeshHdr->primitiveCount]; curPrim++)
        {
            if (curPrim->field_6.bits.materialIdx == matIdx)
            {
                curPrim->field_6.bits.field_6_0 = 32;
            }
        }
    }
}
#endif

void Lm_MaterialFlagsApply(s_LmHeader* lmHdr) // 0x80056954
{
    s32         i;
    s32         j;
    s32         matFlags;
    s_Material* curMat;

    for (i = 0, curMat = lmHdr->materials; i < lmHdr->materialCount; i++, curMat++)
    {
#ifdef SH_PC_PORT
        if (g_PcInteriorMatSync && curMat->texture == NULL)
        {
            for (j = 0; j < lmHdr->modelCount; j++)
            {
                if (lmHdr->magic == LM_HEADER_MAGIC)
                {
                    Model_MaterialUntexture(&lmHdr->modelHdrs[j], i);
                }
            }
            continue;
        }
#endif
        matFlags = (curMat->field_E != curMat->field_F) ? MaterialFlag_0 : MaterialFlag_None;

        if (curMat->field_10 != curMat->field_12)
        {
            matFlags |= MaterialFlag_1;
        }
        if (curMat->field_14.u16 != curMat->field_16.u16)
        {
            matFlags |= MaterialFlag_2;
        }
#ifdef SH_PC_PORT
        /* Reaching here during an interior sync means this material IS resident
         * (the texture==NULL case untextured its prims and continued above). Its
         * VRAM page may have been stolen and then returned to the SAME slot, so
         * field_E == field_F and the change-detection wouldn't re-apply the page —
         * leaving the prims we set to the untextured sentinel (field_6_0=32) stuck
         * flat forever (the "some interiors render untextured" report). Force the
         * page re-apply while syncing so a returned page re-textures its prims;
         * a no-op for prims already at field_E. */
        if (g_PcInteriorMatSync)
        {
            matFlags |= MaterialFlag_0;
        }
#endif

        if (matFlags != 0)
        {
            for (j = 0; j < lmHdr->modelCount; j++)
            {
                if (lmHdr->magic == LM_HEADER_MAGIC)
                {
                    Model_MaterialFlagsApply(&lmHdr->modelHdrs[j], i, curMat, matFlags);
                }
            }

            curMat->field_F        = curMat->field_E;
            curMat->field_12       = curMat->field_10;
            curMat->field_16.u8[0] = curMat->field_14.u8[0];
            curMat->field_16.u8[1] = curMat->field_14.u8[1];
        }
    }
}

void Model_MaterialFlagsApply(s_ModelHeader* modelHdr, s32 arg1, const s_Material* mat, s32 matFlags) // 0x80056A88
{
    u16           field_14;
    u16           field_16;
    s_MeshHeader* curMeshHdr;
    s_Primitive*  curPrim;

    // Run through meshes.
    for (curMeshHdr = modelHdr->meshHdrs; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        // Run through primitives.
        for (curPrim = curMeshHdr->primitives; curPrim < &curMeshHdr->primitives[curMeshHdr->primitiveCount]; curPrim++)
        {
            // No material(?).
            if (curPrim->field_6.bits.materialIdx == NO_VALUE)
            {
                curPrim->field_6.bits.field_6_0 = 32;
            }

            // Apply material flags.
            if (curPrim->field_6.bits.materialIdx == arg1)
            {
                if (matFlags & MaterialFlag_0)
                {
                    curPrim->field_6.bits.field_6_0 = mat->field_E;
                }
                if (matFlags & MaterialFlag_1)
                {
                    curPrim->field_2 = mat->field_10 + (curPrim->field_2 - mat->field_12);
                }
                if (matFlags & MaterialFlag_2)
                {
                    field_16      = mat->field_16.u16;
                    field_14      = mat->field_14.u16;
                    curPrim->field_0 = field_14 + (curPrim->field_0 - field_16);
                    curPrim->field_4 = field_14 + (curPrim->field_4 - field_16);
                    curPrim->field_8 = field_14 + (curPrim->field_8 - field_16);
                    curPrim->field_A = field_14 + (curPrim->field_A - field_16);
                }
            }
        }
    }
}

void Lm_MaterialRefCountDec(s_LmHeader* lmHdr) // 0x80056BF8
{
    s_Material* curMat;
    s_Texture*  tex;

#ifdef SH_PC_PORT
    /* Unused-map path (map6_s05): its LM file never loads, so the header
     * keeps junk/sentinel bytes — materials can be NO_VALUE and the walk
     * below dereferences -1. Gate on the header magic like Bone_ModelAssign
     * and Lm_ModelFind already do. */
    if (lmHdr == NULL || lmHdr->magic != LM_HEADER_MAGIC)
    {
        return;
    }
#endif

    // Run through materials.
    for (curMat = &lmHdr->materials[0]; curMat < &lmHdr->materials[lmHdr->materialCount]; curMat++)
    {
        tex = curMat->texture;
        if (tex != NULL)
        {
            tex->refCount--;
            if (tex->refCount < 0)
            {
                tex->refCount = 0;
            }

            curMat->texture = NULL;
        }
    }
}

s32 LmHeader_ModelCountGet(s_LmHeader* lmHdr) // 0x80056C80
{
    return lmHdr->modelCount;
}

void Bone_ModelAssign(s_Bone* bone, s_LmHeader* lmHdr, s32 modelHdrIdx)
{
    s_ModelHeader* modelHdr;

    modelHdr = lmHdr->modelHdrs;
    bone->modelInfo.modelIdx = modelHdrIdx;

    if (lmHdr->magic == LM_HEADER_MAGIC)
    {
        bone->modelInfo.modelHdr = &modelHdr[modelHdrIdx];
    }
}

bool Lm_ModelFind(s_WorldObjectModel* model, s_LmHeader* lmHdr, s_WorldObjectMetadata* metadata) // 0x80056CB4
{
    u_Filename     sp10;
    s32            modelHdrCount;
    bool           result;
    s32            i;
    s_ModelHeader* modelHdr;

    result = false;

    StringCopy(sp10.str, metadata->name.str);

    modelHdrCount = lmHdr->modelCount;

    if (lmHdr->magic == LM_HEADER_MAGIC)
    {
        for (i = 0, modelHdr = &lmHdr->modelHdrs[i]; i < modelHdrCount; i++, modelHdr++)
        {
            if (!COMPARE_FILENAMES(&modelHdr->name, &sp10))
            {
                result                       = true;
                model->modelInfo.modelIdx = i;
                model->modelInfo.modelHdr = modelHdr;
            }
        }
    }

    return result;
}

void StringCopy(char* prevStr, char* newStr) // 0x80056D64
{
    *(s32*)&prevStr[4] = 0;
    *(s32*)&prevStr[0] = 0;
#ifdef SH_PC_PORT
    if (newStr != NULL)
#endif
    strncpy(prevStr, newStr, 8);
}

// ========================================
// ENVIRONMENT AND SCREEN GFX 2 (Drawing?)
// ========================================

void Gfx_FogOverlayQuadDraw(s16 arg0, s16 arg1, s16 arg2, s16 arg3, s32 arg4, s32 arg5, GsOT* ot, s32 arg7) // 0x80056D8C
{
    s16       var_a3;
    s16       var_a3_2;
    s16       var_v1;
    s16       var_v1_2;
    q23_8     temp_a0_2;
    q19_12    var_s0;
    s32       var_t1;
    q23_8     var_v0;
    q23_8     var_v1_3;
    s32       var_v1_4;
    POLY_G4*  poly;
    DR_MODE*  mode;
    PACKET*   packet;
    PACKET*   packet2;
    GsOT_TAG* tag;
    s16       temp_s6;
    s16       temp_s5;

    var_t1  = arg4;
    temp_s5 = arg3;

    if (!g_WorldEnvWork.isFogEnabled)
    {
        return;
    }

#ifdef SH_PC_PORT
    /* Skip the 2D fog overlay quad on PC. PSX uses GPU mask bits
     * (SetPriority) so the overlay only affects geometry pixels.
     * Without mask bits, the overlay renders over the background
     * too, brightening the sky and double-fogging world geometry.
     * Per-vertex shader fog (dpcs vertex colors) handles fog
     * correctly without needing the overlay. */
    return;
#endif

    var_a3 = MAX(arg0, ~(g_GameWork.gsScreenWidth >> 1));
    var_v1 = MAX(arg1, ~(g_GameWork.gsScreenHeight >> 1));

    var_v1_2 = CLAMP_HIGH(arg2, (g_GameWork.gsScreenWidth >> 1) + 1);
    temp_s6  = var_v1_2;

    var_a3_2 = CLAMP_HIGH(temp_s5, (g_GameWork.gsScreenHeight >> 1) + 1);
    temp_s5  = var_a3_2;

    temp_a0_2 = 0x79C << (arg7 + 2);

    if (g_WorldEnvWork.isFogEnabled)
    {
        var_v1_3 = MIN(temp_a0_2, FOG_FAR_DIST());
    }
    else
    {
        var_v1_3 = temp_a0_2;
    }

    var_v0 = var_t1 >> 4;
    if (var_v0 < (var_v1_3 + Q8(3.0f)))
    {
        if (var_t1 < 0)
        {
            var_t1 = 0;
        }

        var_s0 = (func_80055A50(var_t1) * 16) + g_WorldEnvWork.fog.intensity;
        var_s0 = MIN(var_s0, Q12(1.0f));

        var_v1_4 = MAX(arg5 >> 7, 1);

        packet = GsOUT_PACKET_P;
        tag    = &ot->org[var_v1_4];

        SetPriority(packet, 0, 0);
        AddPrim(tag, packet);

        poly           = (POLY_G4*)(packet + 0xC);
        GsOUT_PACKET_P = (PACKET*)poly;

        *(u32*)&poly->r0 =
        *(u32*)&poly->r1 =
        *(u32*)&poly->r2 =
        *(u32*)&poly->r3 = Q12_MULT(g_WorldEnvWork.fog.color.r, var_s0)       +
                          (Q12_MULT(g_WorldEnvWork.fog.color.g, var_s0) << 8) +
                          (Q12_MULT(g_WorldEnvWork.fog.color.b, var_s0) << 16);

        SetPolyG4(poly);

        *(s32*)&poly->x0 = var_a3 + (var_v1 << 16);
        *(s32*)&poly->x1 = temp_s6 + (var_v1 << 16);
        *(s32*)&poly->x2 = var_a3 + (temp_s5 << 16);
        *(s32*)&poly->x3 = temp_s6 + (temp_s5 << 16);

        setSemiTrans(poly, true);
        AddPrim(tag, poly);

        mode           = (DR_MODE*)(packet + 0x30);
        GsOUT_PACKET_P = (PACKET*)mode;
        SetDrawMode(mode, 0, 1, 32, NULL);
        AddPrim(tag, mode);

        packet2        = packet + 60;
        GsOUT_PACKET_P = packet2;
        SetPriority(packet2, 1, 1);
        AddPrim(tag, packet2);

        GsOUT_PACKET_P = packet + 72;
    }
}

void func_80057090(s_ModelInfo* modelInfo, GsOT* arg1, s32 arg2, MATRIX* viewMat, MATRIX* worldMat, u16 arg5) // 0x80057090
{
    s32            temp_a0;
    GsOT_TAG*      otTag;
    s_ModelHeader* modelHdr;

    modelHdr = modelInfo->modelHdr;

#ifdef SH_PC_PORT
    if (modelHdr == NULL) {
        return;
    }
#endif

    if (modelInfo->field_0 < 0)
    {
        return;
    }


    otTag = &arg1->org[func_800571D0(modelHdr->field_B_1)];
    temp_a0 = modelHdr->field_B_4;

    if ((temp_a0 & 0xFF) && temp_a0 >= 0 && temp_a0 < 4) // TODO: `& 0xFF` needed for match.
    {
        func_80059D50(temp_a0, modelInfo, viewMat, arg2, otTag);
    }
    else
    {
        if (worldMat != NULL && g_WorldEnvWork.field_0 != 0)
        {
            func_80057228(worldMat, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
        }

        if (modelHdr->field_B_0)
        {
            g_WorldEnvWork.field_14C = arg5;
            func_8005A21C(modelInfo, otTag, arg2, viewMat);
        }
        else
        {
            func_80057344(modelInfo, otTag, arg2, viewMat);
        }
    }
}

s32 func_800571D0(u32 arg0) // 0x800571D0
{
    switch (arg0)
    {
        default:
        case 0:
            return 2;

        case 1:
            return 0;

        case 2:
            return 4;

        case 3:
            return 33;

        case 4:
            return 66;

        case 5:
            return 99;
    }
}

void func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3) // 0x80057228
{
    q23_8 posX;
    q23_8 posY;
    q23_8 posZ;

    gte_SetRotMatrix_custom(mat);

    gte_ldv0(arg2);
    gte_rtv0();
    gte_lddp(alpha);
    gte_gpf12();
    gte_stsv(&g_WorldEnvWork.field_74);

    // Divide `arg3` by 16 and subtract matrix translation.
    posX = Q12_TO_Q8(arg3->vx) - mat->t[0];
    posY = Q12_TO_Q8(arg3->vy) - mat->t[1];
    posZ = Q12_TO_Q8(arg3->vz) - mat->t[2];

    gte_LoadVector0_XYZ(posX, posY, posZ);
    gte_rtv0();
    gte_stsv(&g_WorldEnvWork.field_7C);
}

void func_80057344(s_ModelInfo* modelInfo, GsOT_TAG* otTag, bool arg2, MATRIX* mat) // 0x80057344
{
    u32               normalOffset;
    u32               vertOffset;
    s_MeshHeader*     curMeshHdr;
    s_ModelHeader*    modelHdr;
    s_GteScratchData* scratchData;

    scratchData = PSX_SCRATCH_ADDR(0);

    modelHdr     = modelInfo->modelHdr;
#ifdef SH_PC_PORT
    if (modelHdr == NULL || modelHdr->meshHdrs == NULL) {
        return;
    }
#endif
    vertOffset   = modelHdr->vertexOffset;
    normalOffset = modelHdr->normalOffset;

    gte_lddqa(g_WorldEnvWork.field_4C);
    gte_lddqb_0();

    for (curMeshHdr = modelHdr->meshHdrs; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        if (vertOffset != 0 || normalOffset != 0)
        {
            func_8005759C(curMeshHdr, scratchData, vertOffset, normalOffset);
        }
        else
        {
            func_800574D4(curMeshHdr, scratchData);
        }

        switch (g_WorldEnvWork.field_0)
        {
            case 0:
                break;

            case 1:
                func_80057658(curMeshHdr, normalOffset, scratchData, &g_WorldEnvWork.field_74, &g_WorldEnvWork.field_7C);
                break;

            case 2:
                func_80057A3C(curMeshHdr, normalOffset, scratchData, &g_WorldEnvWork.field_74);
                break;
        }

        func_80057B7C(curMeshHdr, vertOffset, scratchData, mat);

        Gfx_MeshDraw(curMeshHdr, scratchData, otTag, arg2);
    }

}

void func_800574D4(s_MeshHeader* meshHdr, s_GteScratchData* scratchData) // 0x800574D4
{
    DVECTOR* vertexXy;
    s16*     vertexZ;
    DVECTOR* screenXy;
    s16*     var_a2;
    u8*      unkPtr;
    u8*      unkPtrDest;

    screenXy = &scratchData->screenXy_0[0];
    var_a2   = &scratchData->field_18C[0]; // `screenZ`? There's already an earlier struct field though.
    vertexXy = &meshHdr->verticesXy[0];
    vertexZ  = &meshHdr->verticesZ[0];

    while (var_a2 < &scratchData->field_18C[meshHdr->vertexCount])
    {
        *(u32*)screenXy++ = *(u32*)vertexXy++;

        *(u32*)var_a2 = *(u32*)vertexZ;
        vertexZ += 2;
        var_a2 += 2;
    }

    while (screenXy < &scratchData->screenXy_0[meshHdr->vertexCount])
    {
        *(u32*)screenXy++ = *(u32*)vertexXy++;
    }

    unkPtr     = &meshHdr->unkPtr_14[0];
    unkPtrDest = &scratchData->field_2B8[0];

    while (unkPtrDest < &scratchData->field_2B8[meshHdr->unkCount_3])
    {
        *(u32*)unkPtrDest = *(u32*)unkPtr;
        unkPtr += 4;
        unkPtrDest += 4;
    }
}

void func_8005759C(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 vertOffset, s32 normalOffset) // 0x8005759C
{
    s16* vertexZPtr;
    s16* field_18CPtr;
    s32* vertexXyPtr;
    s32* screenXyPtr;
    u8*  field_2B8Ptr;
    u8*  field_14Ptr;

    // Should be loop? Tried but no luck.
    screenXyPtr  = &scratchData->screenXy_0[vertOffset];
    field_18CPtr = &scratchData->field_18C[vertOffset];
    vertexXyPtr  = meshHdr->verticesXy;
    vertexZPtr   = meshHdr->verticesZ;
    while (vertexXyPtr < &meshHdr->verticesXy[meshHdr->vertexCount])
    {
        *screenXyPtr++  = *vertexXyPtr++;
        *field_18CPtr++ = *vertexZPtr++;
    }

    field_14Ptr  = meshHdr->unkPtr_14;
    field_2B8Ptr = &scratchData->field_2B8[normalOffset];
    while (field_14Ptr < &meshHdr->unkPtr_14[meshHdr->unkCount_3])
    {
        *field_2B8Ptr++ = *field_14Ptr++;
    }
}

void func_80057658(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* arg3, SVECTOR* arg4) // 0x80057658
{
    s32       geomOffsetX;
    s32       geomOffsetY;
    s32       geomScreen;
    s32       temp_t9;
    s32       var_a1;
    s32       var_v1;
    s32       temp_t2;
    u8        temp_v1;
    u8*       end;
    u8*       var_t0;
    s16*      temp_t8;
    s32*      depthP;
    MATRIX*   mat;
    DVECTOR*  screenPos;
    s_Normal* curNormal;

    scratchData->field_3AC = *arg4; // 3AC changed to `SVECTOR`.

    scratchData->field_380.field_0.m[0][0] = arg3->vx;
    scratchData->field_380.field_0.m[0][1] = arg3->vy;
    scratchData->field_380.field_0.m[0][2] = arg3->vz;

    gte_ldtr_0();

    ReadGeomOffset(&geomOffsetX, &geomOffsetY);
    geomScreen = ReadGeomScreen();
    SetGeomOffset(-1024, -1024);
    SetGeomScreen(16);

    temp_t9 = g_WorldEnvWork.field_20 >> 5;
    temp_v1 = g_WorldEnvWork.field_3;

    var_t0 = &scratchData->field_2B8[offset];
    mat    = &scratchData->field_380.field_0;

    for (curNormal = meshHdr->normals; curNormal < &meshHdr->normals[meshHdr->normalCount]; curNormal++)
    {
        temp_t8   = &scratchData->field_380.field_0.m[2][0];
        screenPos = &scratchData->screenPos_3A4;
        depthP    = &scratchData->depthP_3A8;
        temp_t2   = temp_v1;

        *(u32*)&scratchData->field_3A0 = *(u32*)curNormal;

        mat->m[1][0] = scratchData->field_3A0.nx << 5;
        mat->m[1][1] = scratchData->field_3A0.ny << 5;
        mat->m[1][2] = scratchData->field_3A0.nz << 5;
        gte_SetRotMatrix_Row0_1(mat->m);

        end = &var_t0[scratchData->field_3A0.count];

        mat->m[2][0] = scratchData->screenXy_0[*var_t0].vx - scratchData->field_3AC.vx;
        mat->m[2][1] = scratchData->screenXy_0[*var_t0].vy - scratchData->field_3AC.vy;
        mat->m[2][2] = scratchData->field_18C[*var_t0] - scratchData->field_3AC.vz;
        gte_SetRotMatrix_Row2(mat->m);

        gte_ldv0(temp_t8);
        gte_rtps();
        gte_stsxy(screenPos);
        gte_stdp(depthP);

        while (++var_t0 < end)
        {
            mat->m[2][0] = scratchData->screenXy_0[*var_t0].vx - scratchData->field_3AC.vx;
            mat->m[2][1] = scratchData->screenXy_0[*var_t0].vy - scratchData->field_3AC.vy;
            mat->m[2][2] = scratchData->field_18C[*var_t0] - scratchData->field_3AC.vz;

            gte_SetRotMatrix_Row2(mat->m);
            gte_ldv0(temp_t8);
            gte_rtps();

            scratchData->screenPos_3A4.vx += 1024;
            scratchData->screenPos_3A4.vy += 1024;

            var_a1   = (scratchData->screenPos_3A4.vx * scratchData->screenPos_3A4.vy) + (scratchData->screenPos_3A4.vy * (scratchData->depthP_3A8 >> 4));
            var_a1 >>= 5;
            var_a1  -= 16;
            if (var_a1 < 0)
            {
                var_a1 = 0;
            }

            var_v1 = (scratchData->screenPos_3A4.vx >> 1) + (scratchData->depthP_3A8 >> 2);
            var_a1 += temp_t9;
            if (var_v1 > 48)
            {
                var_v1 = 48;
            }
            var_a1 += var_v1;

            if (temp_t2 < var_a1)
            {
                var_a1 = temp_t2;
            }

            *(var_t0 - 1) = var_a1;

            gte_stsxy(screenPos);
            gte_stdp(depthP);
        }

        scratchData->screenPos_3A4.vx += 1024;
        scratchData->screenPos_3A4.vy += 1024;

        var_a1   = (scratchData->screenPos_3A4.vx * scratchData->screenPos_3A4.vy) + (scratchData->screenPos_3A4.vy * (scratchData->depthP_3A8 >> 4));
        var_a1 >>= 5;
        var_a1  -= 16;
        if (var_a1 < 0)
        {
            var_a1 = 0;
        }

        var_v1  = (scratchData->screenPos_3A4.vx >> 1) + (scratchData->depthP_3A8 >> 2);
        var_a1 += temp_t9;
        if (var_v1 > 48)
        {
            var_v1 = 48;
        }
        var_a1 += var_v1;

        if (temp_t2 < var_a1)
        {
            var_a1 = temp_t2;
        }

        *(var_t0 - 1) = var_a1;
    }

    SetGeomOffset(geomOffsetX, geomOffsetY);
    SetGeomScreen(geomScreen);
}

void func_80057A3C(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* lightVec) // 0x80057A3C
{
    s32       var_v1;
    s32       temp_t2;
    u8*       var_a3;
    void*     endPtr;
    s_Normal* normal;

    scratchData->field_380.field_0.m[0][0] = lightVec->vx;
    scratchData->field_380.field_0.m[0][1] = lightVec->vy;
    scratchData->field_380.field_0.m[0][2] = lightVec->vz;
    gte_SetLightMatrix(&scratchData->field_380.field_0);

    var_a3  = &scratchData->field_2B8[offset];
    temp_t2 = g_WorldEnvWork.field_20;

    for (normal = meshHdr->normals; normal < &meshHdr->normals[meshHdr->normalCount]; normal++)
    {
        *(u32*)&scratchData->field_3A0 = *(u32*)normal;

        scratchData->field_3AC.vx = scratchData->field_3A0.nx << 5;
        scratchData->field_3AC.vy = scratchData->field_3A0.ny << 5;
        scratchData->field_3AC.vz = scratchData->field_3A0.nz << 5;

        gte_ldv0(&scratchData->field_3AC);
        gte_ll();

        var_v1   = gte_stIR1();
        var_v1  += temp_t2;
        var_v1 >>= 5;

        if (var_v1 > 0xFF)
        {
            var_v1 = 0xFF;
        }

        endPtr = &var_a3[scratchData->field_3A0.count];
        for (; var_a3 < endPtr; var_a3++)
        {
            *var_a3 = var_v1;
        }
    }
}

void func_80057B7C(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat) // 0x80057B7C
{
    DVECTOR* screenXy;
    s16*     temp_a2;
    s32      temp_s2;
    u8*      var_t1;

    temp_s2 = g_WorldEnvWork.fog.depthShift;

#ifdef SH_PC_PORT
    /* The per-vertex depths below live in s16 slots; a GTE SZ >= 32768
     * reads back negative, passes the `< (1 << depthShift)` fog test and
     * indexes fogRamp[] with a large NEGATIVE value — garbage fog bytes
     * that corrupted character vertex colors (this was the reason Harry's
     * gameplay render disabled fog entirely). Read the depth as u16:
     * negative-as-stored is just "very far" -> full fog, and the taken
     * index is always 0..127. */
    #define PC_FOG_VTX_RAMP(zv) \
        (((u16)(zv)) < (1u << temp_s2) ? g_WorldEnvWork.fogRamp[(((u32)(u16)(zv)) << 7) >> temp_s2] : 0xFF)
#endif

    SetRotMatrix(mat);
    SetTransMatrix(mat);

    screenXy = &scratchData->screenXy_0[offset];

    *(s32*)&scratchData->field_380.field_0.m[0][0] = *(s32*)&screenXy[0];
    *(s32*)&scratchData->field_380.field_0.m[1][1] = *(s32*)&screenXy[1];
    *(s32*)&scratchData->field_380.field_0.m[2][2] = *(s32*)&screenXy[2];

    temp_a2 = &scratchData->field_18C[offset];

    scratchData->field_380.field_0.m[0][2]      = temp_a2[0];
    scratchData->field_380.field_0.m[2][0]      = temp_a2[1];
    *(s16*)&scratchData->field_380.field_0.t[0] = temp_a2[2];

    var_t1 = &scratchData->field_252[offset];

    gte_ldv3c(&scratchData->field_380.field_0);
    gte_rtpt();
    gte_stsxy3c(screenXy);
    gte_stsz3(&scratchData->field_380.field_0.m[0][2], &scratchData->field_380.field_0.m[2][0], &scratchData->field_380.field_0.t[0]);


    temp_a2[0] = scratchData->field_380.field_0.m[0][2];
    temp_a2[1] = scratchData->field_380.field_0.m[2][0];
    temp_a2[2] = scratchData->field_380.field_0.t[0];

    screenXy += 3;
    var_t1  += 3;
    temp_a2 += 3;

    if (g_WorldEnvWork.isFogEnabled)
    {
        for (;
             screenXy < &scratchData->screenXy_0[meshHdr->vertexCount + offset];
             screenXy += 3, temp_a2 += 3, var_t1 += 3)
        {
            *(s32*)&scratchData->field_380.field_0.m[0][0] = *(s32*)&screenXy[0];
            *(s32*)&scratchData->field_380.field_0.m[1][1] = *(s32*)&screenXy[1];
            *(s32*)&scratchData->field_380.field_0.m[2][2] = *(s32*)&screenXy[2];

            scratchData->field_380.field_0.m[0][2]      = temp_a2[0];
            scratchData->field_380.field_0.m[2][0]      = temp_a2[1];
            *(s16*)&scratchData->field_380.field_0.t[0] = temp_a2[2];

            gte_ldv3c(&scratchData->field_380.field_0);
            gte_rtpt();

#ifdef SH_PC_PORT
            var_t1[-3] = PC_FOG_VTX_RAMP(temp_a2[-3]);
            var_t1[-2] = PC_FOG_VTX_RAMP(temp_a2[-2]);
            var_t1[-1] = PC_FOG_VTX_RAMP(temp_a2[-1]);
#else
            var_t1[-3] = temp_a2[-3] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-3] << 7) >> temp_s2] : 0xFF;
            var_t1[-2] = temp_a2[-2] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-2] << 7) >> temp_s2] : 0xFF;
            var_t1[-1] = temp_a2[-1] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-1] << 7) >> temp_s2] : 0xFF;
#endif

            gte_stsxy3c(screenXy);
            gte_stsz3(&scratchData->field_380.field_0.m[0][2], &scratchData->field_380.field_0.m[2][0], &scratchData->field_380.field_0.t[0]);

            temp_a2[0] = scratchData->field_380.field_0.m[0][2];
            temp_a2[1] = scratchData->field_380.field_0.m[2][0];
            temp_a2[2] = scratchData->field_380.field_0.t[0];
        }

#ifdef SH_PC_PORT
        var_t1[-3] = PC_FOG_VTX_RAMP(temp_a2[-3]);
        var_t1[-2] = PC_FOG_VTX_RAMP(temp_a2[-2]);
        var_t1[-1] = PC_FOG_VTX_RAMP(temp_a2[-1]);
#else
        var_t1[-3] = temp_a2[-3] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-3] << 7) >> temp_s2] : 0xFF;
        var_t1[-2] = temp_a2[-2] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-2] << 7) >> temp_s2] : 0xFF;
        var_t1[-1] = temp_a2[-1] < (1 << temp_s2) ? g_WorldEnvWork.fogRamp[(temp_a2[-1] << 7) >> temp_s2] : 0xFF;
#endif
    }
    else
    {
        for (; screenXy < &scratchData->screenXy_0[meshHdr->vertexCount + offset]; screenXy += 3, temp_a2 += 3)
        {
            *(s32*)&scratchData->field_380.field_0.m[0][0] = *(s32*)&screenXy[0];
            *(s32*)&scratchData->field_380.field_0.m[1][1] = *(s32*)&screenXy[1];
            *(s32*)&scratchData->field_380.field_0.m[2][2] = *(s32*)&screenXy[2];

            scratchData->field_380.field_0.m[0][2]      = temp_a2[0];
            scratchData->field_380.field_0.m[2][0]      = temp_a2[1];
            *(s16*)&scratchData->field_380.field_0.t[0] = temp_a2[2];

            gte_ldv3c(&scratchData->field_380.field_0);
            gte_rtpt();
            gte_stsxy3c(screenXy);
            gte_stsz3(&scratchData->field_380.field_0.m[0][2], &scratchData->field_380.field_0.m[2][0], &scratchData->field_380.field_0.t[0]);

            temp_a2[0] = scratchData->field_380.field_0.m[0][2];
            temp_a2[1] = scratchData->field_380.field_0.m[2][0];
            temp_a2[2] = scratchData->field_380.field_0.t[0];
        }
    }
}

void Gfx_MeshDraw(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, GsOT_TAG* tag, bool arg3) // 0x8005801C
{
    s32          sp10;
    s32          sp14;
    s32          sp18;
    s32          sp1C;
    s32          sp20;
    s32          var_t3;
    s32          var_t3_2;
    s32          temp_a2;
    s32          temp_a2_3;
    s32          temp_a2_4;
    s32          temp_a2_5;
    s32          temp_a2_7;
    s32          temp_a0;
    s32          temp_a0_13;
    s32          temp_a0_5;
    s32          temp_a0_7;
    s32          temp_a0_9;
    s32          temp_a1;
    s32          temp_a1_2;
    s32          temp_a1_3;
    s32          temp_a1_4;
    s32          temp_a1_5;
    s32          temp_a2_2;
    s32          temp_a2_6;
    s32          temp_a3;
    s32          temp_a3_2;
    s32          temp_a3_3;
    s32          temp_a3_4;
    s32          temp_a3_5;
    s32          temp_v1;
    s32          temp_v1_11;
    s32          temp_v1_16;
    s32          temp_v1_21;
    s32          temp_v1_27;
    s32          temp_v1_5;
    u32          temp_t0;
    u32          temp_t0_2;
    u32          temp_t0_3;
    u32          temp_t0_4;
    u32          temp_t0_5;
    s32          temp;
    s32          temp2;
    s32          temp3;
    s32          temp4;
    s_Primitive* prim;
    PACKET*      packet0;
    PACKET*      packet1;
    POLY_GT4*    poly0;
    POLY_G4*     poly1;
    POLY_G4*     poly2;
    POLY_GT4*    poly3;
    POLY_FT4*    poly4;


    temp_v1 = 0x79C << (arg3 + 2);

    if (!g_WorldEnvWork.isFogEnabled)
    {
        scratchData->field_380.s_0.field_1C = temp_v1;
    }
    else
    {
        scratchData->field_380.s_0.field_1C = FOG_FAR_DIST();

        if (temp_v1 < scratchData->field_380.s_0.field_1C)
        {
            scratchData->field_380.s_0.field_1C = temp_v1;
        }
    }
    SH_WHOLEMAP_FARCAP(scratchData->field_380.s_0.field_1C);

#ifdef SH_PC_PORT
    /* Hor+ widescreen: per-polygon visibility bound. PSX uses raw
     * screenWidth/2 (=160 in 4:3); 16:9 windows need a wider bound
     * (psxH * winW / (2 * winH)) so polygons in the extension band
     * don't get silently culled (gives 213 for 1920x1080). Without
     * this hor+ shows holes in geometry near the screen edges.
     *
     * For windows TALLER than 4:3 (e.g. 5:4 = 1280x1024) the formula
     * yields HALF-WIDTH < 160, which over-culls compared to PSX:
     * polys at the rightmost / leftmost ~10 px of the PSX viewport get
     * dropped even though they'd render on real hardware. Clamp to
     * AT LEAST PSX's half-width so taller windows still see the full
     * native FOV (with ortho stretching to fill vertically). */
    {
        /* Multiply by PSX_NTSC_PIXEL_ASPECT (1.09375) to match
         * PsyCross's hor+ ortho — see PsyX_render.cpp commit 9c502de.
         * Without this factor, polygons in the ~9.4% horizontal slice
         * exposed by the pixel-aspect compensation get culled by this
         * per-poly visibility bound even though they'd render. This is
         * the actual source of the persistent edge-of-screen "missing
         * triangle" culling reported across multiple test sessions
         * since the pixel-aspect fix landed. */
        const s32   psxHalfW     = g_GameWork.gsScreenWidth >> 1;
        const float visibleHalfW = (g_PcConfig.windowHeight > 0)
            ? ((float)g_GameWork.gsScreenHeight * (float)g_PcConfig.windowWidth /
               (2.0f * (float)g_PcConfig.windowHeight)) * g_PsxPixelAspect
            : (float)psxHalfW;
        s32 halfW = (s32)(visibleHalfW + 16.5f);
        if (halfW < psxHalfW) halfW = psxHalfW;
        scratchData->field_380.s_0.field_0 = halfW;
    }
#else
    scratchData->field_380.s_0.field_0    = g_GameWork.gsScreenWidth >> 1;
#endif
    scratchData->field_380.s_0.field_4    = g_WorldEnvWork.fog.intensity;
    scratchData->field_380.s_0.field_8    = g_WorldEnvWork.worldTintColor;
    scratchData->field_380.s_0.field_8.cd = 0x3C;


    if (g_WorldEnvWork.field_0 == 0)
    {
        gte_lddp(0x1000 - g_WorldEnvWork.field_20);
        gte_ldrgb(&scratchData->field_380.s_0.field_8);
        gte_dpcs();
        gte_strgb(&scratchData->field_380.s_0.field_8);
    }


    scratchData->field_380.s_0.field_C    = g_WorldEnvWork.fog.color;
    scratchData->field_380.s_0.field_C.cd = 0x38;

    SetBackColor(0, 0, 0);


    prim = meshHdr->primitives;

    if (g_WorldEnvWork.field_0 != 0)
    {
        if (g_WorldEnvWork.isFogEnabled != 0)
        {
            if (*(s32*)&scratchData->field_380.s_0.field_C & 0xFFFFFF)
            {
                poly3 = GsOUT_PACKET_P;
                poly1  = poly3 + 1;

                for (; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
                {
                    *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;
#ifdef SH_PC_PORT
                    PsyX_SetNextPrimSz(
                        (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_10],
                        (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_11],
                        (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_12],
                        (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_13],
                        arg3
                    );
#endif
                    scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_10];

                    if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_11])
                    {
                        scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_11];
                    }

                    if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_12])
                    {
                        scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_12];
                    }

                    if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_13])
                    {
                        scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_13];
                    }

                    SH_WHOLEMAP_DEPTH_RESCUE(scratchData->field_380.s_0.field_18, arg3);
                    if (scratchData->field_380.s_0.field_18 <= 0)
                    {
                        continue;
                    }

                    if (scratchData->field_380.s_0.field_18 <= 32)
                    {
                        scratchData->field_380.s_0.field_18 = 32;
                    }

                    SH_CLAMP_OT_DEPTH(scratchData->field_380.s_0.field_18, arg3);

                    if (scratchData->field_380.s_0.field_18 > scratchData->field_380.s_0.field_1C)
                    {
                        continue;
                    }

#ifdef SH_PC_PORT
                    if (SH_WHOLEMAP_FAR_POLY(scratchData->field_380.s_0.field_18, arg3))
                    {
                        /* Far poly: integer NCLIP collapses sub-pixel-tall distant
                         * ground to zero area and culls it. Re-test on the PGXP
                         * float projection (integer fallback = same semantics). */
                        s32 _n012 = 0, _n312 = 0;
                        extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
                        gte_NormalClip(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12], &_n012);
                        gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                        gte_nclip();
                        gte_stopz(&_n312);
                        if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                                   &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                                   &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                                   &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                                   _n012, _n312))
                        {
                            continue;
                        }
                    }
                    else
#endif
                    {
                    gte_NormalClip(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12], &sp10);

                    if (sp10 <= 0)
                    {
                        gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                        gte_nclip();
                        gte_stopz(&sp10);

                        if (sp10 >= 0)
                        {
                            continue;
                        }
                    }
                    }

                    temp_a3 = scratchData->field_380.s_0.field_0;
                    temp_a2 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];

                    temp_a1   = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
                    temp_a0   = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
                    temp_v1_5 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];
                    temp_t0   = temp_a3 * 2;
                    temp_a2_2 = temp_a2;

                    if ((s16)temp_a2 + temp_a3 < temp_t0 || (s16)temp_a1 + temp_a3 < temp_t0 ||
                        (s16)temp_a0 + temp_a3 < temp_t0 || (s16)temp_v1_5 + temp_a3 < temp_t0)
                    {
                        *(s32*)&poly3->x0 = temp_a2_2;
                        *(s32*)&poly1->x0  = temp_a2_2;
                        *(s32*)&poly3->x1 = temp_a1;
                        *(s32*)&poly1->x1  = temp_a1;
                        *(s32*)&poly3->x2 = temp_a0;
                        *(s32*)&poly1->x2  = temp_a0;
                        *(s32*)&poly3->x3 = temp_v1_5;
                        *(s32*)&poly1->x3  = temp_v1_5;
#ifdef SH_PC_PORT
                        SH_PGXP_PROP4(scratchData, poly3,
                            scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
                            scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

                        *(s32*)&scratchData->field_380.s_0.field_14 = *(s32*)&prim->field_10;

                        var_t3  = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_10] * 16;
                        var_t3 -= scratchData->field_380.s_0.field_4;
                        if (var_t3 < 0)
                        {
                            var_t3 = 0;
                        }

                        gte_lddp(var_t3);
                        gte_ldrgb(&scratchData->field_380.s_0.field_C);
                        gte_dpcs();
                        gte_strgb(&poly1->r0);
                        VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_10] << 4);
                        gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_14] << 5);
                        gte_ldrgb(&scratchData->field_380.s_0.field_8);
                        gte_dpcl();
                        gte_strgb(&poly3->r0);

                        var_t3  = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_11] * 16;
                        var_t3 -= scratchData->field_380.s_0.field_4;
                        if (var_t3 < 0)
                        {
                            var_t3 = 0;
                        }

                        gte_lddp(var_t3);
                        gte_ldrgb(&scratchData->field_380.s_0.field_C);
                        gte_dpcs();
                        gte_strgb(&poly1->r1);
                        VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_11] << 4);
                        gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_15] << 5);
                        gte_ldrgb(&scratchData->field_380.s_0.field_8);
                        gte_dpcl();
                        gte_strgb(&poly3->r1);

                        var_t3  = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_12] * 0x10;
                        var_t3 -= scratchData->field_380.s_0.field_4;
                        if (var_t3 < 0)
                        {
                            var_t3 = 0;
                        }

                        gte_lddp(var_t3);
                        gte_ldrgb(&scratchData->field_380.s_0.field_C);
                        gte_dpcs();
                        gte_strgb(&poly1->r2);
                        VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_12] << 4);
                        gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_16] << 5);
                        gte_ldrgb(&scratchData->field_380.s_0.field_8);
                        gte_dpcl();
                        gte_strgb(&poly3->r2);

                        var_t3  = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_13] * 0x10;
                        var_t3 -= scratchData->field_380.s_0.field_4;
                        if (var_t3 < 0)
                        {
                            var_t3 = 0;
                        }

                        gte_lddp(var_t3);
                        gte_ldrgb(&scratchData->field_380.s_0.field_C);
                        gte_dpcs();
                        gte_strgb(&poly1->r3);
                        VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_13] << 4);
                        gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_17] << 5);
                        gte_ldrgb(&scratchData->field_380.s_0.field_8);
                        gte_dpcl();
                        gte_strgb(&poly3->r3);

                        *(s32*)&poly3->u0 = *(s32*)&prim->field_0;
                        *(s32*)&poly3->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
                        *(u16*)&poly3->u2 = prim->field_8;
                        *(u16*)&poly3->u3 = prim->field_A;

                        setlen(poly3, 12);
                        setlen(poly1, 8);

                        if (prim->field_6.flags & 0x8000)
                        {
#ifdef SH_PC_PORT
                            /* PC: Skip fog overlay + SetPriority packets.
                             * Encode fog factor for shader fog blending. */
                            PC_FACE_FOG_VERTS(scratchData);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);

                            poly3 = (PACKET*)(poly1 + 1) + 0xC + 0xC;
                            poly1  = poly3 + 1;
#else
                            packet1 = poly1 + 1;

                            SetPriority(packet1, 0, 0);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], packet1);

                            setSemiTrans(poly1, 1);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly1);

                            packet1 = (PACKET*)(poly1 + 1) + 0xC;
                            SetPriority(packet1, 1, 1);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], packet1);

                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);

                            poly3 = (PACKET*)(poly1 + 1) + 0xC + 0xC;
                            poly1  = poly3 + 1;
#endif
                        }
                        else
                        {
#ifdef SH_PC_PORT
                            /* PC: Render opaque, skip fog overlay.
                             * Encode per-vertex fog for shader blending. */
                            PC_FACE_FOG_VERTS(scratchData);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);
#else
                            setSemiTrans(poly3, 1);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);
                            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly1);
#endif
                            poly3 = poly1 + 1;
                            poly1  = poly3 + 1;
                        }
                    }
                }

                GsOUT_PACKET_P = poly1; // @bug? Should be `poly_gt4`
                return;
            }

            poly3 = GsOUT_PACKET_P;

            for (; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
            {
                *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;
#ifdef SH_PC_PORT
                PsyX_SetNextPrimSz(
                    (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_10],
                    (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_11],
                    (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_12],
                    (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_13],
                    arg3
                );
#endif
                scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_10];

                if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_11])
                {
                    scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_11];
                }

                if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_12])
                {
                    scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_12];
                }

                if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_13])
                {
                    scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_13];
                }

                SH_WHOLEMAP_DEPTH_RESCUE(scratchData->field_380.s_0.field_18, arg3);
                if (scratchData->field_380.s_0.field_18 <= 0)
                {
                    continue;
                }

                if (scratchData->field_380.s_0.field_18 <= 32)
                {
                    scratchData->field_380.s_0.field_18 = 32;
                }

                SH_CLAMP_OT_DEPTH(scratchData->field_380.s_0.field_18, arg3);

                if (scratchData->field_380.s_0.field_18 > scratchData->field_380.s_0.field_1C)
                {
                    continue;
                }

#ifdef SH_PC_PORT
                if (SH_WHOLEMAP_FAR_POLY(scratchData->field_380.s_0.field_18, arg3))
                {
                    /* Far poly: precise backface re-test (see loop A). */
                    s32 _n012 = 0, _n312 = 0;
                    extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
                    gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                               *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                               *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
                    gte_nclip();
                    gte_stopz(&_n012);
                    gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                    gte_nclip();
                    gte_stopz(&_n312);
                    if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                               &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                               &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                               &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                               _n012, _n312))
                    {
                        continue;
                    }
                }
                else
#endif
                {
                gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
                gte_nclip();
                gte_stopz(&sp14);

                if (sp14 <= 0)
                {
                    gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                    gte_nclip();
                    gte_stopz(&sp14);

                    if (sp14 >= 0)
                    {
                        continue;
                    }
                }
                }

                temp_a3_2 = scratchData->field_380.s_0.field_0;
                temp_a2_3 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];

                temp_a1_2  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
                temp_a0_5  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
                temp_v1_11 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];
                temp_t0_2  = temp_a3_2 * 2;
                temp2      = temp_a2_3;

                if ((s16)temp_a2_3 + temp_a3_2 < temp_t0_2 || (s16)temp_a1_2 + temp_a3_2 < temp_t0_2 ||
                    (s16)temp_a0_5 + temp_a3_2 < temp_t0_2 || (s16)temp_v1_11 + temp_a3_2 < temp_t0_2)
                {
                    *(s32*)&poly3->x0 = temp2;
                    *(s32*)&poly3->x1 = temp_a1_2;
                    *(s32*)&poly3->x2 = temp_a0_5;
                    *(s32*)&poly3->x3 = temp_v1_11;
#ifdef SH_PC_PORT
                    SH_PGXP_PROP4(scratchData, poly3,
                        scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
                        scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

                    *(s32*)&scratchData->field_380.s_0.field_14 = *(s32*)&prim->field_10;

                    VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_10] << 4);
                    gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_14] << 5);
                    gte_ldrgb(&scratchData->field_380.s_0.field_8);
                    gte_dpcl();
                    gte_strgb(&poly3->r0);

                    VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_11] << 4);
                    gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_15] << 5);
                    gte_ldrgb(&scratchData->field_380.s_0.field_8);
                    gte_dpcl();
                    gte_strgb(&poly3->r1);

                    VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_12] << 4);
                    gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_16] << 5);
                    gte_ldrgb(&scratchData->field_380.s_0.field_8);
                    gte_dpcl();
                    gte_strgb(&poly3->r2);

                    VTXCOL_LDDP(scratchData->field_252[scratchData->field_380.s_0.field_13] << 4);
                    gte_ldsv_(scratchData->field_2B8[scratchData->field_380.s_0.field_17] << 5);
                    gte_ldrgb(&scratchData->field_380.s_0.field_8);
                    gte_dpcl();
                    gte_strgb(&poly3->r3);

                    *(s32*)&poly3->u0 = *(s32*)&prim->field_0;
                    *(s32*)&poly3->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
                    *(u16*)&poly3->u2 = prim->field_8;
                    *(u16*)&poly3->u3 = prim->field_A;

                    setlen(poly3, 12);
#ifdef SH_PC_PORT
                    PC_FACE_FOG_VERTS(scratchData);
#endif
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);
                    poly3++;
                }
            }

            GsOUT_PACKET_P = poly3;
            return;
        }
        else
        {
            goto __block1530;
        }
    }

    if (g_WorldEnvWork.isFogEnabled != 0)
    {
        poly3  = GsOUT_PACKET_P;
        poly2 = poly3 + 1;

        for (; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
        {
            *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;
#ifdef SH_PC_PORT
            PsyX_SetNextPrimSz(
                (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_10],
                (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_11],
                (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_12],
                (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_13],
                arg3
            );
#endif
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_10];

            if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_11])
            {
                scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_11];
            }

            if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_12])
            {
                scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_12];
            }

            if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_13])
            {
                scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_13];
            }

            SH_WHOLEMAP_DEPTH_RESCUE(scratchData->field_380.s_0.field_18, arg3);
            if (scratchData->field_380.s_0.field_18 <= 0)
            {
                continue;
            }

            if (scratchData->field_380.s_0.field_18 < 0x21)
            {
                scratchData->field_380.s_0.field_18 = 0x20;
            }

            SH_CLAMP_OT_DEPTH(scratchData->field_380.s_0.field_18, arg3);

            if (scratchData->field_380.s_0.field_18 > scratchData->field_380.s_0.field_1C)
            {
                continue;
            }

#ifdef SH_PC_PORT
            if (SH_WHOLEMAP_FAR_POLY(scratchData->field_380.s_0.field_18, arg3))
            {
                /* Far poly: precise backface re-test (see loop A). */
                s32 _n012 = 0, _n312 = 0;
                extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
                gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
                gte_nclip();
                gte_stopz(&_n012);
                gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                gte_nclip();
                gte_stopz(&_n312);
                if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                           &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                           &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                           &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                           _n012, _n312))
                {
                    continue;
                }
            }
            else
#endif
            {
            gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
            gte_nclip();
            gte_stopz(&sp18);

            if (sp18 <= 0)
            {
                gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
                gte_nclip();
                gte_stopz(&sp18);

                if (sp18 >= 0)
                {
                    continue;
                }
            }
            }

            temp_a3_4 = scratchData->field_380.s_0.field_0;
            temp_a2_5 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];

            temp_a1_4  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
            temp_a0_9  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
            temp_v1_21 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];
            temp_t0_4  = temp_a3_4 * 2;
            temp_a2_6  = temp_a2_5;

            if ((s16)temp_a2_5 + temp_a3_4 < temp_t0_4 || (s16)temp_a1_4 + temp_a3_4 < temp_t0_4 ||
                (s16)temp_a0_9 + temp_a3_4 < temp_t0_4 || (s16)temp_v1_21 + temp_a3_4 < temp_t0_4)
            {
                *(s32*)&poly3->x0  = temp_a2_6;
                *(s32*)&poly2->x0 = temp_a2_6;
                *(s32*)&poly3->x1  = temp_a1_4;
                *(s32*)&poly2->x1 = temp_a1_4;
                *(s32*)&poly3->x2  = temp_a0_9;
                *(s32*)&poly2->x2 = temp_a0_9;
                *(s32*)&poly3->x3  = temp_v1_21;
                *(s32*)&poly2->x3 = temp_v1_21;
#ifdef SH_PC_PORT
                SH_PGXP_PROP4(scratchData, poly3,
                    scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
                    scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

                temp4    = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_10] * 0x10;
                var_t3_2 = temp4 - scratchData->field_380.s_0.field_4;
                if (var_t3_2 < 0)
                {
                    var_t3_2 = 0;
                }

                gte_lddp(var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_C);
                gte_dpcs();
                gte_strgb(&poly2->r0);
                VTXCOL_LDDP(0x1000 - var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly3->r0);

                temp4    = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_11] * 0x10;
                var_t3_2 = temp4 - scratchData->field_380.s_0.field_4;
                if (var_t3_2 < 0)
                {
                    var_t3_2 = 0;
                }

                gte_lddp(var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_C);
                gte_dpcs();
                gte_strgb(&poly2->r1);
                VTXCOL_LDDP(0x1000 - var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly3->r1);

                temp4    = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_12] * 0x10;
                var_t3_2 = temp4 - scratchData->field_380.s_0.field_4;
                if (var_t3_2 < 0)
                {
                    var_t3_2 = 0;
                }

                gte_lddp(var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_C);
                gte_dpcs();
                gte_strgb(&poly2->r2);
                VTXCOL_LDDP(0x1000 - var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly3->r2);

                temp4    = 0x1000 - scratchData->field_252[scratchData->field_380.s_0.field_13] * 0x10;
                var_t3_2 = temp4 - scratchData->field_380.s_0.field_4;
                if (var_t3_2 < 0)
                {
                    var_t3_2 = 0;
                }

                gte_lddp(var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_C);
                gte_dpcs();
                gte_strgb(&poly2->r3);
                VTXCOL_LDDP(0x1000 - var_t3_2);
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly3->r3);

                *(s32*)&poly3->u0 = *(s32*)&prim->field_0;
                *(s32*)&poly3->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
                *(u16*)&poly3->u2 = prim->field_8;
                *(u16*)&poly3->u3 = prim->field_A;

                setlen(poly3, 12);
                setlen(poly2, 8);

                if (prim->field_6.flags & 0x8000)
                {
#ifdef SH_PC_PORT
                    /* PC: Skip fog overlay + SetPriority (no mask bit support).
                     * Encode fog factor in p1 for shader fog blending. */
                    PC_FACE_FOG_VERTS(scratchData);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);

                    poly3  = (PACKET*)(poly2 + 1) + 0xC + 0xC;
                    poly2 = poly3 + 1;
#else
                    packet0 = poly2 + 1;

                    SetPriority(packet0, 0, 0);

                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], packet0);
                    setSemiTrans(poly2, 1);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly2);

                    packet0 = (PACKET*)(poly2 + 1) + 0xC;
                    SetPriority(packet0, 1, 1);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], packet0);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);

                    poly3  = (PACKET*)(poly2 + 1) + 0xC + 0xC;
                    poly2 = poly3 + 1;
#endif
                }
                else
                {
#ifdef SH_PC_PORT
                    /* PC: Render opaque, skip fog overlay.
                     * Encode per-vertex fog for shader blending. */
                    PC_FACE_FOG_VERTS(scratchData);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);
#else
                    setSemiTrans(poly3, 1);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly3);
                    addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly2);
#endif
                    poly3  = poly2 + 1;
                    poly2 = poly3 + 1;
                }
            }
        }
        GsOUT_PACKET_P = poly2; // @bug? Should be `poly_gt4`
        return;
    }
    else
    {
        goto __block19CC;
    }

__block1530:
{
    poly0 = GsOUT_PACKET_P;

#ifdef SH_PC_PORT
    {
    static int _b1530Log = 0;
    int _b1530Total = 0, _b1530ZeroZ = 0, _b1530FarZ = 0, _b1530Nclip = 0, _b1530Emitted = 0;
#endif

    for (; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
    {
#ifdef SH_PC_PORT
        _b1530Total++;
#endif
        *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;
#ifdef SH_PC_PORT
        PsyX_SetNextPrimSz(
            (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_10],
            (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_11],
            (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_12],
            (unsigned short)(u16)scratchData->field_18C[scratchData->field_380.s_0.field_13],
            arg3
        );
#endif
        scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_10];

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_11])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_11];
        }

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_12])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_12];
        }

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_13])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_13];
        }

        SH_WHOLEMAP_DEPTH_RESCUE(scratchData->field_380.s_0.field_18, arg3);
        if (scratchData->field_380.s_0.field_18 <= 0)
        {
#ifdef SH_PC_PORT
            _b1530ZeroZ++;
#endif
            continue;
        }

        if (scratchData->field_380.s_0.field_18 <= 32)
        {
            scratchData->field_380.s_0.field_18 = 32;
        }

        SH_CLAMP_OT_DEPTH(scratchData->field_380.s_0.field_18, arg3);

        if (scratchData->field_380.s_0.field_18 > scratchData->field_380.s_0.field_1C)
        {
#ifdef SH_PC_PORT
            _b1530FarZ++;
#endif
            continue;
        }

#ifdef SH_PC_PORT
        if (SH_WHOLEMAP_FAR_POLY(scratchData->field_380.s_0.field_18, arg3))
        {
            /* Far poly: precise backface re-test (see loop A). */
            s32 _n012 = 0, _n312 = 0;
            extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
            gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
            gte_nclip();
            gte_stopz(&_n012);
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&_n312);
            if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                       _n012, _n312))
            {
                continue;
            }
        }
        else
#endif
        {
        gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
        gte_nclip();
        gte_stopz(&sp1C);

        if (sp1C <= 0)
        {
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&sp1C);

            if (sp1C >= 0)
            {
                continue;
            }
        }
        }

        temp_a3_3 = scratchData->field_380.s_0.field_0;
        temp_a2_4 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];

        temp_a1_3  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
        temp_a0_7  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
        temp_v1_16 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];
        temp_t0_3  = temp_a3_3 * 2;
        temp3      = temp_a2_4;

        if ((s16)temp_a2_4 + temp_a3_3 < temp_t0_3 || (s16)temp_a1_3 + temp_a3_3 < temp_t0_3 ||
            (s16)temp_a0_7 + temp_a3_3 < temp_t0_3 || (s16)temp_v1_16 + temp_a3_3 < temp_t0_3)
        {
            *(s32*)&poly0->x0 = temp3;
            *(s32*)&poly0->x1 = temp_a1_3;
            *(s32*)&poly0->x2 = temp_a0_7;
            *(s32*)&poly0->x3 = temp_v1_16;
#ifdef SH_PC_PORT
            SH_PGXP_PROP4(scratchData, poly0,
                scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
                scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

            *(s32*)&scratchData->field_380.s_0.field_14 = *(s32*)&prim->field_10;

            if (scratchData->field_2B8[scratchData->field_380.s_0.field_14] >= 8)
            {
                gte_lddp(0x1000 - (scratchData->field_2B8[scratchData->field_380.s_0.field_14] << 5));
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly0->r0);
            }
            else
            {
                *(s32*)&poly0->r0 = 0x3C000000;
            }

            if (scratchData->field_2B8[scratchData->field_380.s_0.field_15] >= 8)
            {
                gte_lddp(0x1000 - (scratchData->field_2B8[scratchData->field_380.s_0.field_15] << 5));
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly0->r1);
            }
            else
            {
                *(s32*)&poly0->r1 = 0x3C000000;
            }

            if (scratchData->field_2B8[scratchData->field_380.s_0.field_16] >= 8)
            {
                gte_lddp(0x1000 - (scratchData->field_2B8[scratchData->field_380.s_0.field_16] << 5));
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly0->r2);
            }
            else
            {
                *(s32*)&poly0->r2 = 0x3C000000;
            }

            if (scratchData->field_2B8[scratchData->field_380.s_0.field_17] >= 8)
            {
                gte_lddp(0x1000 - (scratchData->field_2B8[scratchData->field_380.s_0.field_17] << 5));
                gte_ldrgb(&scratchData->field_380.s_0.field_8);
                gte_dpcs();
                gte_strgb(&poly0->r3);
            }
            else
            {
                *(s32*)&poly0->r3 = 0x3C000000;
            }

            *(s32*)&poly0->u0 = *(s32*)&prim->field_0;
            *(s32*)&poly0->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
            *(u16*)&poly0->u2 = prim->field_8;
            *(u16*)&poly0->u3 = prim->field_A;

#ifdef SH_PC_PORT
            /* PSX command byte 0x3C lands in p1/p2/p3 which PsyCross reads as
             * per-vertex fog factor. Zero them so no-fog maps render unfogged.
             * pad2 carries v0's fog on PC — zero it too (no fog here). */
            poly0->p1 = 0;
            poly0->p2 = 0;
            poly0->p3 = 0;
            poly0->pad2 = 0;
#endif

            setlen(poly0, 12);

            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly0);
#ifdef SH_PC_PORT
            _b1530Emitted++;
#endif
            poly0++;
        }
    }
#ifdef SH_PC_PORT
    if (_b1530Log < 8 && _b1530Total > 0) {
        _b1530Log++;
    }
    }
#endif
    GsOUT_PACKET_P = poly0;
    return;
}

__block19CC:
    scratchData->field_380.s_0.field_8.cd = 0x2C;
    poly4                       = GsOUT_PACKET_P;

#ifdef SH_PC_PORT
    {
    s32 pc_primIdx = 0;
    s32 pc_emitted = 0;
#endif

    for (prim = meshHdr->primitives; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
    {
        *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;

#ifdef SH_PC_PORT
        /* 0xFF sentinel = triangle (3 verts). Replace 4th index with 1st to
         * form a degenerate quad, matching how PSX scratchpad wrap behaves. */
        if (scratchData->field_380.s_0.field_13 == 0xFF) {
            scratchData->field_380.s_0.field_13 = scratchData->field_380.s_0.field_10;
        }
        /* Bounds-check vertex indices before using them as array indices */
        if (scratchData->field_380.s_0.field_10 >= 90 || scratchData->field_380.s_0.field_11 >= 90 ||
            scratchData->field_380.s_0.field_12 >= 90 || scratchData->field_380.s_0.field_13 >= 90) {
            pc_primIdx++;
            continue;
        }
#endif

        scratchData->field_380.s_0.field_18         = scratchData->field_18C[scratchData->field_380.s_0.field_10];

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_11])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_11];
        }

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_12])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_12];
        }

        if (scratchData->field_380.s_0.field_18 < scratchData->field_18C[scratchData->field_380.s_0.field_13])
        {
            scratchData->field_380.s_0.field_18 = scratchData->field_18C[scratchData->field_380.s_0.field_13];
        }

        SH_WHOLEMAP_DEPTH_RESCUE(scratchData->field_380.s_0.field_18, arg3);
        if (scratchData->field_380.s_0.field_18 <= 0)
        {
#ifdef SH_PC_PORT
            pc_primIdx++;
#endif
            continue;
        }

        if (scratchData->field_380.s_0.field_18 < 0x21)
        {
            scratchData->field_380.s_0.field_18 = 0x20;
        }

        SH_CLAMP_OT_DEPTH(scratchData->field_380.s_0.field_18, arg3);

        if (scratchData->field_380.s_0.field_18 > scratchData->field_380.s_0.field_1C)
        {
#ifdef SH_PC_PORT
            pc_primIdx++;
#endif
            continue;
        }

#ifdef SH_PC_PORT
        {
            s32 otIdx = (scratchData->field_380.s_0.field_18 >> arg3) >> 2;
            if (otIdx >= 2048 || otIdx < 0) {
                pc_primIdx++;
                continue;
            }
        }
#endif

#ifdef SH_PC_PORT
        if (SH_WHOLEMAP_FAR_POLY(scratchData->field_380.s_0.field_18, arg3))
        {
            /* Far poly: precise backface re-test (see loop A). */
            s32 _n012 = 0, _n312 = 0;
            extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
            gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
            gte_nclip();
            gte_stopz(&_n012);
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&_n312);
            if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                       _n012, _n312))
            {
                continue;
            }
        }
        else
#endif
        {
        gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                   *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12]);
        gte_nclip();
        gte_stopz(&sp20);

        if (sp20 <= 0)
        {
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&sp20);

            if (sp20 >= 0)
            {
                continue;
            }
        }
        }

        temp_a3_5 = scratchData->field_380.s_0.field_0;
        temp_a2_7 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];

        temp_a1_5  = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
        temp_a0_13 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
        temp_v1_27 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];
        temp_t0_5  = temp_a3_5 * 2;
        temp       = temp_a2_7;


        if ((s16)temp_a2_7 + temp_a3_5 < temp_t0_5 || (s16)temp_a1_5 + temp_a3_5 < temp_t0_5 ||
            (s16)temp_a0_13 + temp_a3_5 < temp_t0_5 || (s16)temp_v1_27 + temp_a3_5 < temp_t0_5)
        {
            *(s32*)&poly4->x0 = temp;
            *(s32*)&poly4->x1 = temp_a1_5;
            *(s32*)&poly4->x2 = temp_a0_13;
            *(s32*)&poly4->x3 = temp_v1_27;
#ifdef SH_PC_PORT
            SH_PGXP_PROP4(scratchData, poly4,
                scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
                scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

            *(s32*)&poly4->r0 = *(s32*)&scratchData->field_380.s_0.field_8;

            *(s32*)&poly4->u0 = *(s32*)&prim->field_0;
            *(s32*)&poly4->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
            *(u16*)&poly4->u2 = prim->field_8;
            *(u16*)&poly4->u3 = prim->field_A;

            setlen(poly4, 9);

            addPrim(&tag[(scratchData->field_380.s_0.field_18 >> arg3) >> 2], poly4);

            poly4++;
#ifdef SH_PC_PORT
            pc_emitted++;
#endif
        }
#ifdef SH_PC_PORT
        pc_primIdx++;
#endif
    }

#ifdef SH_PC_PORT
    (void)pc_primIdx; (void)pc_emitted;
    }
#endif

    GsOUT_PACKET_P = poly4;
}

void func_80059D50(s32 arg0, s_ModelInfo* modelInfo, MATRIX* mat, bool arg3, GsOT_TAG* tag) // 0x80059D50
{
    s_GteScratchData* scratchData;
    s_MeshHeader*     curMeshHdr;
    s_ModelHeader*    modelHdr;

    scratchData = PSX_SCRATCH_ADDR(0);

    modelHdr = modelInfo->modelHdr;

    for (curMeshHdr = &modelHdr->meshHdrs[0]; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        func_800574D4(curMeshHdr, scratchData);
        func_80057B7C(curMeshHdr, 0, scratchData, mat);
        func_80059E34(arg0, curMeshHdr, scratchData, arg3, tag);
    }
}

void func_80059E34(u32 arg0, s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 arg3, GsOT_TAG* tag) // 0x80059E34
{
    s32          sp0;
    s32          var_t2;
    s32          x2;
    s32          x1;
    s32          temp_v1;
    s32          x3;
    u16          var_a2;
    s32          packedColor;
    s32          var_t9;
    u32          temp_t0;
    u32          temp_t1;
    s32          temp;
    s32          x0;
    GsOT_TAG*    tag0;
    GsOT_TAG*    tag1;
    s_Primitive* prim;
    POLY_FT4*    poly;

    tag1 = &g_OrderingTable0[g_ActiveBufferIdx].org[640];

    switch (arg0)
    {
        case 0:
        default:
            return;

        case 1:
        case 3:
            packedColor = 0x2E383838;
            var_a2 = 0x140;
            break;

        case 2:
            packedColor = 0x2E808080;
            var_a2 = 0x20;
            break;
    }

    temp_v1 = 0x79C << (arg3 + 2);
    var_t9  = g_WorldEnvWork.isFogEnabled ? MIN(temp_v1, FOG_FAR_DIST()) : temp_v1;
    SH_WHOLEMAP_FARCAP(var_t9);

    poly                        = (POLY_FT4*)GsOUT_PACKET_P;
#ifdef SH_PC_PORT
    /* Same per-poly visibility bound as Gfx_MeshDraw (line 1979) —
     * applied at the second mesh-render path. Apply hor+ +
     * pixel-aspect formula here too or polys get culled at edges
     * even with the other fix in place. */
    {
        const s32   psxHalfW     = g_GameWork.gsScreenWidth >> 1;
        const float visibleHalfW = (g_PcConfig.windowHeight > 0)
            ? ((float)g_GameWork.gsScreenHeight * (float)g_PcConfig.windowWidth /
               (2.0f * (float)g_PcConfig.windowHeight)) * g_PsxPixelAspect
            : (float)psxHalfW;
        s32 halfW = (s32)(visibleHalfW + 16.5f);
        if (halfW < psxHalfW) halfW = psxHalfW;
        scratchData->field_380.s_0.field_0 = halfW;
    }
#else
    scratchData->field_380.s_0.field_0 = g_GameWork.gsScreenWidth >> 1;
#endif

    for (prim = meshHdr->primitives; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
    {
        *(s32*)&scratchData->field_380.s_0.field_10 = *(s32*)&prim->field_C;


        var_t2 = scratchData->field_18C[scratchData->field_380.s_0.field_10];
        var_t2 = MAX(scratchData->field_18C[scratchData->field_380.s_0.field_11], var_t2);
        var_t2 = MAX(scratchData->field_18C[scratchData->field_380.s_0.field_12], var_t2);
        var_t2 = MAX(scratchData->field_18C[scratchData->field_380.s_0.field_13], var_t2);

        SH_WHOLEMAP_DEPTH_RESCUE(var_t2, arg3);
        if (var_t2 <= 0)
        {
            continue;
        }

        if (var_t2 <= 32)
        {
            var_t2 = 32;
        }

        if (var_t9 < var_t2)
        {
            continue;
        }
        SH_CLAMP_OT_DEPTH(var_t2, arg3);

#ifdef SH_PC_PORT
        if (SH_WHOLEMAP_FAR_POLY(var_t2, arg3))
        {
            /* Far poly: precise backface re-test (see Gfx_MeshDraw loop A). */
            s32 _n012 = 0, _n312 = 0;
            extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
            gte_NormalClip(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                           *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12], &_n012);
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&_n312);
            if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                                       &scratchData->screenXy_0[scratchData->field_380.s_0.field_13],
                                       _n012, _n312))
            {
                continue;
            }
        }
        else
#endif
        {
        gte_NormalClip(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11],
                       *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12],
                       &sp0);

        if (sp0 <= 0)
        {
            gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13]);
            gte_nclip();
            gte_stopz(&sp0);

            if (sp0 >= 0)
            {
                continue;
            }
        }
        }

        temp = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_10];
        x1 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_11];
        x2 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_12];
        x3 = *(s32*)&scratchData->screenXy_0[scratchData->field_380.s_0.field_13];

        temp_t0 = scratchData->field_380.s_0.field_0;
        temp_t1 = temp_t0 * 2;
        x0 = temp;

        if (scratchData->screenXy_0[scratchData->field_380.s_0.field_10].vx + temp_t0 >= temp_t1 &&
            (s16)x1 + temp_t0 >= temp_t1 &&
            (s16)x2 + temp_t0 >= temp_t1 &&
            (s16)x3 + temp_t0 >= temp_t1)
        {
            continue;
        }

        *(s32*)&poly->x0 = x0;
        *(s32*)&poly->x1 = x1;
        *(s32*)&poly->x2 = x2;
        *(s32*)&poly->x3 = x3;
#ifdef SH_PC_PORT
        SH_PGXP_PROP4(scratchData, poly,
            scratchData->field_380.s_0.field_10, scratchData->field_380.s_0.field_11,
            scratchData->field_380.s_0.field_12, scratchData->field_380.s_0.field_13);
#endif

        *(s32*)&poly->r0 = packedColor;
        *(s32*)&poly->u0 = *(s32*)&prim->field_0;
        *(s32*)&poly->u1 = ((*(u32*)&prim->field_4 & 0x1FFFFF) | (var_a2 << 16)); // Maybe `field_4` is bitfield
        *(u16*)&poly->u2 = prim->field_8;
        *(u16*)&poly->u3 = prim->field_A;

        setlen(poly, 9);

        if (arg0 == 1)
        {
            tag0 = tag1;
        }
        else
        {
            tag0 = &tag[var_t2 >> (arg3 + 2)];
        }

        addPrim(tag0, poly);
        poly++;
    }

    GsOUT_PACKET_P = (PACKET*)poly;
}

void func_8005A21C(s_ModelInfo* modelInfo, GsOT_TAG* otTag, bool arg2, MATRIX* mat) // 0x8005A21C
{
    s16               var_v1;
    u32               normalOffset;
    u32               vertOffset;
    s_ModelHeader*    modelHdr;
    s_MeshHeader*     curMeshHdr;
    s_GteScratchData* scratchData;

    scratchData = PSX_SCRATCH_ADDR(0);

#ifdef SH_PC_PORT
    modelHdr = modelInfo->modelHdr;
    if (modelHdr == NULL || modelHdr->meshHdrs == NULL) {
        return;
    }
#endif

#ifdef SH_PC_PORT
    /* Characters are fogged by the SHADER: the prim builder below attaches
     * per-vertex fog bytes (PC_SCREEN_Z_TO_FOG) and the GPU fades these prims
     * toward the fog color exactly like world geometry (whose CPU vertex fog
     * was moved to the shader wholesale — see VTXCOL_LDDP). Feeding the
     * fog-attenuated alpha into the flat character light here as well fogged
     * characters TWICE — a CPU fade toward the black far color on top of the
     * shader fade — so characters rendered darker than PSX relative to the
     * world in every foggy scene (the "characters dark vs environment"
     * report; the shader bytes predate the restoration of this CPU fog path,
     * which is how the double-application crept in). Use the no-fog alpha so
     * the dpcs keeps only the field_20 light boost (tint * map gain, e.g.
     * 121 -> 133 in the cafe intro) and the shader owns ALL character fog. */
    var_v1 = 256 << 4;
#else
    if (g_WorldEnvWork.isFogEnabled)
    {
        if (mat->t[2] < (1 << g_WorldEnvWork.fog.depthShift))
        {
            var_v1 = Q12(1.0f) - (g_WorldEnvWork.fogRamp[(s32)(mat->t[2] << 7) >> g_WorldEnvWork.fog.depthShift] << 4);
        }
        else
        {
            var_v1 = 1 << 4;
        }
    }
    else
    {
        var_v1 = 256 << 4;
    }
#endif

    switch (g_WorldEnvWork.field_0)
    {
        case 0:
            func_8005A42C(scratchData, var_v1);
            break;

        case 1:
            func_8005A478(scratchData, var_v1);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;

        case 2:
            func_8005A838(scratchData, var_v1);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
    }

#ifndef SH_PC_PORT
    modelHdr     = modelInfo->modelHdr;
#endif
    vertOffset   = modelHdr->vertexOffset;
    normalOffset = modelHdr->normalOffset;

    for (curMeshHdr = modelHdr->meshHdrs; curMeshHdr < &modelHdr->meshHdrs[modelHdr->meshCount]; curMeshHdr++)
    {
        func_8005A900(curMeshHdr, vertOffset, scratchData, mat);

        if (g_WorldEnvWork.field_0 != 0)
        {
            func_8005AA08(curMeshHdr, normalOffset, (s_GteScratchData2*)scratchData);
        }

        func_8005AC50(curMeshHdr, (s_GteScratchData2*)scratchData, otTag, (s32)(intptr_t)arg2);
    }
}

void func_8005A42C(s_GteScratchData* scratchData, q19_12 alpha) // 0x8005A42C
{
    q19_12 invAlpha = Q12(1.0f) - Q12_MULT(alpha, g_WorldEnvWork.field_20);

    gte_lddp(invAlpha);
    gte_ldrgb(&g_WorldEnvWork.worldTintColor);
    gte_dpcs();
    gte_strgb(&scratchData->field_3D8);
}

void func_8005A478(s_GteScratchData* scratchData, q19_12 alpha) // 0x8005A478
{
    s32 geomOffsetX;
    s32 geomOffsetY;
    s32 temp_s0;
    s32 temp_s1;
    s32 temp_s2;
    s32 temp_a0;
    s16 geomScreen;
    s32 temp_v1;
    s32 var_a1;
    s32 var_a2;
    s32 var_s0;
    s32 var_t1;
    s32 var_v1;

    s32 temp_s0_neg;
    s32 temp_s1_neg;
    s32 temp_s2_neg;

    ReadGeomOffset(&geomOffsetX, &geomOffsetY);
    geomScreen = ReadGeomScreen();
    SetGeomOffset(-1024, -1024);
    SetGeomScreen(16);

    temp_s0 = g_WorldEnvWork.field_7C.vx;
    temp_s1 = g_WorldEnvWork.field_7C.vy;
    temp_s2 = g_WorldEnvWork.field_7C.vz;

    var_t1 = SquareRoot0(SQUARE(temp_s0) + SQUARE(temp_s1) + SQUARE(temp_s2));
    if (var_t1 == 0)
    {
        var_t1 = 1;
    }

    temp_s0_neg = -temp_s0;
    temp_s1_neg = -temp_s1;
    temp_s2_neg = -temp_s2;

    *(u32*)&scratchData->rotMatrix_3E4[0][0] = *(u32*)&g_WorldEnvWork.field_74;
    scratchData->rotMatrix_3E4[0][2]         = g_WorldEnvWork.field_74.vz;

    scratchData->rotMatrix_3E4[1][0] = Q12(temp_s0_neg) / var_t1;
    scratchData->rotMatrix_3E4[1][1] = Q12(temp_s1_neg) / var_t1;
    scratchData->rotMatrix_3E4[1][2] = Q12(temp_s2_neg) / var_t1;

    scratchData->rotMatrix_3E4[2][0] = temp_s0_neg;
    scratchData->rotMatrix_3E4[2][1] = temp_s1_neg;
    scratchData->rotMatrix_3E4[2][2] = temp_s2_neg;

    gte_SetRotMatrix(scratchData->rotMatrix_3E4);
    gte_SetVector0(&scratchData->rotMatrix_3E4[2][0]);
    gte_ldtr_0();
    gte_rtps();
    gte_stsxy(&scratchData->screenPos_3DC);
    gte_stdp(&scratchData->depthP_3E0);

    scratchData->screenPos_3DC.vx += 1024;
    scratchData->screenPos_3DC.vy += 1024;

    var_s0  = (scratchData->screenPos_3DC.vx * scratchData->screenPos_3DC.vy) + (scratchData->screenPos_3DC.vy * (scratchData->depthP_3E0 >> 4));
    var_s0 >>= 5;
    var_s0  -= 16;
    if (var_s0 < 0)
    {
        var_s0 = 0;
    }

    var_v1 = (scratchData->screenPos_3DC.vx >> 1) + (scratchData->depthP_3E0 >> 2);
    if (var_v1 > 48)
    {
        var_v1 = 48;
    }

    var_s0  += var_v1;
    var_s0 >>= 1;
    if (g_WorldEnvWork.field_3 < var_s0)
    {
        var_s0 = g_WorldEnvWork.field_3;
    }

    var_s0 = Q12_MULT(var_s0, alpha);

    if (var_s0 < 0)
    {
        var_s0 = 0;
    }

    if ((var_s0 >> 2) > 40)
    {
        var_a2 = var_s0 - 40;
    }
    else
    {
        var_a2 = var_s0 - (var_s0 >> 2);
    }

    temp_a0 = (scratchData->rotMatrix_3E4[1][0] * var_a2) >> 7;
    temp_a0 = CLAMP(temp_a0, Q12(-1.4f), Q12(1.4f));

    temp_v1 = (scratchData->rotMatrix_3E4[1][1] * var_a2) >> 7;
    temp_v1 = CLAMP(temp_v1, Q12(-1.4f), Q12(1.4f));

    gte_SetLightSourceXY(temp_a0, temp_v1);

    temp_a0 = (scratchData->rotMatrix_3E4[1][2] * var_a2) >> 7;
    temp_a0 = CLAMP(temp_a0, Q12(-1.4f), Q12(1.4f));

    gte_SetLightSourceZ(temp_a0);

    SetGeomOffset(geomOffsetX, geomOffsetY);
    SetGeomScreen(geomScreen);

    var_a1 = 40;

    if ((var_s0 >> 2) <= 40)
    {
        var_a1 = (var_s0 >> 2);
    }

    if (var_a1 > 64)
    {
        var_a1 = 64;
    }

    SetBackColor(Q12_MULT(g_WorldEnvWork.field_24 + ((g_WorldEnvWork.worldTintColor.r * var_a1) >> 7), alpha),
                 Q12_MULT(g_WorldEnvWork.field_25 + ((g_WorldEnvWork.worldTintColor.g * var_a1) >> 7), alpha),
                 Q12_MULT(g_WorldEnvWork.field_26 + ((g_WorldEnvWork.worldTintColor.b * var_a1) >> 7), alpha));
}

void func_8005A838(s_GteScratchData* scratchData, s32 scale) // 0x8005A838
{
    SVECTOR3 vec;

    vec.vx = Q12_MULT(g_WorldEnvWork.field_74.vx, scale) >> 1;
    vec.vy = Q12_MULT(g_WorldEnvWork.field_74.vy, scale) >> 1;
    vec.vz = Q12_MULT(g_WorldEnvWork.field_74.vz, scale) >> 1;

    gte_SetLightSVector(&vec);

    SetBackColor(Q12_MULT(g_WorldEnvWork.field_24, scale),
                 Q12_MULT(g_WorldEnvWork.field_25, scale),
                 Q12_MULT(g_WorldEnvWork.field_26, scale));
}

void func_8005A900(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat) // 0x8005A900
{
    DVECTOR* inXy;  // Model-space XY input
    u16*     inZ;   // Model-space Z input
    DVECTOR* outXy; // Projected XY output buffer
    u16*     outZ;  // Projected Z output buffer

    if (meshHdr->vertexCount == 0)
    {
        return;
    }

    SetRotMatrix(mat);
    SetTransMatrix(mat);

    outXy = &scratchData->screenXy_0[offset];
    outZ  = &scratchData->screenZ_168[offset];

    inXy = meshHdr->verticesXy;
    inZ  = meshHdr->verticesZ;

    while (outXy < &scratchData->screenXy_0[meshHdr->vertexCount + offset])
    {
        // Nearly same as `gte_RotTransPers3`, processes 3 vertices per iteration.
        gte_LoadVector0_1_2_XYZ(inXy, inZ);
        gte_rtpt();
        gte_FetchScreen0_1_2_XYZ(outXy, outZ);

        outXy += 3;
        outZ  += 3;
        inXy  += 3;
        inZ   += 3;
    }
}

u8 func_8005AA08(s_MeshHeader* meshHdr, s32 arg1, s_GteScratchData2* scratchData) // 0x8005AA08
{
    // Same as `gte_strgb3`, but takes `VECTOR3` pointer to store results.
    // Not sure why this was needed, the func that uses it also ends up calling the normal `gte_strgb3` too.
#ifdef SH_PC_PORT
    #define gte_strgb3_vec( r0 ) do { \
        *(u32*)((char*)(r0) + 0) = MFC2(20); \
        *(u32*)((char*)(r0) + 4) = MFC2(21); \
        *(u32*)((char*)(r0) + 8) = MFC2(22); \
    } while(0)
#else
    #define gte_strgb3_vec( r0 ) __asm__ volatile ( \
        "swc2    $20, 0( %0 );"                      \
        "swc2    $21, 4( %0 );"                      \
        "swc2    $22, 8( %0 )"                       \
        :                                           \
        : "r"( r0 )                                 \
        : "memory" )
#endif

    CVECTOR   sp0;
    s_Normal* var_a3;
    VECTOR3*  var_t0;

    if (meshHdr->normalCount == 0)
    {
        return;
    }

    sp0.cd = 0;
    gte_ldrgb(&sp0);

    var_a3 = meshHdr->normals;
    *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[0];
    scratchData->u.normal.field_3E0[0].vx = scratchData->u.normal.field_3DC.nx << 5;
    scratchData->u.normal.field_3E0[0].vy = scratchData->u.normal.field_3DC.ny << 5;
    scratchData->u.normal.field_3E0[0].vz = scratchData->u.normal.field_3DC.nz << 5;

    *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[1];
    scratchData->u.normal.field_3E0[1].vx = scratchData->u.normal.field_3DC.nx << 5;
    scratchData->u.normal.field_3E0[1].vy = scratchData->u.normal.field_3DC.ny << 5;
    scratchData->u.normal.field_3E0[1].vz = scratchData->u.normal.field_3DC.nz << 5;

    *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[2];
    var_a3 += 3;
    scratchData->u.normal.field_3E0[2].vx = scratchData->u.normal.field_3DC.nx << 5;
    scratchData->u.normal.field_3E0[2].vy = scratchData->u.normal.field_3DC.ny << 5;
    scratchData->u.normal.field_3E0[2].vz = scratchData->u.normal.field_3DC.nz << 5;

    var_t0 = &scratchData->field_21C[arg1];

    gte_ldv3c(scratchData->u.normal.field_3E0);
    gte_nct();

    while(var_a3 < &meshHdr->normals[meshHdr->normalCount])
    {
        *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[0];
        scratchData->u.normal.field_3E0[0].vx = scratchData->u.normal.field_3DC.nx << 5;
        scratchData->u.normal.field_3E0[0].vy = scratchData->u.normal.field_3DC.ny << 5;
        scratchData->u.normal.field_3E0[0].vz = scratchData->u.normal.field_3DC.nz << 5;

        *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[1];
        scratchData->u.normal.field_3E0[1].vx = scratchData->u.normal.field_3DC.nx << 5;
        scratchData->u.normal.field_3E0[1].vy = scratchData->u.normal.field_3DC.ny << 5;
        scratchData->u.normal.field_3E0[1].vz = scratchData->u.normal.field_3DC.nz << 5;

        *(u32*)&scratchData->u.normal.field_3DC = *(u32*)&var_a3[2];
        var_a3 += 3;
        scratchData->u.normal.field_3E0[2].vx = scratchData->u.normal.field_3DC.nx << 5;
        scratchData->u.normal.field_3E0[2].vy = scratchData->u.normal.field_3DC.ny << 5;
        scratchData->u.normal.field_3E0[2].vz = scratchData->u.normal.field_3DC.nz << 5;

        gte_strgb3_vec(var_t0); // Store result of previous `gte_nct` call.
        var_t0++;
        gte_ldv3c(scratchData->u.normal.field_3E0);
        gte_nct();
    }

    gte_strgb3(&var_t0->vx, &var_t0->vy, &var_t0->vz); // Store result from final `gte_nct`.
}

void func_8005AC50(s_MeshHeader* meshHdr, s_GteScratchData2* scratchData, GsOT_TAG* ot, bool arg3) // 0x8005AC50
{
    typedef union
    {
        POLY_GT3* gt3;
        POLY_GT4* gt4;
        PACKET*   packet;
    } u_poly;

#ifdef SH_PC_PORT
    /* PSX GPU clips huge polys to the drawing area; PsyCross does not.
     * Skip any polygon where a vertex is extremely far off-screen. */
    #define SCREEN_BOUND 1024
    #define VERTEX_OOB(packed) \
        ((s16)((packed) & 0xFFFF) < -SCREEN_BOUND || (s16)((packed) & 0xFFFF) > SCREEN_BOUND || \
         (s16)(((packed) >> 16) & 0xFFFF) < -SCREEN_BOUND || (s16)(((packed) >> 16) & 0xFFFF) > SCREEN_BOUND)
#endif

    s32          r4;
    s32          sp4;
    s16          temp_v1;
    s32          temp_a0;
    s32          temp_t4;
    s32          temp_t4_2;
    s32          temp_v0;
    s32          temp_v0_2;
    s32          var_t9;
    s32          var_v0;
    s32          var_v1;
    s32          var_a3;
    GsOT_TAG*    localOt;
    s_Primitive* prim;
    u16*         var_t5;
    u_poly       poly;

    var_a3              = g_WorldEnvWork.field_0;
    scratchData->u.s_1.field_8 = g_WorldEnvWork.field_14C << 16;

    temp_a0 = 0x79C << (arg3 + 2);
    var_t9  = g_WorldEnvWork.isFogEnabled ? MIN(temp_a0, FOG_FAR_DIST()) : temp_a0;
    SH_WHOLEMAP_FARCAP(var_t9);

#ifdef SH_PC_PORT
    s32 _dbgPrimPass = 0, _dbgPrimDepthFail = 0, _dbgPrimOobFail = 0, _dbgPrimTotal = 0;
    {
        static int _charTexLog = 0;
        if (_charTexLog < 3 && meshHdr->primitiveCount > 0) {
            s_Primitive* _p0 = &meshHdr->primitives[0];
            u8 tpg = _p0->field_6.bits.field_6_0;
            int pageX = (tpg & 0xF) * 64;
            int pageY = ((tpg >> 4) & 1) * 256;
            int fmt = (tpg >> 7) & 0x3;
            u16 clut = _p0->field_2;
            int clutX = (clut & 0x3F) << 4;
            int clutY = (clut >> 6) & 0x1FF;
            /* Check a few pixels of VRAM at the texture page location */
            {
                RECT16 _chk = { pageX, pageY, 4, 1 };
                u16 _vramSample[4];
                StoreImage(&_chk, (u_long*)_vramSample);
            }
            /* Also check CLUT */
            {
                RECT16 _chk2 = { clutX, clutY, 4, 1 };
                u16 _clutSample[4];
                StoreImage(&_chk2, (u_long*)_clutSample);
            }
            _charTexLog++;
        }
    }
#endif

    for (prim = meshHdr->primitives, poly.packet = GsOUT_PACKET_P; prim < &meshHdr->primitives[meshHdr->primitiveCount]; prim++)
    {
        *(s32*)&scratchData->u.s_1.field_0 = *(s32*)&prim->field_C;
        *(s32*)&scratchData->u.s_1.field_4 = *(s32*)&prim->field_10;

#ifdef SH_PC_PORT
        _dbgPrimTotal++;
#endif

        if (scratchData->u.s_1.field_3 == 0xFF)
        {
            temp_t4 = (scratchData->screenZ_168[scratchData->u.s_1.field_0] + scratchData->screenZ_168[scratchData->u.s_1.field_1] +
                       scratchData->screenZ_168[scratchData->u.s_1.field_2] + scratchData->screenZ_168[scratchData->u.s_1.field_2]) >> 2;

            SH_WHOLEMAP_DEPTH_RESCUE(temp_t4, arg3);
            if (temp_t4 <= 0 || var_t9 < temp_t4)
            {
#ifdef SH_PC_PORT
                _dbgPrimDepthFail++;
#endif
                continue;
            }

            gte_NormalClip(*(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_0],
                           *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_1],
                           *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_2],
                           &r4);

            /* PC: re-enabled — this backface cull was bypassed during the port,
             * which let lit/transparent cutscene characters draw their back-face
             * polys (face visible through the back of the head). The identical
             * cull in the non-lit Gfx_MeshDraw path works correctly on PC, so
             * the GTE NormalClip sign is right here too. */
#ifdef SH_PC_PORT
            /* Backface decision on the PGXP-precise projection, not the rounded
             * 16-bit coords, so distant near-edge-on faces don't flip sign and
             * vanish (the "model sheds chunks far away" holes). Integer fallback. */
            {
                extern int PsyX_PGXP_TriBackface(const void*, const void*, const void*, int);
                if (PsyX_PGXP_TriBackface(&scratchData->screenXy_0[scratchData->u.s_1.field_0],
                                          &scratchData->screenXy_0[scratchData->u.s_1.field_1],
                                          &scratchData->screenXy_0[scratchData->u.s_1.field_2], r4))
                    continue;
            }
#else
            if (r4 <= 0)
            {
                continue;
            }
#endif

            *(s32*)&poly.gt3->x0 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_0];
            *(s32*)&poly.gt3->x1 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_1];
            *(s32*)&poly.gt3->x2 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_2];

#ifdef SH_PC_PORT
            if (VERTEX_OOB(*(s32*)&poly.gt3->x0) || VERTEX_OOB(*(s32*)&poly.gt3->x1) || VERTEX_OOB(*(s32*)&poly.gt3->x2)) {
                _dbgPrimOobFail++;
                continue;
            }
            SH_PGXP_PROP3(scratchData, poly.gt3,
                scratchData->u.s_1.field_0, scratchData->u.s_1.field_1, scratchData->u.s_1.field_2);
#endif

            if (var_a3 != 0)
            {
                *(s32*)&poly.gt3->r0 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_4];
                *(s32*)&poly.gt3->r1 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_5];
                *(s32*)&poly.gt3->r2 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_6];
            }
            else
            {
                *(s32*)&poly.gt3->r0 = *(s32*)&poly.gt3->r1 = *(s32*)&poly.gt3->r2 = *(s32*)&scratchData->field_3D8;
            }

            poly.gt3->code = ((prim->field_6.flags >> 15) * 2) | 0x34;

            *(s32*)&poly.gt3->u0 = *(s32*)&prim->field_0 + scratchData->u.s_1.field_8;
            *(s32*)&poly.gt3->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
            *(u16*)&poly.gt3->u2 = prim->field_8;

#ifdef SH_PC_PORT
            /* Encode per-vertex fog so the shader can blend toward fog color
             * with distance. Without this, distant characters render as the
             * solid color from gte_nct (which collapses to the background
             * color = black when fog dampens light contribution to ~0). */
            poly.gt3->p1 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_1]);
            poly.gt3->p2 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_2]);
#endif

            setlen(poly.gt3, 9);

            SH_CLAMP_OT_DEPTH(temp_t4, arg3);
            addPrim(&ot[(temp_t4 >> arg3) >> 2], poly.gt3);
            poly.gt3++;
#ifdef SH_PC_PORT
            _dbgPrimPass++;
#endif
        }
        else
        {
            temp_t4 = (scratchData->screenZ_168[scratchData->u.s_1.field_0] + scratchData->screenZ_168[scratchData->u.s_1.field_1] +
                       scratchData->screenZ_168[scratchData->u.s_1.field_2] + scratchData->screenZ_168[scratchData->u.s_1.field_3]) >> 2;

            SH_WHOLEMAP_DEPTH_RESCUE(temp_t4, arg3);
            if (temp_t4 <= 0 || var_t9 < temp_t4)
            {
#ifdef SH_PC_PORT
                _dbgPrimDepthFail++;
#endif
                continue;
            }

            gte_ldsxy3(*(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_0],
                       *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_1],
                       *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_2]);
            gte_nclip();
            gte_stopz(&sp4);

            /* PC: re-enabled backface cull (see GT3 path above). */
#ifdef SH_PC_PORT
            /* Precise backface cull on both triangles of the quad (see GT3). */
            {
                s32 _sp4b = 0;
                extern int PsyX_PGXP_QuadBackface(const void*, const void*, const void*, const void*, int, int);
                gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_3]);
                gte_nclip();
                gte_stopz(&_sp4b);
                if (PsyX_PGXP_QuadBackface(&scratchData->screenXy_0[scratchData->u.s_1.field_0],
                                           &scratchData->screenXy_0[scratchData->u.s_1.field_1],
                                           &scratchData->screenXy_0[scratchData->u.s_1.field_2],
                                           &scratchData->screenXy_0[scratchData->u.s_1.field_3], sp4, _sp4b))
                    continue;
            }
#else
            if (sp4 <= 0)
            {
                gte_ldsxy0(*(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_3]);
                gte_nclip();
                gte_stopz(&sp4);

                if (sp4 >= 0)
                {
                    continue;
                }
            }
#endif

            *(s32*)&poly.gt4->x0 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_0];
            *(s32*)&poly.gt4->x1 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_1];
            *(s32*)&poly.gt4->x2 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_2];
            *(s32*)&poly.gt4->x3 = *(s32*)&scratchData->screenXy_0[scratchData->u.s_1.field_3];

#ifdef SH_PC_PORT
            if (VERTEX_OOB(*(s32*)&poly.gt4->x0) || VERTEX_OOB(*(s32*)&poly.gt4->x1) ||
                VERTEX_OOB(*(s32*)&poly.gt4->x2) || VERTEX_OOB(*(s32*)&poly.gt4->x3)) {
                _dbgPrimOobFail++;
                continue;
            }
            SH_PGXP_PROP4(scratchData, poly.gt4,
                scratchData->u.s_1.field_0, scratchData->u.s_1.field_1,
                scratchData->u.s_1.field_2, scratchData->u.s_1.field_3);
#endif

            if (var_a3 != 0)
            {
                *(s32*)&poly.gt4->r0 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_4];
                *(s32*)&poly.gt4->r1 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_5];
                *(s32*)&poly.gt4->r2 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_6];
                *(s32*)&poly.gt4->r3 = *(s32*)&scratchData->field_21C[scratchData->u.s_1.field_7];
            }
            else
            {
                *(s32*)&poly.gt4->r0 = *(s32*)&poly.gt4->r1 = *(s32*)&poly.gt4->r2 = *(s32*)&poly.gt4->r3 = *(s32*)&scratchData->field_3D8;
            }

            poly.gt4->code = ((prim->field_6.flags >> 15) * 2) | 0x3C;

            *(s32*)&poly.gt4->u0 = *(s32*)&prim->field_0 + scratchData->u.s_1.field_8;
            *(s32*)&poly.gt4->u1 = *(s32*)&prim->field_4 & 0xFFFFFF;
            *(u16*)&poly.gt4->u2 = prim->field_8;
            *(u16*)&poly.gt4->u3 = prim->field_A;

#ifdef SH_PC_PORT
            /* Encode per-vertex fog. PsyCross GT4 reads v0 fog from pad2,
             * v1<-p1, v2<-p2, v3<-p3. v0's color-word pad is the code byte,
             * so its fog rides in the unused pad2 short. */
            poly.gt4->pad2 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_0]);
            poly.gt4->p1 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_1]);
            poly.gt4->p2 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_2]);
            poly.gt4->p3 = PC_SCREEN_Z_TO_FOG(scratchData->screenZ_168[scratchData->u.s_1.field_3]);
#endif

            setlen(poly.gt4, 12);

            SH_CLAMP_OT_DEPTH(temp_t4, arg3);
            addPrim(&ot[(temp_t4 >> arg3) >> 2], poly.gt4);
            poly.gt4++;
#ifdef SH_PC_PORT
            _dbgPrimPass++;
#endif
        }
    }

#ifdef SH_PC_PORT
    {
        static int _primSummaryLog = 0;
        if (_primSummaryLog < 25) {
            /* Log first prim vertex indices for ALL models */
            if (meshHdr->primitiveCount > 0) {
                s_Primitive* p0 = &meshHdr->primitives[0];
                u8* fc = (u8*)&p0->field_C;
            }
            _primSummaryLog++;
        }
    }
#endif

    GsOUT_PACKET_P = poly.packet;
}

// ========================================
// TEXTURE HANDLING
// ========================================

void Texture_Init(s_Texture* tex, char* texName, u8 tPage0, u8 tPage1, s32 u, s32 v, s16 clutX, s16 clutY) // 0x8005B1A0
{
    tex->imageDesc.tPage[0] = tPage0;
    tex->imageDesc.tPage[1] = tPage1;
    tex->imageDesc.u        = u;
    tex->imageDesc.v        = v;
    tex->imageDesc.clutX    = clutX;
    tex->imageDesc.clutY    = clutY;

    StringCopy(tex->name.str, texName);

    tex->refCount = 0;
    tex->queueIdx = NO_VALUE;
}

#ifdef SH_PC_PORT
/* TIMs whose VRAM CLUT map code manipulates directly — StoreImage of the
 * palette row, derived palette rows (getClut(x, y+1/+13/+14/+15)), blend-
 * variant tpages — must keep a REAL pool page: a virtual GL slot has no VRAM
 * CLUT to read and its synthetic clutY makes every derived row a bogus slot
 * key. Only known case: map4_s03's Twinfeeler palette animation
 * (func_800D078C via Texture_InfoGet). */
static bool Pc_MaterialNeedsVramSlot(const s_Material* mat)
{
    return !COMPARE_FILENAMES(&mat->name, "SPUM602F");
}
#endif

s_Texture* Texture_Get(s_Material* mat, s_ActiveChunkTextures* activeTexs, void* fsBuffer9, e_FsFile fileIdx, s32 arg4)
{
    s8         filename[12];
    s8         debugStr[12];
    s32        fileId;
    s32        i;
    s32        smallestQueueIdx;
    u32        queueIdx;
    s_Texture* curTex;
    s_Texture* foundTex;
#ifdef SH_PC_PORT
    bool       needsVram = Pc_MaterialNeedsVramSlot(mat);
#endif

    smallestQueueIdx = INT_MAX;
    mat->texture = NULL;
    foundTex = NULL;

#ifdef SH_PC_PORT
    /* Claim a VIRTUAL slot when one is free: GL-backed slots are immune to
     * the 2D-event/map-screen/boss-FX uploads that write straight into the
     * pool's VRAM pages. Vanilla healed those stomps by constantly
     * releasing + reloading chunk textures; resident texturing PINS
     * materials for the whole map, so a stomped physical page stayed
     * garbled forever (school floors/walls after any 2D screen). Physical
     * pages remain for VRAM-dependent TIMs (needsVram), the global LM
     * (its count=4 clamp only ever exposes the first 4 physical entries),
     * and overflow. With resident_textures=0 no virtual slots are listed
     * and this reduces to the vanilla single-candidate scan. */
    {
        s_Texture* bestVirt  = NULL;
        s_Texture* bestPhys  = NULL;
        s32        bestVirtQ = INT_MAX;
        s32        bestPhysQ = INT_MAX;

        for (i = 0; i < activeTexs->count; i++)
        {
            curTex = activeTexs->textures[i];

            if (!COMPARE_FILENAMES(&mat->name, &curTex->name))
            {
                mat->texture = curTex;
                curTex->refCount++;
                return curTex;
            }

            if (curTex->refCount != 0)
            {
                continue;
            }

            if (curTex->imageDesc.clutY >= HIRES_POOL_CLUT_ROW_BASE)
            {
                if ((s32)curTex->queueIdx < bestVirtQ)
                {
                    bestVirtQ = (s32)curTex->queueIdx;
                    bestVirt  = curTex;
                }
            }
            else if ((s32)curTex->queueIdx < bestPhysQ)
            {
                bestPhysQ = (s32)curTex->queueIdx;
                bestPhys  = curTex;
            }
        }

        foundTex = needsVram ? bestPhys : (bestVirt != NULL ? bestVirt : bestPhys);
    }
    (void)smallestQueueIdx;
    (void)queueIdx;
#else
    for (i = 0; i < activeTexs->count; i++)
    {
        curTex = activeTexs->textures[i];

        if (!COMPARE_FILENAMES(&mat->name, &curTex->name))
        {
            mat->texture = curTex;
            curTex->refCount++;
            return curTex;
        }

        queueIdx = curTex->queueIdx;
        if ((s32)queueIdx < smallestQueueIdx && curTex->refCount == 0)
        {
            smallestQueueIdx = queueIdx;
            foundTex       = curTex;
        }
    }
#endif

    if (foundTex == NULL)
    {
#ifdef SH_PC_PORT
        /* Every slot pinned (refCount > 0). Under resident_textures this
         * means the expanded pool is undersized for this map's distinct TIM
         * count — the material stays untextured and its chunk is never drawn.
         * Loud so sizing problems name themselves. */
        if (g_PcConfig.residentTextures)
        {
            static int s_poolExhaustLog = 0;
            if (s_poolExhaustLog < 8)
            {
                SH_DBG("[POOLTEX] pool exhausted: no free slot for '%.8s'", mat->name.str);
                s_poolExhaustLog++;
            }
        }
#endif
        return NULL;
    }

    Material_TimFileNameGet(&filename, mat);
    fileId = Fs_FindNextFile(&filename, 0, fileIdx);
    if (fileId == NO_VALUE)
    {
        // Failed to find file, log filename to screen.
        debugStr[12] = 0;
        Text_Debug_PositionSet(100, 80);
        strncpy(&debugStr, &filename, 12);

#if VERSION_EQUAL_OR_OLDER(PROTO_981216)
        // Code seen in 98-12-16 build.
        Text_Debug_Draw(debugStr);
#endif

#ifdef SH_PC_PORT
        /* Retail PSX enqueued fileId = -1 here anyway: info became
         * &g_FileTable[-1] and the CD read garbage — tolerated on PSX, but
         * on 64-bit the (u32)-1 index makes a non-canonical pointer and the
         * queue tick faults (the sewer/save-load crash family). Keep the
         * PSX control flow (texture slot claimed, no retry loop) but skip
         * the bogus read; the slot keeps its previous VRAM content exactly
         * like PSX's garbage read effectively did. */
        SH_DBG("[FSQ] chunk TIM '%.11s' not in file table — skipping read (Texture_Get)", filename);

        /* "Previous VRAM content" only exists on a PHYSICAL slot; a virtual
         * slot with no read never registers a GL texture and its bit-15 clut
         * has nothing to fall back on. Swap to a free physical slot when one
         * exists so the missing-TIM material keeps the PSX degraded look. */
        if (foundTex->imageDesc.clutY >= HIRES_POOL_CLUT_ROW_BASE)
        {
            s32        bestQ = INT_MAX;
            s_Texture* phys  = NULL;

            for (i = 0; i < activeTexs->count; i++)
            {
                curTex = activeTexs->textures[i];
                if (curTex->imageDesc.clutY >= HIRES_POOL_CLUT_ROW_BASE)
                {
                    continue;
                }
                if (curTex->refCount == 0 && (s32)curTex->queueIdx < bestQ)
                {
                    bestQ = (s32)curTex->queueIdx;
                    phys  = curTex;
                }
            }
            if (phys != NULL)
            {
                foundTex = phys;
            }
        }

        foundTex->queueIdx = 0;
        foundTex->refCount++;
        foundTex->name     = mat->name;
        return foundTex;
#endif
    }

    foundTex->queueIdx = Fs_QueueStartReadTim(fileId, fsBuffer9, &foundTex->imageDesc);
    foundTex->refCount++;
    foundTex->name = mat->name;

    return foundTex;
}

void Texture_RefCountReset(s_Texture* tex) // 0x8005B370
{
    tex->refCount = 0;
}

void func_8005B378(s_Texture* tex, char* arg1) // 0x8005B378
{
    tex->refCount = 1;
    tex->queueIdx = 0;
    StringCopy(tex->name.str, arg1);
}

void Texture_RefClear(s_Texture* tex) // 0x8005B3A4
{
    tex->name.u32[1] = 0;
    tex->name.u32[0] = 0;

    tex->refCount = 0;
    tex->queueIdx = NO_VALUE;
}

void Material_TimFileNameGet(char* filename, s_Material* mat) // 0x8005B3BC
{
    char sp10[12];

    // Some inline `memcpy`/`bcopy`/`strncpy`? those use `lwl`/`lwr`/`swl`/`swr` instead though
    // Example: casting `filename`/`arg1` to `u32*` and using `memcpy` does generate `lw`/`sw`,
    // but not in same order as this, guess it's some custom inline/macro instead.
    *(u32*)&sp10[0] = *(u32*)&mat->name.str[0];
    *(u32*)&sp10[4] = *(u32*)&mat->name.str[4];
    *(u32*)&sp10[8] = 0;

    strcat(sp10, D_80028544); // Copies `TIM` to end of `sp10` string.

    *(u32*)&filename[0] = *(u32*)&sp10[0];
    *(u32*)&filename[4] = *(u32*)&sp10[4];
    *(u32*)&filename[8] = *(u32*)&sp10[8];
}

INCLUDE_RODATA("bodyprog/nonmatchings/gfx/bodyprog_80055028", D_80028544);

void func_8005B424(VECTOR3* vec0, const VECTOR3* vec1) // 0x8005B424
{
    vec0->vz = 0;
    vec0->vy = 0;
    vec0->vx = 0;

    if (vec1 == NULL)
    {
        return;
    }

    *((s_func_8005B424*)vec0) = *((s_func_8005B424*)vec1);
}

void Textures_ActiveTex_CountReset(s_ActiveChunkTextures* activeTexs) // 0x8005B46C
{
    activeTexs->count = 0;
}

void Textures_ActiveTex_PutTextures(s_ActiveChunkTextures* activeTexs, s_Texture* texs, s32 texIdx) // 0x8005B474
{
    s_Texture*  curTex;
    s_Texture** texEntries;

    texEntries = activeTexs->textures;
    for (curTex = &texs[0]; curTex < &texs[texIdx];)
    {
        *texEntries++ = curTex++;
        activeTexs->count++;
    }
}

s_Texture* Textures_ActiveTex_FindTexture(char* texName, s_ActiveChunkTextures* activeTexs) // 0x8005B4BC
{
    char       prevTexName[8];
    s32        i;
    s_Texture* curTex;

    StringCopy(prevTexName, texName);

    for (i = 0; i < activeTexs->count; i++)
    {
        curTex = activeTexs->textures[i];
        if (curTex->queueIdx != NO_VALUE && !COMPARE_FILENAMES(prevTexName, &curTex->name))
        {
            return curTex;
        }
    }

    return NULL;
}

void func_8005B55C(GsCOORDINATE2* coord) // 0x8005B55C
{
    s_800AE204* curPtr;

    D_800C42B8 = coord;

    for (curPtr = &D_800AE204[0]; curPtr < &D_800AE204[26]; curPtr++)
    {
        curPtr->field_C.vz = -Math_Cos(curPtr->field_4);
        curPtr->field_C.vx  = -Math_Sin(curPtr->field_4);
        curPtr->field_C.vy  = 0;
        curPtr->position.vz = Q12_MULT(curPtr->field_2, Math_Cos(curPtr->field_4));
        curPtr->position.vx = Q12_MULT(curPtr->field_2, Math_Sin(curPtr->field_4));
        curPtr->position.vy = curPtr->positionY;
    }
}

void Gfx_BillboardDraw(s32 arg0, q19_12 posX, q19_12 posY, q19_12 posZ, GsOT* ot_arg4, s32 arg5) // 0x8005B62C
{
    MATRIX            matrix_sp18[2];
    CVECTOR           sp58[5];
    MATRIX            sp70;
    s_GteScratchData2 sp90;
    DVECTOR           field_1C;
    s32               field_24;
    s32               sp494;
    s32               sp498;
    u16               sp4A0;
    u16               sp4A8;
    s_WorldEnvWork*   worldEnvWork;
    s_WorldEnvWork*   worldEngWork1;
    s32               temp_a0;
    s32               temp_a0_2;
    s32               temp_lo;
    s32               temp_lo_2;
    s32               temp_v0;
    s32               temp_v0_2;
    s32               temp_v1;
    s32               temp_v1_2;
    s32               temp_v1_4;
    q19_12            posX0;
    q19_12            posY0;
    q19_12            posZ0;
    s32               var_s1;
    s32               var_v0;
    s_800AE204*       curPtr;
    s_800AE4DC*       temp_fp;
    s32               temp;
    s32               i;
    PACKET*           packet;
    PACKET*           packet2;
    POLY_GT4*         poly_gt4;
    POLY_G4*          poly_g4;

    temp_fp = &D_800AE4DC[arg0];
    sp4A0   = temp_fp->field_A | (temp_fp->field_9 << 8);
    sp4A8   = temp_fp->field_8 | (temp_fp->field_B << 8);
    sp498   = ReadGeomScreen();

    temp_v1 = 0x79C << (arg5 + 2);
    sp494   = g_WorldEnvWork.isFogEnabled ? MIN(temp_v1, FOG_FAR_DIST()) : temp_v1;
    SH_WHOLEMAP_FARCAP(sp494);
    Vw_WorldScreenMatrixAtPositionGet(&matrix_sp18[0], posX, posY, posZ);

    // @hack Pointer needed for match, is there a way to remove this?
    // `Gfx_FogOverlayQuadDraw` `Gfx_MeshDraw` `func_8005AC50` all seem to do similar thing without needing pointer?
    worldEnvWork = &g_WorldEnvWork;
    if (!worldEnvWork->isFogEnabled)
    {
        var_s1 = Q12(1.0f);
    }
    else
    {
#ifdef SH_PC_PORT
        /* PC: skip group-level fog dimming here — per-billboard fog is
         * applied separately after vertex colors are set. This avoids
         * double-darkening (var_s1 + dp both reducing toward 0). */
        var_s1 = Q12(1.0f);
#else
        var_s1 = Q12(1.0f) - Q8_TO_Q12(MIN(func_80055A50(Q8_TO_Q12(matrix_sp18[0].t[2])), Q12(1.0f)));
#endif
    }

    // @hack Make sure compiler doesn't optimize out `new_var` pointer.
    if (worldEnvWork != NULL)
    {
        u8 x = -x;
    }

    if (g_WorldEnvWork.field_0 == 0)
    {
        sp58[4].r = Q12_MULT(g_WorldEnvWork.worldTintColor.r, g_WorldEnvWork.field_20);
        sp58[4].g = Q12_MULT(g_WorldEnvWork.worldTintColor.g, g_WorldEnvWork.field_20);
        sp58[4].b = Q12_MULT(g_WorldEnvWork.worldTintColor.b, g_WorldEnvWork.field_20);

        if (g_WorldEnvWork.isFogEnabled)
        {
            sp58[4].r = Q12_MULT(sp58[4].r, var_s1);
            sp58[4].g = Q12_MULT(sp58[4].g, var_s1);
            sp58[4].b = Q12_MULT(sp58[4].b, var_s1);
        }

        *(s32*)&sp58[3] = *(s32*)&sp58[4];
        *(s32*)&sp58[2] = *(s32*)&sp58[4];
        *(s32*)&sp58[1] = *(s32*)&sp58[4];
        *(s32*)&sp58[0] = *(s32*)&sp58[4];
    }
    else
    {
        Vw_CoordHierarchyMatrixCompute(D_800C42B8, &matrix_sp18[1]);

        matrix_sp18[1].t[0] = Q12_TO_Q8(posX);
        matrix_sp18[1].t[1] = Q12_TO_Q8(posY) + temp_fp->field_6;
        matrix_sp18[1].t[2] = Q12_TO_Q8(posZ);

        func_80057228(&matrix_sp18[1], g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);

        switch (g_WorldEnvWork.field_0)
        {
            case 0:
            case 1:
                func_8005A478(&sp90, var_s1);
                SetColorMatrix(&g_WorldEnvWork.field_2C);
                gte_lddqa(g_WorldEnvWork.field_4C);
                gte_lddqb_0();
                break;

            case 2:
                func_8005A838(&sp90, var_s1);
                SetColorMatrix(&g_WorldEnvWork.field_2C);
                break;
        }

        ReadLightMatrix(&sp70);

        if (g_WorldEnvWork.isFogEnabled)
        {
            posX0 = Q12_MULT(sp70.m[0][0], var_s1);
            posY0 = Q12_MULT(sp70.m[0][1], var_s1);
            posZ0 = Q12_MULT(sp70.m[0][2], var_s1);
        }
        else
        {
            posX0 = sp70.m[0][0];
            posY0 = sp70.m[0][1];
            posZ0 = sp70.m[0][2];
        }

        temp_v0 = SquareRoot0(SQUARE(posX0) + SQUARE(posY0) + SQUARE(posZ0));

        if (temp_v0 > Q12(0.25f))
        {
            temp_v1_2 = Q12_MULT(temp_v0 - Q12(0.25f), Q12(0.3f)) + Q12(0.25f);

            var_v0 = Q12(MIN(temp_v1_2, Q12(1.0f)));

            temp_lo = var_v0 / temp_v0;

            posX0   = Q12_MULT(posX0, temp_lo);
            posY0 = Q12_MULT(posY0, temp_lo);
            posZ0   = Q12_MULT(posZ0, temp_lo);

            if (g_WorldEnvWork.field_0 == 1)
            {
                gte_lddp(temp_lo);
                gte_ldir_stbk();
                gte_gpf12();
                gte_ldmac_stir();
            }
        }

        sp70.m[0][0] = posX0;
        sp70.m[0][1] = posY0;
        sp70.m[0][2] = posZ0;

        sp70.m[1][0] = -posZ0 >> 4;
        sp70.m[1][1] = posY0 >> 4;
        sp70.m[1][2] = posX0 >> 4;

        sp70.m[2][0] = posZ0 >> 4;
        sp70.m[2][1] = posY0 >> 4;
        sp70.m[2][2] = -posX0 >> 4;

        SetLightMatrix(&sp70);
        NormalColor3(&D_800AE500[0], &D_800AE500[1], &D_800AE500[2], &sp58[0], &sp58[1], &sp58[2]);
        NormalColor(&D_800AE500[3], &sp58[3]);
    }

#ifndef SH_PC_PORT
    // @hack Needed to get right reg order for `poly_gt4`.
    worldEngWork1 = &g_WorldEnvWork;
#endif


    SetRotMatrix(&matrix_sp18[0]);
    SetTransMatrix(&matrix_sp18[0]);

    for (curPtr = temp_fp->ptr_0, poly_gt4 = GsOUT_PACKET_P;
         curPtr < &temp_fp->ptr_0[temp_fp->count_4]; curPtr++)
    {
        temp_v0_2 = RotTransPers((SVECTOR*)&curPtr->position.vx, &field_1C, &field_24, &field_24);
        temp_a0   = temp_v0_2 << 2;

#ifdef SH_PC_PORT
        /* Whole-town far billboards: beyond ~256u the GTE SZ3 register pegs at
         * 0xFFFF, so the size divisor below freezes at the 256u scale while the
         * re-projected center is correct — trees/grass rendered huge all over
         * the street. Substitute the TRUE unclamped depth (exported by the GTE
         * far path) so distant sprites shrink correctly instead. */
        if (temp_a0 >= 0xFFFC && Pc_WholeMapDrawActive())
        {
            extern int g_PsxWholeMapLastSz;
            if (g_PsxWholeMapLastSz > temp_a0)
                temp_a0 = g_PsxWholeMapLastSz;
        }
#endif

        if (temp_v0_2 > 32 && temp_a0 < sp494)
        {
            s32 tpage = 0x2C;
            s32 clut  = 0x8C;
            temp_lo_2 = Q12(sp498) / temp_a0;

            *(s32*)&poly_gt4->r0 = *(s32*)&sp58[0];
            *(s32*)&poly_gt4->r1 = *(s32*)&sp58[1];
            *(s32*)&poly_gt4->r2 = *(s32*)&sp58[2];
            temp_a0              = *(s32*)&sp58[3];

            do {} while (0); // @hack

            setPolyGT4(poly_gt4);

            poly_gt4->tpage = tpage;

            poly_gt4->clut       = clut;
            *(s32*)&poly_gt4->r3 = temp_a0;
            *(s16*)&poly_gt4->u0 = *(s16*)&temp_fp->field_8;
            *(s16*)&poly_gt4->u1 = sp4A0;
            *(s16*)&poly_gt4->u2 = sp4A8;
            *(s16*)&poly_gt4->u3 = *(s16*)&temp_fp->field_A;

            temp_a0_2 = Q12_MULT(curPtr->field_8, temp_lo_2);
            temp_v1_4 = Q12_MULT(curPtr->field_6, temp_lo_2);

            setXY4(poly_gt4,
                   field_1C.vx, field_1C.vy - (temp_a0_2 * 2),
                   field_1C.vx + temp_v1_4, field_1C.vy - temp_a0_2,
                   field_1C.vx - temp_v1_4, field_1C.vy - temp_a0_2,
                   field_1C.vx, field_1C.vy);

#ifdef SH_PC_PORT
            /* PC billboard fog: encode fog factor in p1 pad byte.
             * Shader does mix(fragColor.rgb, u_fogColor, fogAmount). */
            {
                if (g_WorldEnvWork.isFogEnabled)
                {
                    s32 fogAmt = (func_80055A50(temp_v0_2 << 6) * 16) + g_WorldEnvWork.fog.intensity;
                    if (fogAmt > 0x1000) fogAmt = 0x1000;
                    poly_gt4->p1 = (u8)((fogAmt * 127) >> 12);
                    poly_gt4->p2 = poly_gt4->p1;
                    poly_gt4->p3 = poly_gt4->p1;
                }
                else
                {
                    poly_gt4->p1 = 0;
                    poly_gt4->p2 = 0;
                    poly_gt4->p3 = 0;
                }
                poly_gt4->pad2 = poly_gt4->p1; /* v0 fog (uniform billboard) */

                /* PGXP: billboard corners are built in screen space from the one
                 * projected centre (field_1C) — the 4th vertex IS that centre, so
                 * with PGXP it alone matches the ring (centre W) while the other
                 * corners go affine, warping the quad ("tree-foliage spikes").
                 * Billboards are camera-facing flat quads, so affine is correct. */
                PsyX_SetNextPrimAffine();
                addPrim(&ot_arg4->org[MIN(temp_v0_2 >> arg5, ORDERING_TABLE_SIZE - 1)], poly_gt4);
                poly_gt4++;
            }
#else
            if (worldEngWork1->isFogEnabled)
            {
                packet = poly_gt4 + 1;
                SetPriority(packet, 0, 0);
                packet2 = packet + 0xC; // TODO: `sizeof`?
                SetPriority(packet2, 1, 1);

                poly_g4 = packet2 + 0xC;

                temp = (func_80055A50(temp_v0_2 << 6) * 16) + worldEngWork1->fog.intensity;

                gte_lddp(0x1000 - MIN(temp, 0x1000));
                gte_ldrgb(&g_WorldEnvWork.fog.color);
                gte_dpcs();
                gte_strgb((CVECTOR*)&poly_g4->r0);

                *(s32*)&poly_g4->r1 = *(s32*)&poly_g4->r2 = *(s32*)&poly_g4->r3 = *(s32*)&poly_g4->r0;

                *(s32*)&poly_g4->x0 = *(s32*)&poly_gt4->x0;
                *(s32*)&poly_g4->x1 = *(s32*)&poly_gt4->x1;
                *(s32*)&poly_g4->x2 = *(s32*)&poly_gt4->x2;
                *(s32*)&poly_g4->x3 = *(s32*)&poly_gt4->x3;

                setPolyG4(poly_g4);
                setSemiTrans(poly_g4, 1);

                addPrim(&ot_arg4->org[temp_v0_2 >> arg5], packet);
                addPrim(&ot_arg4->org[temp_v0_2 >> arg5], poly_g4);
                addPrim(&ot_arg4->org[temp_v0_2 >> arg5], packet2);
                addPrim(&ot_arg4->org[temp_v0_2 >> arg5], poly_gt4);
                poly_g4++;
                poly_gt4 = poly_g4;
            }
            else
            {
                addPrim(&ot_arg4->org[temp_v0_2 >> arg5], poly_gt4);
                poly_gt4++;
            }
#endif
        }
    }

    GsOUT_PACKET_P = poly_gt4;
}

// ========================================
// UNUSED
// ========================================
// Split from something related to a cut debug feature?

void Gfx_DebugStringPositionSet(s16 unused, s16 posX, s16 posY) // 0x8005BF0C
{
    Text_Debug_PositionSet(posX, posY);
}

/* PC port: upstream particle.c calls these (new upstream functions). Our s_WorldEnvWork
 * keeps the flat fork light-region layout, so map by role: field_60 = light position
 * (VECTOR3), field_58 = light rotation (SVECTOR, fork-commented "Rotation"),
 * field_54 = light intensity scalar. Verify flashlight particle placement at runtime. */
void WorldEnv_LightPositionGet(VECTOR3* pos)
{
    *pos = g_WorldEnvWork.field_60;
}

s32 WorldEnv_LightRotationAndIntensityGet(SVECTOR* rot)
{
    *rot = g_WorldEnvWork.field_58;
    return g_WorldEnvWork.field_54;
}
