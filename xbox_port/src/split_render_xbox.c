/*
 * split_render_xbox.c - split screen's second view.
 *
 * View 1 is the game's own world ordering table (OT0), built exactly as for one
 * player. View 2 is a second ordering table of the same shape: every world draw
 * that should appear in Player 2's view is issued a second time between
 * SplitRender_View2Begin and SplitRender_View2End, which publish Player 2's
 * camera (the matrices vwSetViewInfo publishes, plus the GTE projection
 * distance), Player 2's light (g_WorldEnvWork), and swap the OT0 header for view
 * 2's, so the stock draw code lands in view 2 without knowing it. Almost every
 * world emitter writes through g_OrderingTable0[g_ActiveBufferIdx], and none of
 * them keeps a pointer to it across a frame, which is what makes the header swap
 * enough.
 *
 * At present time (game_main.c) SplitRender_Present sanitizes both tables and
 * walks each into its own part of the screen (GpuXbox_ViewBegin); OT2, the 2D
 * layer, is drawn full-screen afterwards as always.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/map/terrain.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vw_system.h"
#include "gpu_nv2a.h"
#include "pc_config.h"
#include "split_xbox.h"
#include "split_internal_xbox.h"
#include "sh_log.h"

#include <stdint.h>
#include <string.h>

extern int            g_Nv2aContentX, g_Nv2aContentW, g_Nv2aContentH;
extern s_WorldEnvWork g_WorldEnvWork;
extern void           GsSortOt_GetSubrootBounds(uintptr_t* lo, uintptr_t* hi);
extern void           Pc_FarOt_GetBounds(uintptr_t* lo, uintptr_t* hi);
extern void           Audio_XboxPump(void);

/* ---- view 2's ordering table ---- */

#define V2_OT_LEN 11
static GsOT_TAG s_v2Tags[2][1 << V2_OT_LEN];
static GsOT     s_v2Ot[2] = {
    { V2_OT_LEN, s_v2Tags[0], 0, 0, 0 },
    { V2_OT_LEN, s_v2Tags[1], 0, 0, 0 }
};

/* This frame's view 2 has been built (Player 2's camera captured and the world
 * issued into s_v2Ot); only then does present take the two-view path. */
static int s_v2Built;

/* ---- the per-view state a world draw reads ---- */

typedef struct
{
    VW_VIEW_WORK   vp;
    MATRIX         c3868;
    MATRIX         wvs;
    MATRIX         ws;
    s32            h;
    int            valid;
} SplitViewPub;

static SplitViewPub   s_pub[2];
static s_WorldEnvWork s_env2;
static int            s_env2Valid;

/* View 2 bracket state */
static int            s_inV2;
static GsOT           s_v2SavedOt0;
static SplitViewPub   s_v2SavedPub;
static s_WorldEnvWork s_v2SavedEnv;

static void Sr_PubRead(SplitViewPub* p)
{
    p->vp    = vwViewPointInfo;
    p->c3868 = D_800C3868;
    p->wvs   = VbWvsMatrix;
    p->ws    = GsWSMATRIX;
    p->h     = ReadGeomScreen();
}

static void Sr_PubWrite(const SplitViewPub* p)
{
    vwViewPointInfo = p->vp;
    /* vwViewPointInfo.rview.super points at its own vwcoord; the copy carries the
     * address of the original, which is the same object, so nothing to fix. */
    D_800C3868  = p->c3868;
    VbWvsMatrix = p->wvs;
    GsWSMATRIX  = p->ws;
    SetGeomScreen(p->h);
}

void SplitRender_CapturePub(int v)
{
    Sr_PubRead(&s_pub[v & 1]);
    s_pub[v & 1].valid = 1;
}

void SplitRender_PublishPub(int v)
{
    if (s_pub[v & 1].valid)
        Sr_PubWrite(&s_pub[v & 1]);
}

