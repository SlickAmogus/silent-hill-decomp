/* Config-only PC minimap overlay (top-left).
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
 * paper map fall back to a scrolling grid.
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

#define MM_GRID_LINES 5
#define MM_CELL       40
#define MM_VIEW       100

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
    static TILE     s_panel[2];
    static TILE     s_grid[2][MM_GRID_LINES * 2];
    static POLY_FT4 s_map[2];
    static POLY_F3  s_mark[2];
    static DR_TPAGE s_tp[2];
    int       buf, i, n = 0;
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
        /* Harry in map-image UV space. */
        s32 mx = (s32)(s16)(packed & 0xFFFF);
        s32 mz = (s32)(s16)((packed >> 16) & 0xFFFF);
        int cu = mm_clamp(((mx + MM_HALF_X) * MM_UV_MAX) / (2 * MM_HALF_X), 0, MM_UV_MAX);
        int cv = mm_clamp(((mz + MM_HALF_Z) * MM_UV_MAX) / (2 * MM_HALF_Z), 0, MM_UV_MAX);
        int half = MM_UV_MAX / (2 * MM_ZOOM);

        /* Zoom window centred on Harry, clamped to the image edges. */
        u0 = mm_clamp(cu - half, 0, MM_UV_MAX - 2 * half);
        v0 = mm_clamp(cv - half, 0, MM_UV_MAX - 2 * half);
        u1 = u0 + 2 * half;
        v1 = v0 + 2 * half;

        /* Marker sits where Harry falls inside that window (centre normally, and
         * correctly off-centre once the window clamps at a map edge). */
        px = MM_X0 + ((cu - u0) * MM_SIZE) / (u1 - u0);
        py = MM_Y0 + ((cv - v0) * MM_SIZE) / (v1 - v0);
    }
    else
    {
        px = MM_X0 + MM_SIZE / 2;
        py = MM_Y0 + MM_SIZE / 2;
    }
    px = mm_clamp(px, MM_X0 + 4, MM_X1 - 4);
    py = mm_clamp(py, MM_Y0 + 4, MM_Y1 - 4);

    /* --- marker: small triangle along Harry's heading (screen +Y is down, so
     *     "north" is -Y at angle 0) --- */
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

    /* --- map image, or a scrolling grid where the area has no paper map --- */
    if (haveMap)
    {
        setPolyFT4(&s_map[buf]);
        setRGB0(&s_map[buf], 128, 128, 128); /* neutral = unmodulated */
        setXY4(&s_map[buf], MM_X0, MM_Y0, MM_X1, MM_Y0, MM_X0, MM_Y1, MM_X1, MM_Y1);
        /* tpage is irrelevant: the bit-15 clut alone keys the pool-slot override. */
        s_map[buf].tpage = 0;
        s_map[buf].clut  = MM_CLUT;
        s_map[buf].u0 = (u8)u0; s_map[buf].v0 = (u8)v0;
        s_map[buf].u1 = (u8)u1; s_map[buf].v1 = (u8)v0;
        s_map[buf].u2 = (u8)u0; s_map[buf].v2 = (u8)v1;
        s_map[buf].u3 = (u8)u1; s_map[buf].v3 = (u8)v1;
    }
    else
    {
        s32 hx = g_SysWork.playerWork.player.position.vx >> 12;
        s32 hz = g_SysWork.playerWork.player.position.vz >> 12;
        for (i = 0; i < MM_GRID_LINES; i++)
        {
            s32 wx = ((hx - MM_VIEW) / MM_CELL + i) * MM_CELL;
            s32 wz = ((hz - MM_VIEW) / MM_CELL + i) * MM_CELL;
            s32 gx = MM_X0 + (((wx - hx) + MM_VIEW) * MM_SIZE) / (2 * MM_VIEW);
            s32 gy = MM_Y0 + (((wz - hz) + MM_VIEW) * MM_SIZE) / (2 * MM_VIEW);
            if (gx > MM_X0 && gx < MM_X1)
            {
                setTile(&s_grid[buf][n]);
                setRGB0(&s_grid[buf][n], 72, 88, 104);
                setWH(&s_grid[buf][n], 1, MM_SIZE);
                setXY0(&s_grid[buf][n], gx, MM_Y0);
                n++;
            }
            if (gy > MM_Y0 && gy < MM_Y1)
            {
                setTile(&s_grid[buf][n]);
                setRGB0(&s_grid[buf][n], 72, 88, 104);
                setWH(&s_grid[buf][n], MM_SIZE, 1);
                setXY0(&s_grid[buf][n], MM_X0, gy);
                n++;
            }
        }
    }

    /* --- panel background --- */
    setTile(&s_panel[buf]);
    setSemiTrans(&s_panel[buf], 1);
    setRGB0(&s_panel[buf], 16, 20, 30);
    setWH(&s_panel[buf], MM_SIZE, MM_SIZE);
    setXY0(&s_panel[buf], MM_X0, MM_Y0);

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0)); /* abr=0 = 50% blend */

    /* AddPrim pushes to the head, so the LAST added is processed FIRST: add
     * front-to-back (marker, map/grid, panel) and the tpage last. */
    AddPrim(ot, &s_mark[buf]);
    if (haveMap)
        AddPrim(ot, &s_map[buf]);
    else
        for (i = 0; i < n; i++) AddPrim(ot, &s_grid[buf][i]);
    AddPrim(ot, &s_panel[buf]);
    AddPrim(ot, &s_tp[buf]);
}

/* The GL overlay is gone; keep the symbol so dbg_overlay.c's call is harmless. */
void Pc_MinimapDraw(void) {}
