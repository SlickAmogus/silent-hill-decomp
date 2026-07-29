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
#include "bodyprog/item_screens.h" /* s_800AEDBC (paper-map marking table) */
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_PaperMapFileIdxs */
#include "pc_config.h"
#include "hires_override.h"
#include "sh_log.h"
#include <stdlib.h>

/* The paper map's MARKINGS (blocked roads, locked/jammed doors, "no way through"
 * scribbles) come straight from the game's own tables in
 * bodyprog_mapscreen_80066D90.c — see func_80068E0C, which this mirrors:
 *
 *   D_800AEDBC[paperMapIdx] = { ptr_0 = sprite-cell table, ptr_4 = placement
 *                               table, field_8/field_A = event-flag range }
 *   placement (s_800AE8A0_4) = { s8 x, s8 y, u8 spriteA, u8 spriteB }
 *   sprite   (s_800AE8A0_0) = { u8 atlasU, u8 atlasV, u8 w, u8 h }
 *
 * Each placement owns TWO consecutive event flags (field_8 + i*2 + j + 1); flag
 * j picks sprite A or B, which is how a door mark switches from "locked" to
 * "opened". Nothing here is invented: reading the same tables and the same flags
 * means the minimap gains a mark at the same instant the paper map does. */
extern s_800AEDBC D_800AEDBC[];

/* Set while polling func_80067914 for coords only (suppresses its arrow draw). */
int g_PcMapQueryOnly = 0;

/* Written by func_80067914 during that query: Harry's heading expressed in
 * PAPER-MAP space rather than world space. Not every area's paper map is drawn on
 * the usual (map-X = +world X, map-Y = -world Z) axes — the fog sewers put map-X
 * on world -Z, the alt sewer inverts both axes, and a handful of interior chunks
 * elsewhere are laid out rotated. func_80067914 already compensates per area for
 * its own arrow; this hands the same corrected angle to the minimap. */
q3_12 g_PcMapQueryAngle = 0;

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
/* Second slot, same free range, for the marking sprite ATLAS (MR_*.TIM). */
#define MM_MARK_SLOT (HIRES_POOL_SLOT_MAX - 2)
#define MM_MARK_CLUT                                                           \
    (u16)((((HIRES_POOL_CLUT_ROW_BASE + (MM_MARK_SLOT / 64) * HIRES_POOL_MAX_ROWS)) << 6) | \
          (MM_MARK_SLOT % 64))
/* Prims reserved for markings. Only the ones inside the zoom window get one, and
 * the densest area (Alt School 1F) has 40 in the WHOLE 320x480 map, so a window
 * covering ~1/9 of it cannot plausibly fill this. Overflow just stops drawing. */
#define MM_MARK_MAX 32
/* UV denominator: the shader maps 0..native over the GL texture, so any decoded
 * map resolution works and UVs stay in the u8 prim range. */
#define MM_NATIVE 256
#define MM_UV_MAX (MM_NATIVE - 1)
#define MM_ZOOM   3      /* show ~1/3 of the map around Harry */

#define MM_SEG     20   /* triangle-fan segments approximating the circle */
#define MM_OUTLINE 2    /* black outline thickness, in the same 2D units */

/* Half-extents of Harry's map-cell coords (func_80067914 query: mapCoordIdxX/Z),
 * used to place him on the paper-map image. X spans the full 320-wide texture
 * (mapCoordIdxX = -160..+160). Z is HALF the texture height: the query returns
 * mapCoordIdxZ in 240-tall space but the map texture spans Y=-240..+240 (480),
 * so the arrow gets a *2 Y multiplier (see func_80067914's sp10[1]*2). Using
 * MM_HALF_Z=240 here omitted that doubling → Harry tracked at HALF speed in Z
 * (he read too far NORTH as he walked south). 120 = the real ±120 Z range. */
#define MM_HALF_X 160
#define MM_HALF_Z 120