const VECTOR3* SplitRender_ViewPos(int v)
{
    static VECTOR3 s_pos[2];

    if (!s_pub[v & 1].valid)
        return NULL;
    /* VW_VIEW_WORK.worldpos is the published camera position (Q19.12). */
    s_pos[v & 1] = s_pub[v & 1].vp.worldpos;
    return &s_pos[v & 1];
}

void SplitRender_CaptureEnv2(void)
{
    s_env2      = g_WorldEnvWork;
    s_env2Valid = 1;
}

/* ---- frame lifecycle ---- */

/* View 1's camera is captured every frame the world updates. View 2's camera and
 * light are kept from the last frame the other player ran: while that player is
 * frozen (a message, a pickup) view 2 keeps showing it from where it stands. */
void SplitRender_FrameStart(void)
{
    s_v2Built      = 0;
    s_pub[0].valid = 0;
    s_inV2         = 0;
    GsClearOt(0, 0, &s_v2Ot[g_ActiveBufferIdx]);
}

/* The slots changed hands: the camera that was view 1's is now the other
 * player's, and the light view 1 drew with is the new side player's. */
void SplitRender_PubSwap(void)
{
    SplitViewPub t = s_pub[0];

    s_pub[0]    = s_pub[1];
    s_pub[1]    = t;
    s_env2      = g_WorldEnvWork;
    s_env2Valid = 1;
}

void SplitRender_Reset(void)
{
    memset(s_pub, 0, sizeof(s_pub));
    s_env2Valid = 0;
    s_v2Built   = 0;
    s_inV2      = 0;
}

void SplitRender_MarkBuilt(void)
{
    s_v2Built = 1;
}

/* Issue what follows into view 2. Returns 0 (and does nothing) when this frame
 * has no second view; nests never. */
int SplitRender_View2Begin(void)
{
    const int buf = g_ActiveBufferIdx;

    if (!g_XbSplitTwoViews || s_inV2 || !s_pub[1].valid)
        return 0;

    s_inV2 = 1;
    Sr_PubRead(&s_v2SavedPub);
    Sr_PubWrite(&s_pub[1]);
    if (s_env2Valid)
    {
        s_v2SavedEnv   = g_WorldEnvWork;
        g_WorldEnvWork = s_env2;
    }
    s_v2SavedOt0          = g_OrderingTable0[buf];
    g_OrderingTable0[buf] = s_v2Ot[buf];
    return 1;
}

void SplitRender_View2End(void)
{
    const int buf = g_ActiveBufferIdx;

    if (!s_inV2)
        return;
    /* The header's tag/point may have moved while view 2 was the live OT0. */
    s_v2Ot[buf]           = g_OrderingTable0[buf];
    g_OrderingTable0[buf] = s_v2SavedOt0;
    if (s_env2Valid)
        g_WorldEnvWork = s_v2SavedEnv;
    Sr_PubWrite(&s_v2SavedPub);
    s_inV2 = 0;
}

int SplitRender_InView2(void)
{
    return s_inV2;
}

/* ---- the world in view 2 ---- */

extern s_MapTerrain g_Map;
extern int          g_XbSplitStreamValid;
extern q19_12       g_XbSplitStreamPos[4];
extern void         Pc_DecalsDraw(GsOT* ot);

/* After Gfx_InGameDraw (game_sys_states.c): the map, the bullet decals and the
 * full-screen brightness / fog quads from the other player's camera. The world
 * objects went in already (Gfx_WorldObjectsDraw draws its queue into both views
 * before emptying it), characters inline at their own draws. Chunk visibility
 * keys off g_Map's view position and cell, so those become the other player's
 * for the call; streaming already ran for both players this frame and is not run
 * again. The flashlight halo (static packets) and the lens flare (stateful) are
 * left out of view 2 by zeroing their enables for the 2D-effects call.
 *
 * Not in view 2 at all: the weather (Particle_SystemUpdate) and the combat
 * effects (func_8005E89C: muzzle flash, blood, glass). Each simulates and draws
 * in one call, anchored on view 1's camera, and running either twice would
 * advance it twice; they need splitting into update and draw halves first. */
