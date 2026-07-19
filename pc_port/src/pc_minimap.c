/* Config-only PC minimap overlay (corner/shape/opacity configurable).
 *
 * Drawn with PSX PRIMITIVES into the game's own display list — the same path
 * Pc_HealFlashUpdate and every other 2D HUD element uses. An earlier raw-GL
 * overlay was abandoned: borrowing PsyX's live GL context meant any unrestored
 * state corrupted the renderer and killed the NVIDIA driver's async worker.
 *
 * The area map IMAGE is a PC-side texture backing an ordinary textured prim, via
 * the hires-override POOL SLOT mechanism (the same trick pc_decals.c uses): the
 * paper-map TIM is read, decoded to RGBA and registered into a dedicated slot,
 * and the prim addresses it purely through its clut word — no PSX VRAM involved,
 * which sidesteps the residency conflict with the paper-map screen entirely.
 *
 * The file read is deferred and non-blocking: enqueue only after the FS queue has
 * been idle a while, then POLL for completion. Never call Fs_QueueWaitForEmpty
 * here — it pumps the queue and runs post-load callbacks (GL texture uploads) at
 * a point the game does not expect, which corrupted area loads.
 *
 * Harry's position comes from the game's own world->map transform (func_80067914)
 * in query mode, so the marker cannot drift from the real paper map. Areas with no
 * paper map show an empty dark disc.
 *
 * Off by default: g_PcConfig.minimap == 0 early-returns before any work. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_PaperMapFileIdxs */
#include "pc_config.h"
#include "hires_override.h"
#include "sh_log.h"
#include <stdlib.h>

/* Set while polling func_80067914 for coords only (suppresses its arrow draw). */
int g_PcMapQueryOnly = 0;

/* Panel geometry in 2D screen space (origin = screen centre; 4:3 is
 * x -160..+160, y -120..+120). Top-left corner, inset a little. */
#define MM_SIZE   72
#define MM_MARGIN 10

/* Dedicated pool slot for the map image. Must avoid every allocated range in
 * hires_override.h: 0..254 map chunks, 255 decal, 256+ chara, 320..447 chara
 * spill. 448..511 is free, and living above the chara base means
 * HiresOverride_PoolSlotsReset (which only frees below it) won't drop us on map
 * init. Prim clut encoding is the canonical one from hires_override.h. */
#define MM_POOL_SLOT (HIRES_POOL_SLOT_MAX - 1)
#define MM_CLUT                                                                \
    (u16)((((HIRES_POOL_CLUT_ROW_BASE + (MM_POOL_SLOT / 64) * HIRES_POOL_MAX_ROWS)) << 6) | \
          (MM_POOL_SLOT % 64))
/* UV denominator: the shader maps 0..native over the GL texture, so any decoded
 * map resolution works and UVs stay in the u8 prim range. */
#define MM_NATIVE 256
#define MM_UV_MAX (MM_NATIVE - 1)
#define MM_ZOOM   3      /* show ~1/3 of the map around Harry */

#define MM_SEG     20   /* triangle-fan segments approximating the circle */
#define MM_OUTLINE 2    /* black outline thickness, in the same 2D units */

/* Map-cell extents the paper map spans; used to place Harry on the image. */
#define MM_HALF_X 160
#define MM_HALF_Z 240

static int s_mapIdxLoaded = -1;
static unsigned char* s_rawBuf = NULL;
static unsigned int   s_rawSize = 0;
static int s_readPending = 0;
static int s_idleFrames  = 0;
static int s_mapReady    = 0;   /* pool slot holds this area's map */