/* Paper-map TIM pixel dimensions. The image is 320x480: the map screen runs
 * interlaced, so one mapCoordIdxZ step is TWO image rows while one
 * mapCoordIdxX step is one column. Marking placements live in that same image,
 * at (x*2 + 320/2, y*2 + 480/2) — proved by compositing every OldTown marking
 * onto MP_0TOWN.TIM at those coordinates: the X marks land exactly on the
 * blocked street ends and the hospital marks exactly on their doorways. */
#define MM_MAP_W 320
#define MM_MAP_H 480

/* Entries in g_PaperMapFileIdxs / g_PaperMapMarkingFileIdxs / D_800AEDBC.
 * PaperMapIdx_24 exists as an enum value but no table has a row for it. */
#define MM_PAPER_MAP_COUNT 24

static int s_mapIdxLoaded = -1;
static unsigned char* s_rawBuf = NULL;
static unsigned int   s_rawSize = 0;
static int s_pendKind    = 0;   /* in-flight read: 0 none, 1 map image, 2 markings */
static int s_idleFrames  = 0;
static int s_mapReady    = 0;   /* pool slot holds this area's map */
static int s_markFileLoaded = NO_VALUE; /* marking TIM idx read into MM_MARK_SLOT */
static int s_markReady      = 0;

static int mm_clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static int mm_isqrt(int v)
{
    int k = 0;
    while ((k + 1) * (k + 1) <= v) k++;
    return k;
}

/* 1 = read started, 0 = transient failure (retry a later frame), -1 = no file. */
static int mm_start_read(e_FsFile f, int kind)
{
    s32 size = Fs_GetFileSize(f);

    if (size <= 0) return -1;
    s_rawBuf = (unsigned char*)malloc((size_t)size + 2048);
    if (s_rawBuf == NULL) return 0;

    s_rawSize = (unsigned int)size;
    Fs_QueueStartRead(f, s_rawBuf);
    s_pendKind = kind;
    SH_DBG("[MINIMAP] queued %s read: file=%d size=%d",
           (kind == 1) ? "map" : "marking", (int)f, (int)size);
    return 1;
}

/* Deferred, non-blocking load of the area's paper-map TIM and its marking sprite
 * atlas into their pool slots. One read at a time, map first. */
static void mm_map_load_tick(int idx)
{
    int markFile;

    if (s_pendKind != 0)
    {
        unsigned char* rgba = NULL;
        int            w = 0, h = 0;
        int            kind = s_pendKind;
        int            ok   = 0;

        if (Fs_QueueGetLength() > 0) return;   /* poll only — never pump */
        s_pendKind = 0;

        if (HiresOverride_DecodeToRGBA(s_rawBuf, s_rawSize, &rgba, &w, &h) == 0 && rgba != NULL)
        {
            if (kind == 1)
            {
                ok = s_mapReady = (HiresOverride_PoolSlotRegisterRGBA(MM_POOL_SLOT, 0, rgba, w, h,
                                                                      MM_NATIVE, MM_NATIVE) == 0);
            }
            else
            {
                /* The marking atlas registers at its OWN pixel size, so prim UVs
                 * are atlas texels directly and the override shader's footprint
                 * clamp collapses to point sampling — sprite cells sit 1px apart
                 * in the atlas and must not bleed into each other. */
                ok = s_markReady = (HiresOverride_PoolSlotRegisterRGBA(MM_MARK_SLOT, 0, rgba, w, h,
                                                                       w, h) == 0);
            }
            free(rgba);
        }
        free(s_rawBuf); s_rawBuf = NULL;
        SH_DBG("[MINIMAP] %s %dx%d -> slot %d %s", (kind == 1) ? "map" : "markings", w, h,
               (kind == 1) ? MM_POOL_SLOT : MM_MARK_SLOT, ok ? "registered" : "FAILED");
        return;
    }

    if (idx < 0 || idx >= MM_PAPER_MAP_COUNT) return;
    markFile = g_PaperMapMarkingFileIdxs[idx];

    if (idx == s_mapIdxLoaded && (markFile == NO_VALUE || markFile == s_markFileLoaded)) return;

    /* Only start once the queue has been continuously idle: during an area load it
     * dips to empty between the game's own reads, and slipping a read in there
     * corrupted the load. The map is cosmetic; it can appear a little late. */
    if (Fs_QueueGetLength() > 0) { s_idleFrames = 0; return; }
    if (++s_idleFrames < 180) return;

    if (idx != s_mapIdxLoaded)
    {
        int rc = mm_start_read((e_FsFile)(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[idx]), 1);
        if (rc == 0) return;                   /* out of memory — retry later */
        s_mapIdxLoaded = idx;
        s_mapReady     = 0;
        return;
    }

    /* Several paper maps share one marking atlas (all four hospital floors use
     * MR_HSP), so key the cache on the FILE index, exactly as the map screen's
     * own activeMarkingFileIdx check does. */
    if (mm_start_read((e_FsFile)(FILE_TIM_MR_0TOWN_TIM + markFile), 2) != 0)
    {
        s_markFileLoaded = markFile;
        s_markReady      = 0;
    }
}