void Split_View2World(void)
{
    const int buf = g_ActiveBufferIdx;
    q19_12    sx = 0, sz = 0;
    s32       cx = 0, cz = 0;
    u8        f2;
    s16       f50;

    if (!SplitRender_View2Begin())
        return;

    if (g_XbSplitStreamValid)
    {
        sx = g_Map.positionX;
        sz = g_Map.positionZ;
        cx = g_Map.cellX;
        cz = g_Map.cellZ;
        g_Map.positionX = g_XbSplitStreamPos[2];
        g_Map.positionZ = g_XbSplitStreamPos[3];
        g_Map.cellX     = FLOOR_TO_STEP(Q12_TO_Q8(g_XbSplitStreamPos[2]), Q12_TO_Q8(CHUNK_CELL_SIZE));
        g_Map.cellZ     = FLOOR_TO_STEP(Q12_TO_Q8(g_XbSplitStreamPos[3]), Q12_TO_Q8(CHUNK_CELL_SIZE));
    }

    Ipd_ChunkCheckDraw(&g_OrderingTable0[buf], 1);
    Pc_DecalsDraw(&g_OrderingTable0[buf]);

    f2  = g_WorldEnvWork.field_2;
    f50 = g_WorldEnvWork.field_50;
    g_WorldEnvWork.field_2  = 0;
    g_WorldEnvWork.field_50 = 0;
    Gfx_2dEffectsDraw();
    g_WorldEnvWork.field_2  = f2;
    g_WorldEnvWork.field_50 = f50;

    if (g_XbSplitStreamValid)
    {
        g_Map.positionX = sx;
        g_Map.positionZ = sz;
        g_Map.cellX     = cx;
        g_Map.cellZ     = cz;
    }

    SplitRender_View2End();
    s_v2Built = 1;
}

/* ---- layouts ---- */

/* Each view's rect inside the content rect, and the part of the PSX frame it
 * shows. The band layouts keep today's pixel aspect (the crop is the rect at the
 * full-frame scale) and centre on the projection's optical axis, which sits at
 * frame (160, 120): the GTE offset is (0, 8) around the 320x224 frame's centre.
 * The whole-picture layouts show the full frame at half scale. */
void SplitRender_ViewRect(int layout, int idx, GpuXboxView* v)
{
    const int   cx = g_Nv2aContentX, cw = g_Nv2aContentW, ch = g_Nv2aContentH;
    const float frameW = 320.0f, frameH = 224.0f;

    v->index = idx;
    switch (layout)
    {
        default:
        case SPLIT_LAYOUT_TOPBOTTOM:
            v->x = cx;
            v->w = cw;
            v->h = idx ? ch - ch / 2 : ch / 2;
            v->y = idx ? ch / 2 : 0;
            v->cropW = frameW;
            v->cropX = 0.0f;
            v->cropH = (float)v->h * frameH / (float)ch;
            v->cropY = 120.0f - v->cropH * 0.5f;
            break;

        case SPLIT_LAYOUT_SIDEBYSIDE:
            v->y = 0;
            v->h = ch;
            v->w = idx ? cw - cw / 2 : cw / 2;
            v->x = idx ? cx + cw / 2 : cx;
            v->cropH = frameH;
            v->cropY = 0.0f;
            v->cropW = (float)v->w * frameW / (float)cw;
            v->cropX = 160.0f - v->cropW * 0.5f;
            break;

        case SPLIT_LAYOUT_TOPBOTTOM_WHOLE:
            v->w = cw / 2;
            v->h = ch / 2;
            v->x = cx + cw / 4;
            v->y = idx ? ch / 2 : 0;
            v->cropX = 0.0f;
            v->cropY = 0.0f;
            v->cropW = frameW;
            v->cropH = frameH;
            break;

        case SPLIT_LAYOUT_SIDEBYSIDE_WHOLE:
            v->w = cw / 2;
            v->h = ch / 2;
            v->x = idx ? cx + cw / 2 : cx;
            v->y = ch / 4;
            v->cropX = 0.0f;
            v->cropY = 0.0f;
            v->cropW = frameW;
            v->cropH = frameH;
            break;
    }
}