static int mm_clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/* Deferred, non-blocking load of the area's paper-map TIM into the pool slot. */
static void mm_map_load_tick(int idx)
{
    if (s_readPending)
    {
        unsigned char* rgba = NULL;
        int            w = 0, h = 0;

        if (Fs_QueueGetLength() > 0) return;   /* poll only — never pump */
        s_readPending = 0;

        if (HiresOverride_DecodeToRGBA(s_rawBuf, s_rawSize, &rgba, &w, &h) == 0 && rgba != NULL)
        {
            s_mapReady = (HiresOverride_PoolSlotRegisterRGBA(MM_POOL_SLOT, 0, rgba, w, h,
                                                             MM_NATIVE, MM_NATIVE) == 0);
            free(rgba);
        }
        free(s_rawBuf); s_rawBuf = NULL;
        SH_DBG("[MINIMAP] map %dx%d -> slot %d %s", w, h, MM_POOL_SLOT,
               s_mapReady ? "registered" : "FAILED");
        return;
    }

    if (idx == s_mapIdxLoaded) return;

    /* Only start once the queue has been continuously idle: during an area load it
     * dips to empty between the game's own reads, and slipping a read in there
     * corrupted the load. The map is cosmetic; it can appear a little late. */
    if (Fs_QueueGetLength() > 0) { s_idleFrames = 0; return; }
    if (++s_idleFrames < 180) return;

    {
        e_FsFile f    = (e_FsFile)(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[idx]);
        s32      size = Fs_GetFileSize(f);

        if (size <= 0) { s_mapIdxLoaded = idx; return; }
        s_rawBuf = (unsigned char*)malloc((size_t)size + 2048);
        if (s_rawBuf == NULL) return;          /* retry later */

        s_mapIdxLoaded = idx;
        s_rawSize      = (unsigned int)size;
        s_mapReady     = 0;
        Fs_QueueStartRead(f, s_rawBuf);
        s_readPending  = 1;
        SH_DBG("[MINIMAP] queued map read: paperMapIdx=%d file=%d size=%d", idx, (int)f, (int)size);
    }
}

