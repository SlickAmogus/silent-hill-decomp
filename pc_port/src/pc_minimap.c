/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "pc_loose_files.h"
#include "tex_pack.h"
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
static int s_pendFileIdx = -1;  /* file the in-flight read is for (override lookup) */
static int s_idleFrames  = 0;
static int s_mapReady    = 0;   /* pool slot holds this area's map */

/* The paper-map index the minimap should use. Normally the savegame's own --
 * but the INTRO street (map0_s00 and friends) carries PaperMapIdx_OtherPlaces
 * because no paper map is ever obtainable there, even though its layout IS the
 * town's. When the minimap is set to always draw (minimap_require_map = 0),
 * probe the town paper maps and adopt whichever one's placement tables actually
 * resolve Harry's position: func_80067914 in query mode returns 0 for an index
 * that does not recognise the coordinates, so the right town map self-selects
 * with no per-area table. Cached until the probe stops resolving (area left).
 * With minimap_require_map = 1 nothing changes: OtherPlaces stays unmapped. */

static int s_otherPlacesIdx = -1;

/* Query wrapper: func_80067914's FIRST gate is
 *     if (g_SavegamePtr->paperMapIdx != paperMapIdx) return 0;
 * so probing a candidate index -- or querying a substituted one at draw time --
 * is rejected outright unless the savegame momentarily agrees. Override for
 * the duration of the one call and restore; same frame, same thread, nothing
 * else reads the field in between. */
static s32 mm_query_at(int idx)
{
    u8  prev = (u8)g_SavegamePtr->paperMapIdx;
    s32 packed;

    g_SavegamePtr->paperMapIdx = (u8)idx;
    packed = func_80067914((s32)idx, 0, 0, (u16)Q12(1.0f));
    g_SavegamePtr->paperMapIdx = prev;
    return packed;
}

static int mm_effective_map_idx(void)
{
    int idx = (int)g_SavegamePtr->paperMapIdx;

    if (idx != 0 /* PaperMapIdx_OtherPlaces */ || g_PcConfig.minimapRequireMap)
    {
        s_otherPlacesIdx = -1;
        return idx;
    }

    /* Save/restore rather than set/clear: this helper runs inside the draw
     * path's own query-only bracket (argument evaluation at the 607 call), and
     * clearing the flag there would flip the REAL query into draw mode. */
    {
    s32 prevQueryOnly = g_PcMapQueryOnly;

    g_PcMapQueryOnly = 1;

    if (s_otherPlacesIdx < 0 || mm_query_at(s_otherPlacesIdx) == 0)
    {
        int c;

        s_otherPlacesIdx = -1;
        /* 1..4 are the four town maps (OldTown, FogCentralTown,
         * AltCentralTown, ResortTown). */
        for (c = 1; c <= 4; c++)
        {
            if (mm_query_at(c) != 0)
            {
                s_otherPlacesIdx = c;
                break;
            }
        }
    }

    g_PcMapQueryOnly = prevQueryOnly;
    }

    return (s_otherPlacesIdx >= 0) ? s_otherPlacesIdx : idx;
}
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

/* Apply the panel's Q12 size factor to a length in 2D units. Rounded, and the
 * exact identity at 4096 (100%), so the default panel is untouched. */
static int mm_scaled(int v, s32 mkScale)
{
    return (int)((((s32)v * mkScale) + 2048) >> 12);
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
    s_pendKind    = kind;
    s_pendFileIdx = (int)f;
    SH_DBG("[MINIMAP] queued %s read: file=%d size=%d",
           (kind == 1) ? "map" : "marking", (int)f, (int)size);
    return 1;
}

/* Pixel dimensions of the disc TIM sitting in s_rawBuf. The marking atlas has to
 * install under its NATIVE size (see the call site), which varies per file. */
static int mm_disc_native(int* outW, int* outH)
{
    TIM_IMAGE tim;

    if (!OpenTIM((u_long*)s_rawBuf)) return 0;
    if (ReadTIM(&tim) == NULL || tim.prect == NULL) return 0;

    /* prect->w counts 16-bit VRAM cells; convert per bit depth. */
    switch ((int)(tim.mode & 0x7))
    {
        case 0:  *outW = (int)tim.prect->w * 4;       break;
        case 1:  *outW = (int)tim.prect->w * 2;       break;
        case 2:  *outW = (int)tim.prect->w;           break;
        case 3:  *outW = ((int)tim.prect->w * 2) / 3; break;
        default: return 0;
    }
    *outH = (int)tim.prect->h;
    return *outW > 0 && *outH > 0;
}