/* The panel and the map window it is currently sampling, so markings can be
 * placed in exactly the same space as Harry's arrow. */
typedef struct
{
    int x0, y0, x1, y1; /* panel rect, 2D screen units */
    int cx, cy, R;      /* disc centre/radius (round shape) */
    int round;
    int u0, v0, u1, v1; /* map-image window currently sampled, in UV units */
    int size;           /* panel edge length */
    int lum, semi;
} s_MmView;

static POLY_FT4 s_markPrim[2][MM_MARK_MAX];

/* Build the visible paper-map markings as textured prims over the map image.
 * Mirrors func_80068E0C's table walk and flag test exactly; only the output
 * transform differs (that function draws at full-map scale, this one inside the
 * minimap's zoom window). Returns the prim count. */
static int mm_markers_build(int buf, const s_MmView* vw)
{
    const s_800AEDBC* tbl;
    int idx = (int)g_SavegamePtr->paperMapIdx;
    int uw  = vw->u1 - vw->u0;
    int vh  = vw->v1 - vw->v0;
    int n, i, j, count = 0;

    if (!s_markReady || uw <= 0 || vh <= 0) return 0;
    if (idx < 0 || idx >= MM_PAPER_MAP_COUNT) return 0;
    /* Atlas belonging to a DIFFERENT area is still resident (its read has not
     * landed yet): drawing this area's placements with it would show the wrong
     * symbols, so wait a frame or two instead. */
    if (g_PaperMapMarkingFileIdxs[idx] == NO_VALUE ||
        g_PaperMapMarkingFileIdxs[idx] != s_markFileLoaded) return 0;

    tbl = &D_800AEDBC[idx];
    if (tbl->ptr_0 == NULL || tbl->ptr_4 == NULL) return 0;

    n = (tbl->field_A - tbl->field_8) >> 1;

    for (i = 0; i < n && count < MM_MARK_MAX; i++)
    {
        s_800AE8A0_4 place = *(s_800AE8A0_4*)&tbl->ptr_4[i];

        for (j = 0; j < 2 && count < MM_MARK_MAX; j++)
        {
            s_800AE8A0_0 spr;
            POLY_FT4*    p;
            int spriteIdx = j ? place.field_3 : place.field_2;
            int col0, row0, sx0, sy0, sx1, sy1;
            int bx0, bx1, by0, by1, kx0, kx1, ky0, ky1;
            int au0, av0, au1, av1, nu0, nv0, nu1, nv1;

            if (spriteIdx == 0) continue;                                    /* no symbol for this half */
            if (!Savegame_EventFlagGet(tbl->field_8 + (i * 2) + j + 1)) continue; /* not learned yet */

            spr = *(s_800AE8A0_0*)&tbl->ptr_0[spriteIdx];

            /* Placement -> paper-map image pixels (see MM_MAP_W/H above), then
             * -> panel units. Kept as one division per axis so the UV window's
             * ~0.86-units-per-UV quantisation never rounds a mark off its
             * street corner. */
            col0 = (place.field_0 * 2) + (MM_MAP_W / 2);
            row0 = (place.field_1 * 2) + (MM_MAP_H / 2);

            #define MM_COL2X(c) (vw->x0 + ((((c) * MM_UV_MAX) - (vw->u0 * MM_MAP_W)) * vw->size) / (MM_MAP_W * uw))
            #define MM_ROW2Y(r) (vw->y0 + ((((r) * MM_UV_MAX) - (vw->v0 * MM_MAP_H)) * vw->size) / (MM_MAP_H * vh))
            sx0 = MM_COL2X(col0);
            sx1 = MM_COL2X(col0 + spr.field_2);
            sy0 = MM_ROW2Y(row0);
            sy1 = MM_ROW2Y(row0 + spr.field_3);
            #undef MM_COL2X
            #undef MM_ROW2Y

            if (sx1 <= sx0 || sy1 <= sy0) continue;   /* collapsed by rounding */

            /* Clip to the panel. Square: the panel box. Disc: the box, then the
             * circle's chord at whichever clipped edge row is farthest from the
             * centre — conservative, so a mark never spills onto the ring. */
            bx0 = vw->x0; bx1 = vw->x1;
            by0 = vw->y0; by1 = vw->y1;

            ky0 = (sy0 < by0) ? by0 : sy0;
            ky1 = (sy1 > by1) ? by1 : sy1;
            if (ky1 <= ky0) continue;

            if (vw->round)
            {
                int d0 = ky0 - vw->cy, d1 = ky1 - vw->cy, dy, hw;
                if (d0 < 0) d0 = -d0;
                if (d1 < 0) d1 = -d1;
                dy = (d0 > d1) ? d0 : d1;
                if (dy >= vw->R) continue;
                hw  = mm_isqrt((vw->R * vw->R) - (dy * dy));
                bx0 = vw->cx - hw;
                bx1 = vw->cx + hw;
            }

            kx0 = (sx0 < bx0) ? bx0 : sx0;
            kx1 = (sx1 > bx1) ? bx1 : sx1;
            if (kx1 <= kx0) continue;

            /* Atlas cell, cropped in step with the screen rect. */
            au0 = spr.field_0;
            av0 = spr.field_1;
            au1 = au0 + spr.field_2;
            av1 = av0 + spr.field_3;
            nu0 = au0 + (((kx0 - sx0) * (au1 - au0)) / (sx1 - sx0));
            nu1 = au1 - (((sx1 - kx1) * (au1 - au0)) / (sx1 - sx0));
            nv0 = av0 + (((ky0 - sy0) * (av1 - av0)) / (sy1 - sy0));
            nv1 = av1 - (((sy1 - ky1) * (av1 - av0)) / (sy1 - sy0));

            p = &s_markPrim[buf][count++];
            setPolyFT4(p);
            setRGB0(p, (u8)vw->lum, (u8)vw->lum, (u8)vw->lum); /* == func_80068E0C's shade 128 at full opacity */
            if (vw->semi) setSemiTrans(p, 1);
            setXY4(p, kx0, ky0, kx1, ky0, kx0, ky1, kx1, ky1);
            p->tpage = 0;
            p->clut  = MM_MARK_CLUT;
            p->u0 = (u8)nu0; p->v0 = (u8)nv0;
            p->u1 = (u8)nu1; p->v1 = (u8)nv0;
            p->u2 = (u8)nu0; p->v2 = (u8)nv1;
            p->u3 = (u8)nu1; p->v3 = (u8)nv1;
        }
    }

    return count;
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
    int       halfW = SCREEN_WIDTH / 2, mmSize = MM_SIZE;
    int       u0 = 0, v0 = 0, u1 = MM_UV_MAX, v1 = MM_UV_MAX;
    int       markCount = 0;
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

    /* --- corner placement ---
     * Vertical is always 4:3 (y -120..+120). Horizontally the port renders 2D at
     * a Hor+ widescreen ortho, so on a wide window the true left/right screen
     * edge sits past ±160 — widen `halfW` to reach it (same as dbg_overlay's
     * collvis: halfW = 160 * winAspect/psxAspect). On 4:3 keep 160 and shrink
     * the panel a touch. */
    {
        float psxA = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
        float winA = (g_PcConfig.windowWidth > 0 && g_PcConfig.windowHeight > 0)
                   ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight : psxA;
        if (winA > psxA + 0.01f) { halfW = (int)(160.0f * (winA / psxA) + 0.5f); }
        else                     { mmSize = MM_SIZE - 8; }
    }
    switch (g_PcConfig.minimapCorner)
    {
        case 1:  x0 =  halfW - MM_MARGIN - mmSize;
                 y0 = -(SCREEN_HEIGHT / 2) + MM_MARGIN; break;
        case 2:  x0 = -halfW + MM_MARGIN;
                 y0 =  (SCREEN_HEIGHT / 2) - MM_MARGIN - mmSize; break;
        case 3:  x0 =  halfW - MM_MARGIN - mmSize;
                 y0 =  (SCREEN_HEIGHT / 2) - MM_MARGIN - mmSize; break;
        default: x0 = -halfW + MM_MARGIN;
                 y0 = -(SCREEN_HEIGHT / 2) + MM_MARGIN; break;
    }
    x1 = x0 + mmSize; y1 = y0 + mmSize;
    cx = x0 + mmSize / 2; cy = y0 + mmSize / 2; R = mmSize / 2;

    /* --- Harry's cell on the paper map (query mode: no arrow drawn) ---
     * Seed the heading with the world angle so unmapped areas (where the query
     * bails before exporting one) still get a sane arrow. */
    g_PcMapQueryAngle = g_SysWork.playerWork.player.rotation.vy;
    g_PcMapQueryOnly  = 1;
    packed = func_80067914((s32)g_SavegamePtr->paperMapIdx, 0, 0, (u16)Q12(1.0f));
    g_PcMapQueryOnly  = 0;

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

        px = x0 + ((cu - u0) * mmSize) / (u1 - u0);
        py = y0 + ((cv - v0) * mmSize) / (v1 - v0);
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
        setWH(&s_sqOut[buf], mmSize + MM_OUTLINE * 2, mmSize + MM_OUTLINE * 2);
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
            setWH(&s_sqFill[buf], mmSize, mmSize);
            setXY0(&s_sqFill[buf], x0, y0);
        }
    }

    /* --- marker: small triangle along Harry's heading (+Y is down => north is -Y).
     * Heading is the PAPER-MAP-space angle from the query, not rotation.vy: see
     * g_PcMapQueryAngle above. --- */
    {
        static const s32 lx[3] = {  0, -3,  3 };
        static const s32 ly[3] = { -5,  3,  3 };
        q3_12 ang = g_PcMapQueryAngle;
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

    /* --- paper-map markings, over the map image and under Harry's arrow --- */
    if (haveMap)
    {
        s_MmView vw;
        vw.x0 = x0; vw.y0 = y0; vw.x1 = x1; vw.y1 = y1;
        vw.cx = cx; vw.cy = cy; vw.R = R; vw.round = round;
        vw.u0 = u0; vw.v0 = v0; vw.u1 = u1; vw.v1 = v1;
        vw.size = mmSize; vw.lum = lum; vw.semi = semi;
        markCount = mm_markers_build(buf, &vw);
    }

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0)); /* abr=0 = 50% blend */

    /* AddPrim pushes to the head: add front-to-back, tpage last. */
    AddPrim(ot, &s_mark[buf]);
    for (i = 0; i < markCount; i++)
        AddPrim(ot, &s_markPrim[buf][i]);
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