void Pc_MinimapUpdate(void)
{
    static POLY_F3  s_ring[2][MM_SEG];   /* circle: black outline + backing disc */
    static POLY_F3  s_fill[2][MM_SEG];   /* circle: dark interior when no map */
    static POLY_FT3 s_fan[2][MM_SEG];    /* circle: the map image */
    static TILE     s_sqOut[2];          /* square: black outline */
    static TILE     s_sqFill[2];         /* square: dark interior when no map */
    static POLY_FT4 s_sqMap[2];          /* square: the map image */
    static POLY_F3  s_mark[2];
    static DR_TPAGE s_tp[2];
    int       buf, i;
    s32       packed, px, py;
    int       haveMap, round, semi, op, lum;
    int       x0, y0, x1, y1, cx, cy, R;
    int       u0 = 0, v0 = 0, u1 = MM_UV_MAX, v1 = MM_UV_MAX;
    GsOT_TAG* ot;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay) return;

    /* PSX blending only offers fixed ratios (abr), so opacity is approximated:
     * under 100 the prims draw semi-transparent (50%) and the colour is scaled by
     * the percentage on top, which reads as a fade. 0 = don't draw at all. */
    op = (int)g_PcConfig.minimapOpacity;
    if (op <= 0) return;
    if (op > 100) op = 100;
    semi = (op < 100);
    lum  = (128 * op) / 100;
    if (lum < 1) lum = 1;

    mm_map_load_tick((int)g_SavegamePtr->paperMapIdx);

    buf   = g_ActiveBufferIdx;
    ot    = &g_OtTags0[buf][4];
    round = (g_PcConfig.minimapShape != 0);

    /* --- corner placement (4:3 bounds: x -160..+160, y -120..+120) --- */
    switch (g_PcConfig.minimapCorner)
    {
        case 1:  x0 =  (SCREEN_WIDTH / 2) - MM_MARGIN - MM_SIZE;
                 y0 = -(SCREEN_HEIGHT / 2) + MM_MARGIN; break;
        case 2:  x0 = -(SCREEN_WIDTH / 2) + MM_MARGIN;
                 y0 =  (SCREEN_HEIGHT / 2) - MM_MARGIN - MM_SIZE; break;
        case 3:  x0 =  (SCREEN_WIDTH / 2) - MM_MARGIN - MM_SIZE;
                 y0 =  (SCREEN_HEIGHT / 2) - MM_MARGIN - MM_SIZE; break;
        default: x0 = -(SCREEN_WIDTH / 2) + MM_MARGIN;
                 y0 = -(SCREEN_HEIGHT / 2) + MM_MARGIN; break;
    }
    x1 = x0 + MM_SIZE; y1 = y0 + MM_SIZE;
    cx = x0 + MM_SIZE / 2; cy = y0 + MM_SIZE / 2; R = MM_SIZE / 2;

    /* --- Harry's cell on the paper map (query mode: no arrow drawn) --- */
    g_PcMapQueryOnly = 1;
    packed = func_80067914((s32)g_SavegamePtr->paperMapIdx, 0, 0, (u16)Q12(1.0f));
    g_PcMapQueryOnly = 0;

    haveMap = (s_mapReady && packed != 0);

    if (packed != 0)
    {
        s32 mx = (s32)(s16)(packed & 0xFFFF);
        s32 mz = (s32)(s16)((packed >> 16) & 0xFFFF);
        int cu = mm_clamp(((mx + MM_HALF_X) * MM_UV_MAX) / (2 * MM_HALF_X), 0, MM_UV_MAX);
        int cv = mm_clamp(((mz + MM_HALF_Z) * MM_UV_MAX) / (2 * MM_HALF_Z), 0, MM_UV_MAX);
        int half = MM_UV_MAX / (2 * MM_ZOOM);

        u0 = mm_clamp(cu - half, 0, MM_UV_MAX - 2 * half);
        v0 = mm_clamp(cv - half, 0, MM_UV_MAX - 2 * half);
        u1 = u0 + 2 * half;
        v1 = v0 + 2 * half;

        px = x0 + ((cu - u0) * MM_SIZE) / (u1 - u0);
        py = y0 + ((cv - v0) * MM_SIZE) / (v1 - v0);
    }
    else
    {
        px = cx;
        py = cy;
    }

    /* keep the arrow inside the panel (disc or box) */
    if (round)
    {
        s32 dx = px - cx, dy = py - cy;
        s32 lim = R - 5, d = dx * dx + dy * dy;
        if (d > lim * lim)
        {
            s32 k = 0;
            while ((k + 1) * (k + 1) <= d) k++;      /* integer sqrt */
            if (k > 0) { px = cx + (dx * lim) / k; py = cy + (dy * lim) / k; }
        }
    }
    else
    {
        px = mm_clamp(px, x0 + 4, x1 - 4);
        py = mm_clamp(py, y0 + 4, y1 - 4);
    }

    if (round)
    {
        for (i = 0; i < MM_SEG; i++)
        {
            q3_12 a0 = (q3_12)((4096 * i) / MM_SEG);
            q3_12 a1 = (q3_12)((4096 * (i + 1)) / MM_SEG);
            s32   c0 = Math_Cos(a0), n0 = Math_Sin(a0);
            s32   c1 = Math_Cos(a1), n1 = Math_Sin(a1);
            s32   Ro = R + MM_OUTLINE;

            setPolyF3(&s_ring[buf][i]);
            setRGB0(&s_ring[buf][i], 0, 0, 0);
            if (semi) setSemiTrans(&s_ring[buf][i], 1);
            setXY3(&s_ring[buf][i], cx, cy,
                   cx + ((Ro * c0) >> 12), cy + ((Ro * n0) >> 12),
                   cx + ((Ro * c1) >> 12), cy + ((Ro * n1) >> 12));

            if (haveMap)
            {
                s32 nx0 = 2048 + (c0 >> 1), ny0 = 2048 + (n0 >> 1);
                s32 nx1 = 2048 + (c1 >> 1), ny1 = 2048 + (n1 >> 1);
                setPolyFT3(&s_fan[buf][i]);
                setRGB0(&s_fan[buf][i], (u8)lum, (u8)lum, (u8)lum);
                if (semi) setSemiTrans(&s_fan[buf][i], 1);
                setXY3(&s_fan[buf][i], cx, cy,
                       cx + ((R * c0) >> 12), cy + ((R * n0) >> 12),
                       cx + ((R * c1) >> 12), cy + ((R * n1) >> 12));
                setUV3(&s_fan[buf][i],
                       (u8)((u0 + u1) / 2),                  (u8)((v0 + v1) / 2),
                       (u8)(u0 + (((u1 - u0) * nx0) >> 12)), (u8)(v0 + (((v1 - v0) * ny0) >> 12)),
                       (u8)(u0 + (((u1 - u0) * nx1) >> 12)), (u8)(v0 + (((v1 - v0) * ny1) >> 12)));
                s_fan[buf][i].tpage = 0;
                s_fan[buf][i].clut  = MM_CLUT;
            }
            else
            {
                setPolyF3(&s_fill[buf][i]);
                setRGB0(&s_fill[buf][i], (u8)((16 * op) / 100), (u8)((20 * op) / 100), (u8)((30 * op) / 100));
                if (semi) setSemiTrans(&s_fill[buf][i], 1);
                setXY3(&s_fill[buf][i], cx, cy,
                       cx + ((R * c0) >> 12), cy + ((R * n0) >> 12),
                       cx + ((R * c1) >> 12), cy + ((R * n1) >> 12));
            }
        }
    }
    else
    {
        setTile(&s_sqOut[buf]);
        setRGB0(&s_sqOut[buf], 0, 0, 0);
        if (semi) setSemiTrans(&s_sqOut[buf], 1);
        setWH(&s_sqOut[buf], MM_SIZE + MM_OUTLINE * 2, MM_SIZE + MM_OUTLINE * 2);
        setXY0(&s_sqOut[buf], x0 - MM_OUTLINE, y0 - MM_OUTLINE);

        if (haveMap)
        {
            setPolyFT4(&s_sqMap[buf]);
            setRGB0(&s_sqMap[buf], (u8)lum, (u8)lum, (u8)lum);
            if (semi) setSemiTrans(&s_sqMap[buf], 1);
            setXY4(&s_sqMap[buf], x0, y0, x1, y0, x0, y1, x1, y1);
            s_sqMap[buf].tpage = 0;
            s_sqMap[buf].clut  = MM_CLUT;
            s_sqMap[buf].u0 = (u8)u0; s_sqMap[buf].v0 = (u8)v0;
            s_sqMap[buf].u1 = (u8)u1; s_sqMap[buf].v1 = (u8)v0;
            s_sqMap[buf].u2 = (u8)u0; s_sqMap[buf].v2 = (u8)v1;
            s_sqMap[buf].u3 = (u8)u1; s_sqMap[buf].v3 = (u8)v1;
        }
        else
        {
            setTile(&s_sqFill[buf]);
            setRGB0(&s_sqFill[buf], (u8)((16 * op) / 100), (u8)((20 * op) / 100), (u8)((30 * op) / 100));
            if (semi) setSemiTrans(&s_sqFill[buf], 1);
            setWH(&s_sqFill[buf], MM_SIZE, MM_SIZE);
            setXY0(&s_sqFill[buf], x0, y0);
        }
    }

    /* --- marker: small triangle along Harry's heading (+Y is down => north is -Y) --- */
    {
        static const s32 lx[3] = {  0, -3,  3 };
        static const s32 ly[3] = { -5,  3,  3 };
        q3_12 ang = g_SysWork.playerWork.player.rotation.vy;
        s32   ca  = Math_Cos(ang), sa = Math_Sin(ang);
        s32   vx[3], vy[3];
        for (i = 0; i < 3; i++)
        {
            vx[i] = px + ((lx[i] * ca - ly[i] * sa) >> 12);
            vy[i] = py + ((lx[i] * sa + ly[i] * ca) >> 12);
        }
        setPolyF3(&s_mark[buf]);
        setRGB0(&s_mark[buf], (u8)((255 * op) / 100), (u8)((216 * op) / 100), (u8)((48 * op) / 100));
        if (semi) setSemiTrans(&s_mark[buf], 1);
        setXY3(&s_mark[buf], vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
    }

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0)); /* abr=0 = 50% blend */

    /* AddPrim pushes to the head: add front-to-back, tpage last. */
    AddPrim(ot, &s_mark[buf]);
    if (round)
    {
        for (i = 0; i < MM_SEG; i++)
            AddPrim(ot, haveMap ? (void*)&s_fan[buf][i] : (void*)&s_fill[buf][i]);
        for (i = 0; i < MM_SEG; i++)
            AddPrim(ot, &s_ring[buf][i]);
    }
    else
    {
        AddPrim(ot, haveMap ? (void*)&s_sqMap[buf] : (void*)&s_sqFill[buf]);
        AddPrim(ot, &s_sqOut[buf]);
    }
    AddPrim(ot, &s_tp[buf]);
}

/* The GL overlay is gone; keep the symbol so dbg_overlay.c's call is harmless. */
void Pc_MinimapDraw(void) {}