/* DuckStation texture pack: compose from the disc TIM exactly as the pool path
 * in Fs_QueuePostLoadTim does — same (pixels, palette, bpp) triple, so the same
 * pack entry matches — and install the composite. Paper maps and their marking
 * atlases are single-palette (both MP_ and MR_ TIMs ship one CLUT row), so row 0
 * is the whole picture. Returns 1 when a pack supplied the image. */
static int mm_pack_register(int slot, int nativeW, int nativeH, const char* what)
{
    TIM_IMAGE            tim;
    const unsigned char* canvas;
    int                  cw = 0, ch = 0, bpp = 0, clutW = 0;

    if (!TexPack_HasEntries() || HiresOverride_PackBudgetExceeded()) return 0;
    if (!OpenTIM((u_long*)s_rawBuf)) return 0;
    if (ReadTIM(&tim) == NULL || tim.prect == NULL || tim.paddr == NULL) return 0;

    /* tim.mode bits 0-2: 0=4bpp, 1=8bpp, 2=16bpp (24bpp has no pack grammar). */
    switch ((int)(tim.mode & 0x7))
    {
        case 0:  bpp = 4;  break;
        case 1:  bpp = 8;  break;
        case 2:  bpp = 16; break;
        default: return 0;
    }
    clutW = (tim.caddr != NULL && tim.crect != NULL) ? (int)tim.crect->w : 0;

    canvas = TexPack_Compose((const unsigned char*)tim.paddr,
                             (int)tim.prect->w, (int)tim.prect->h,
                             (const unsigned short*)tim.caddr, clutW, bpp, &cw, &ch);
    if (canvas != NULL)
    {
        /* canvas is owned by the compose cache — no free. */
        if (HiresOverride_PoolSlotRegisterRGBAKeyed(slot, 0, canvas, cw, ch,
                                                    nativeW, nativeH,
                                                    TexPack_LastComposeHash()) != 0) return 0;
        SH_DBG("[MINIMAP] %s from texture pack: %dx%d", what, cw, ch);
        return 1;
    }
    if (TexPack_LastComposeIsDds())
    {
        size_t               ddsSize = 0;
        const unsigned char* dds     = TexPack_LastComposeDds(&ddsSize);

        if (HiresOverride_PoolSlotRegisterDdsKeyed(slot, 0, dds, ddsSize,
                                                   nativeW, nativeH,
                                                   TexPack_LastComposeHash()) != 0) return 0;
        SH_DBG("[MINIMAP] %s from texture pack (BC7)", what);
        return 1;
    }
    return 0;
}

/* Install one of the area's paper-map textures, honouring the SAME HD
 * replacement the paper-map SCREEN shows. The screen gets its mod from
 * Fs_QueuePostLoadTim (loose .png/.dds override, else a DuckStation pack
 * composite); the minimap reads the TIM itself and never reaches that code, so
 * it rendered disc art no matter what was installed. Same priority order as the
 * post-load, resolved through the queue's own probe chain so the two cannot
 * drift, and falling back to the disc bytes whenever a mod is absent or
 * unusable. A byte-replaced loose TIM needs nothing here — those bytes already
 * arrived in s_rawBuf via the read. Registering from the loose file's raw bytes
 * also reuses PoolSlotRegister's format handling (PNG, BC7 .dds and oversized
 * TIM alike). nativeW/H is the UV denominator the caller's prims address the
 * slot in, NOT the replacement's own size — a mod of any resolution then maps
 * over the original. */
