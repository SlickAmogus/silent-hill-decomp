/* Config-only PC minimap overlay (top-left, circular).
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
#define MM_X0     (-(SCREEN_WIDTH / 2) + 10)
#define MM_Y0     (-(SCREEN_HEIGHT / 2) + 10)
#define MM_X1     (MM_X0 + MM_SIZE)
#define MM_Y1     (MM_Y0 + MM_SIZE)

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
#define MM_R       (MM_SIZE / 2)
#define MM_OUTLINE 2    /* black ring thickness, in the same 2D units */
#define MM_CX      (MM_X0 + MM_SIZE / 2)
#define MM_CY      (MM_Y0 + MM_SIZE / 2)

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
    static POLY_F3  s_ring[2][MM_SEG];   /* black outline + backing disc */
    static POLY_F3  s_fill[2][MM_SEG];   /* dark interior when no map */
    static POLY_FT3 s_fan[2][MM_SEG];    /* the map image */
    static POLY_F3  s_mark[2];
    static DR_TPAGE s_tp[2];
    int       buf, i;
    s32       packed, px, py;
    int       haveMap;
    int       u0 = 0, v0 = 0, u1 = MM_UV_MAX, v1 = MM_UV_MAX;
    GsOT_TAG* ot;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay) return;

    mm_map_load_tick((int)g_SavegamePtr->paperMapIdx);

    buf = g_ActiveBufferIdx;
    ot  = &g_OtTags0[buf][4];

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

        px = MM_CX + (((cu - u0) * 2 - (u1 - u0)) * MM_R) / (u1 - u0);
        py = MM_CY + (((cv - v0) * 2 - (v1 - v0)) * MM_R) / (v1 - v0);
    }
    else
    {
        px = MM_CX;
        py = MM_CY;
    }
    /* keep the arrow inside the disc */
    {
        s32 dx = px - MM_CX, dy = py - MM_CY;
        s32 lim = MM_R - 5;
        if (dx * dx + dy * dy > lim * lim)
        {
            s32 d = dx * dx + dy * dy;
            s32 k = 0;
            while ((k + 1) * (k + 1) <= d) k++;      /* integer sqrt */
            if (k > 0) { px = MM_CX + (dx * lim) / k; py = MM_CY + (dy * lim) / k; }
        }
    }

    /* --- circle fans: black ring underneath, then the map (or a dark disc) --- */
    for (i = 0; i < MM_SEG; i++)
    {
        q3_12 a0 = (q3_12)((4096 * i) / MM_SEG);
        q3_12 a1 = (q3_12)((4096 * (i + 1)) / MM_SEG);
        s32   c0 = Math_Cos(a0), n0 = Math_Sin(a0);
        s32   c1 = Math_Cos(a1), n1 = Math_Sin(a1);
        s32   Ro = MM_R + MM_OUTLINE;

        setPolyF3(&s_ring[buf][i]);
        setRGB0(&s_ring[buf][i], 0, 0, 0);
        setXY3(&s_ring[buf][i], MM_CX, MM_CY,
               MM_CX + ((Ro * c0) >> 12), MM_CY + ((Ro * n0) >> 12),
               MM_CX + ((Ro * c1) >> 12), MM_CY + ((Ro * n1) >> 12));

        if (haveMap)
        {
            /* vertex position within the panel, 0..4096, -> zoom-window UV */
            s32 nx0 = 2048 + (c0 >> 1), ny0 = 2048 + (n0 >> 1);
            s32 nx1 = 2048 + (c1 >> 1), ny1 = 2048 + (n1 >> 1);
            setPolyFT3(&s_fan[buf][i]);
            setRGB0(&s_fan[buf][i], 128, 128, 128);  /* neutral = unmodulated */
            setXY3(&s_fan[buf][i], MM_CX, MM_CY,
                   MM_CX + ((MM_R * c0) >> 12), MM_CY + ((MM_R * n0) >> 12),
                   MM_CX + ((MM_R * c1) >> 12), MM_CY + ((MM_R * n1) >> 12));
            setUV3(&s_fan[buf][i],
                   (u8)((u0 + u1) / 2),                 (u8)((v0 + v1) / 2),
                   (u8)(u0 + (((u1 - u0) * nx0) >> 12)), (u8)(v0 + (((v1 - v0) * ny0) >> 12)),
                   (u8)(u0 + (((u1 - u0) * nx1) >> 12)), (u8)(v0 + (((v1 - v0) * ny1) >> 12)));
            /* tpage irrelevant: the bit-15 clut alone keys the pool-slot override */
            s_fan[buf][i].tpage = 0;
            s_fan[buf][i].clut  = MM_CLUT;
        }
        else
        {
            setPolyF3(&s_fill[buf][i]);
            setRGB0(&s_fill[buf][i], 16, 20, 30);
            setXY3(&s_fill[buf][i], MM_CX, MM_CY,
                   MM_CX + ((MM_R * c0) >> 12), MM_CY + ((MM_R * n0) >> 12),
                   MM_CX + ((MM_R * c1) >> 12), MM_CY + ((MM_R * n1) >> 12));
        }
    }

    /* --- marker: small triangle along Harry's heading (+Y is down, so north is -Y) --- */
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
        setRGB0(&s_mark[buf], 255, 216, 48);
        setXY3(&s_mark[buf], vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
    }

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0));

    /* AddPrim pushes to the head, so the LAST added is processed FIRST: add
     * front-to-back (marker, map/fill, ring) and the tpage last. */
    AddPrim(ot, &s_mark[buf]);
    for (i = 0; i < MM_SEG; i++)
        AddPrim(ot, haveMap ? (void*)&s_fan[buf][i] : (void*)&s_fill[buf][i]);
    for (i = 0; i < MM_SEG; i++)
        AddPrim(ot, &s_ring[buf][i]);
    AddPrim(ot, &s_tp[buf]);
}

/* The GL overlay is gone; keep the symbol so dbg_overlay.c's call is harmless. */
void Pc_MinimapDraw(void) {}