/* ---- present ---- */

/* game_main.c's OT0 sanitizer as a function, for split frames: the same pointer
 * windows and primitive whitelist, run on each view's table. Only view 1 keeps
 * the DR_AREA scene redirect; scenes never run split, so view 2 just drops it. */
static void Sr_Sanitize(GsOT* ot, uintptr_t pktLo, uintptr_t pktHi, int firstView)
{
    extern void GR_SetSceneFbRedirect(int x, int y, int w, int h);
    OT_TAG*   cur    = (OT_TAG*)ot->tag;
    OT_TAG*   prev   = NULL;
    uintptr_t otLo   = (uintptr_t)ot->org;
    int       otLen  = (ot->length > 0 && ot->length <= 16) ? (int)ot->length : 0;
    size_t    otCnt  = (size_t)1 << otLen;
    uintptr_t otHi   = (otLo && otLen) ? (otLo + otCnt * sizeof(GsOT_TAG)) : otLo;
    uintptr_t subLo  = 0, subHi = 0, farLo = 0, farHi = 0;
    s32       scratchX = 0, scratchY = 0, scratchW = 0, scratchH = 0;
    int       w2     = 0;

    GsSortOt_GetSubrootBounds(&subLo, &subHi);
    Pc_FarOt_GetBounds(&farLo, &farHi);

    while (cur && w2 < 8192)
    {
        uintptr_t curAddr = (uintptr_t)cur;
        int       curOk   = ((curAddr >= pktLo && curAddr < pktHi) ||
                             (curAddr >= otLo  && curAddr < otHi)  ||
                             (subLo && curAddr >= subLo && curAddr < subHi) ||
                             (curAddr >= farLo && curAddr < farHi));
        OT_TAG*   next;
        uintptr_t nextA;
        int       len;

        if (!curOk)
        {
            if (prev != NULL)
                setaddr(prev, &ot->org[0]);
            else
                ot->tag = &ot->org[0];
            break;
        }
        if (isendprim(cur))
            break;
        len = getlen(cur);
        if (len > 0)
        {
            u8 hi       = ((P_TAG*)cur)->code & 0xF0;
            u8 codeFull = ((P_TAG*)cur)->code;

            if (codeFull == 0xE3 && len == 2 && firstView)
            {
                DR_AREA* da  = (DR_AREA*)cur;
                s32      ax0 = (s32)(da->code[0] & 1023), ay0 = (s32)((da->code[0] >> 10) & 1023);
                s32      ax1 = (s32)(da->code[1] & 1023), ay1 = (s32)((da->code[1] >> 10) & 1023);

                if (ax0 >= 320 && ax1 > ax0 && ay1 > ay0)
                {
                    scratchX = ax0;
                    scratchY = ay0;
                    scratchW = ax1 - ax0;
                    scratchH = ay1 - ay0;
                    GR_SetSceneFbRedirect(scratchX, scratchY, scratchW, scratchH);
                }
                else if (scratchW > 0)
                {
                    da->code[0] = 0xB4000000u | ((u32)scratchX & 0x3FFu) | (((u32)scratchY & 0x1FFu) << 10);
                    da->code[1] = ((u32)scratchW & 0xFFFFu) | (((u32)scratchH & 0xFFFFu) << 16);
                    hi       = 0xB0;
                    codeFull = 0xB4;
                    scratchW = 0;
                }
            }
            if (len > 32 || (hi != 0x00 && hi != 0x20 && hi != 0x30 &&
                             hi != 0x40 && hi != 0x50 &&
                             hi != 0x60 && hi != 0x70 && hi != 0xA0 &&
                             hi != 0xB0 &&
                             codeFull != 0xE1 &&
                             hi != 0x24 && hi != 0x28 && hi != 0x2C &&
                             hi != 0x34 && hi != 0x38 && hi != 0x3C))
            {
                setlen(cur, 0);
            }
        }
        next  = (OT_TAG*)nextPrim(cur);
        nextA = (uintptr_t)next;
        /* 32-bit: the heap (packet arena) sits above 128MB, so a bare magnitude
         * cap is not a pointer test; accept the windows computed above. */
        if (next && (nextA < 0x1000 ||
                     !(nextA < (uintptr_t)0x08000000 ||
                       (nextA >= pktLo && nextA < pktHi) ||
                       (nextA >= otLo && nextA < otHi) ||
                       (subLo && nextA >= subLo && nextA < subHi))))
        {
            setaddr(cur, &ot->org[0]);
            break;
        }
        prev = cur;
        cur  = next;
        w2++;
    }
}