static int mm_image_register(int slot, int nativeW, int nativeH, const char* what)
{
    unsigned char* rgba = NULL;
    int            w = 0, h = 0, ok = 0;
    char           path[176];

    if (Pc_LooseImageOverridePath((s32)s_pendFileIdx, path, sizeof(path)))
    {
        long           lsz  = 0;
        unsigned char* lbuf = Pc_LooseSlurp(path, &lsz);

        if (lbuf != NULL)
        {
            ok = (HiresOverride_PoolSlotRegister(slot, lbuf, (unsigned int)lsz,
                                                 nativeW, nativeH) == 0);
            free(lbuf);
            if (ok)
            {
                SH_DBG("[MINIMAP] %s from loose override %s", what, path);
                return 1;
            }
            SH_DBG("[MINIMAP] loose override %s unusable — falling back to disc art", path);
        }
    }

    if (mm_pack_register(slot, nativeW, nativeH, what)) return 1;

    if (HiresOverride_DecodeToRGBA(s_rawBuf, s_rawSize, &rgba, &w, &h) == 0 && rgba != NULL)
    {
        ok = (HiresOverride_PoolSlotRegisterRGBA(slot, 0, rgba, w, h,
                                                 nativeW, nativeH) == 0);
        free(rgba);
        SH_DBG("[MINIMAP] %s %dx%d -> slot %d %s", what, w, h, slot,
               ok ? "registered" : "FAILED");
    }
    return ok;
}

/* Deferred, non-blocking load of the area's paper-map TIM and its marking sprite
 * atlas into their pool slots. One read at a time, map first. */
static void mm_map_load_tick(int idx)
{
    int markFile;

    if (s_pendKind != 0)
    {
        int kind = s_pendKind;

        if (Fs_QueueGetLength() > 0) return;   /* poll only — never pump */
        s_pendKind = 0;

        if (kind == 1)
        {
            s_mapReady = mm_image_register(MM_POOL_SLOT, MM_NATIVE, MM_NATIVE, "map image");
        }
        else
        {
            /* The marking atlas is addressed in its OWN texels — mm_markers_build
             * feeds the game's sprite table straight into prim UVs — so the UV
             * denominator is the DISC atlas size, whatever resolution a
             * replacement ships at. Unmodded the two match and the override
             * shader's footprint clamp collapses to point sampling; modded, the
             * clamp keeps each fragment inside its own native texel, which is
             * what stops sprite cells (1 native texel apart) bleeding into each
             * other under linear sampling. */
            int nw = 0, nh = 0;

            if (mm_disc_native(&nw, &nh))
            {
                s_markReady = mm_image_register(MM_MARK_SLOT, nw, nh, "markings");
            }
            else
            {
                SH_DBG("[MINIMAP] markings: unreadable TIM header — slot %d left as-is",
                       MM_MARK_SLOT);
            }
        }
        free(s_rawBuf); s_rawBuf = NULL;
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
    int idx = mm_effective_map_idx();
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
            /* The atlas art is cyan, not red. The map screen turns it red purely
             * with the subtractive blend in its tpage (0x47 -> abr 2, B-F): cyan
             * taken off the light paper leaves (255,0,0). Semi-transparency is
             * unconditional there (RECT_BLEND), so it is here too -- drawing
             * these with abr 0 is what made the markings come out blue.
             * Reduced opacity still fades correctly, via the lum scale below. */
            setSemiTrans(p, 1);
            setXY4(p, kx0, ky0, kx1, ky0, kx0, ky1, kx1, ky1);
            p->tpage = getTPage(0, 2, 0, 0);
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
    s32       mkScale = 4096;
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

    mm_map_load_tick(mm_effective_map_idx());

    buf   = g_ActiveBufferIdx;
    ot    = &g_OtTags0[buf][4];
    round = (g_PcConfig.minimap == 2); /* 1 = square, 2 = circle */

    /* --- corner placement ---
     * Vertical is always 4:3 (y -120..+120). Horizontally the port renders 2D at
     * a Hor+ widescreen ortho, so on a wide window the true left/right screen
     * edge sits past ±160 — widen `halfW` to reach it (same as dbg_overlay's
     * collvis: halfW = 160 * winAspect/psxAspect). On 4:3 keep 160 and shrink
     * the panel a touch. */
    {
        extern void PsyX_GetScreenSize(int* w, int* h);
        extern int  g_PcHorPlusEnabled;
        extern int  g_PcMenuPillarbox;
        extern int  g_PcWidescreenMode;

        float psxA = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
        float winA;
        int   sw = 0, sh = 0;
        /* The 2D layer only reaches past +-160 when it is genuinely widened. A
         * PILLARBOXED wide window still draws 4:3, so widening here put the
         * panel inside (or past) the black bar. Same predicate the inventory
         * mouse hit-test uses for the same question. */
        /* Widen to the true screen edge whenever the gameplay pass fills a wide
         * window: Hor+ (mode 1) AND stretch (mode 2) both do, so gate on
         * "not pillarbox" (mode != 0), not just mode == 2. The old `== 2` left
         * the panel at the 4:3 margin in the default 16:9 Hor+ mode and in
         * menus-only pillarboxing (world still Hor+). Pillarbox (mode 0) draws
         * 4:3 with bars, so it must NOT widen. The second clause covers the 2D
         * screens (g_PcHorPlusEnabled == 0), which the minimap never reaches
         * (it early-returns outside SysState_Gameplay) but is kept for parity. */
        int   stretched = (g_PcHorPlusEnabled && g_PcWidescreenMode != 0) ||
                          (!g_PcHorPlusEnabled && !g_PcMenuPillarbox);

        /* Ask for the REAL backbuffer: the config width/height this used to read
         * describe the windowed size and say nothing about the current
         * fullscreen or borderless resolution. */
        PsyX_GetScreenSize(&sw, &sh);
        winA = (sw > 0 && sh > 0) ? (float)sw / (float)sh : psxA;

        if (stretched && winA > psxA + 0.01f)
        {
            halfW = (int)(160.0f * (winA / psxA) + 0.5f);
        }
        else
        {
            mmSize = MM_SIZE - 8;
        }
    }

    /* Scale last, so it applies to whichever base the aspect branch picked.
     * mkScale carries the same factor in Q12 to the things drawn at a FIXED size
     * instead of being derived from mmSize — Harry's arrow and its edge clamp.
     * (The paper-map markings already scale: mm_markers_build sizes them through
     * vw->size, so they follow the panel on their own.) */
    {
        float sc = g_PcConfig.minimapScale;
        if (sc < MINIMAP_SCALE_MIN) sc = MINIMAP_SCALE_MIN;
        if (sc > MINIMAP_SCALE_MAX) sc = MINIMAP_SCALE_MAX;
        mmSize = (int)(((float)mmSize * sc) / 100.0f + 0.5f);
        if (mmSize < 8) mmSize = 8;
        mkScale = (s32)(((sc * 4096.0f) / 100.0f) + 0.5f);
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
    packed = mm_query_at(mm_effective_map_idx());
    g_PcMapQueryOnly  = 0;

    /* The paper map is only drawn once Harry has actually found it -- HAS_MAP is
     * the same savegame bit the map screen gates on. minimap_require_map=0
     * restores the old always-visible behaviour. */
    haveMap = (s_mapReady && packed != 0);

    if (haveMap && g_PcConfig.minimapRequireMap)
    {
        int mi = mm_effective_map_idx();
        if (mi < 0 || mi >= MM_PAPER_MAP_COUNT || !HAS_MAP(mi)) haveMap = 0;
    }

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

    /* Keep the arrow inside the panel (disc or box). The inset is the arrow's own
     * reach, so it shrinks with it — otherwise a small panel would hold Harry a
     * fixed distance off its edge and misreport him as further inside than he is. */
    if (round)
    {
        s32 dx = px - cx, dy = py - cy;
        s32 lim = R - mm_scaled(5, mkScale), d = dx * dx + dy * dy;
        if (d > lim * lim)
        {
            s32 k = 0;
            while ((k + 1) * (k + 1) <= d) k++;      /* integer sqrt */
            if (k > 0) { px = cx + (dx * lim) / k; py = cy + (dy * lim) / k; }
        }
    }
    else
    {
        int inset = mm_scaled(4, mkScale);
        px = mm_clamp(px, x0 + inset, x1 - inset);
        py = mm_clamp(py, y0 + inset, y1 - inset);
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
                setRGB0(&s_fill[buf][i], (u8)((46 * op) / 100), (u8)((46 * op) / 100), (u8)((49 * op) / 100));
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
            setRGB0(&s_sqFill[buf], (u8)((46 * op) / 100), (u8)((46 * op) / 100), (u8)((49 * op) / 100));
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
            /* Rotate, then apply the panel scale in the same step: the operand is
             * Q12 and mkScale is Q12, so >>24 lands back in 2D units. At 100%
             * mkScale is exactly 4096 and (v * 4096) >> 24 == v >> 12, i.e. the
             * default arrow is bit-identical to the unscaled one. Scaling the
             * ROTATED offsets rather than lx/ly keeps the triangle's proportions
             * intact and its pivot exactly on Harry. */
            vx[i] = px + (((lx[i] * ca - ly[i] * sa) * mkScale) >> 24);
            vy[i] = py + (((lx[i] * sa + ly[i] * ca) * mkScale) >> 24);
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