/* A 2-pixel black seam between the two views. */
static void Sr_Seam(int layout)
{
    const int cx = g_Nv2aContentX, cw = g_Nv2aContentW, ch = g_Nv2aContentH;

    if (layout == SPLIT_LAYOUT_TOPBOTTOM)
        GpuNv2a_ClearRect(cx, ch / 2 - 1, cw, 2, 0xFF000000u);
    else if (layout == SPLIT_LAYOUT_SIDEBYSIDE)
        GpuNv2a_ClearRect(cx + cw / 2 - 1, 0, 2, ch, 0xFF000000u);
}

/* Present both world views. Returns 0 when this is not a two-view frame, and
 * game_main.c then draws OT0 the single-player way. */
int SplitRender_Present(uintptr_t pktLo, uintptr_t pktHi, unsigned int clearArgb)
{
    const int   buf    = g_ActiveBufferIdx;
    const int   layout = g_PcConfig.splitLayout;
    const int   owner  = Split_ViewOwner() & 1;
    GpuXboxView v;

    if (!g_XbSplitTwoViews || !s_v2Built || s_inV2)
        return 0;

    Sr_Sanitize(&g_OrderingTable0[buf], pktLo, pktHi, 1);
    Sr_Sanitize(&s_v2Ot[buf], pktLo, pktHi, 0);

    if (layout == SPLIT_LAYOUT_TOPBOTTOM_WHOLE || layout == SPLIT_LAYOUT_SIDEBYSIDE_WHOLE)
        GpuNv2a_ClearRect(g_Nv2aContentX, 0, g_Nv2aContentW, g_Nv2aContentH, 0xFF000000u);

    /* OT0 was built from the camera of whoever is in the player slots, view 2's
     * table from the other player's: each goes into its own player's rect. */
    SplitRender_ViewRect(layout, owner, &v);
    v.index = 0;
    GpuXbox_ViewBegin(&v);
    GpuNv2a_ClearRect(v.x, v.y, v.w, v.h, clearArgb);
    GsDrawOt(&g_OrderingTable0[buf]);

    /* Two views take long enough that one audio pump a frame can run the ring
     * dry below ~15 fps; a second one here halves that floor. */
    Audio_XboxPump();

    SplitRender_ViewRect(layout, owner ^ 1, &v);
    v.index = 1;
    GpuXbox_ViewBegin(&v);
    GpuNv2a_ClearRect(v.x, v.y, v.w, v.h, clearArgb);
    GsDrawOt(&s_v2Ot[buf]);

    GpuXbox_ViewEnd();
    Sr_Seam(layout);
    GpuNv2a_SetScissor(0, 0, 0, 0);

    {
        static int s_logged = -1;
        if (s_logged != layout)
        {
            s_logged = layout;
            SH_DBG("[SPLIT] two views, layout %d: view rects (%d,%d %dx%d)", layout, v.x, v.y, v.w, v.h);
        }
    }
    return 1;
}
