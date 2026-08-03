/*
 * pgxp_xbox.c - texture-only PGXP shadow table for the NV2A port.
 *
 * PGXP (perspective-correct texturing) on real PSX hardware is impossible; the
 * GPU interpolates texture coordinates affine (screen-linear), so textures swim
 * and warp on near/steep polygons. This is the DuckStation-style shadow-memory
 * model, ported from the PC port's PsyX_GPU.cpp (which the Xbox build does not
 * compile - it is GL-coupled). The software GTE we DO compile (PsyX_GTE.cpp)
 * already computes, for every projected vertex, the full-precision float screen
 * X/Y and the real per-vertex view depth W (pgxpW), and hands them to store
 * hooks - those hooks used to dead-end in no-op stubs (xbox_pcfeature_stubs.c).
 * This file is the real table behind them plus the draw-time resolve.
 *
 * Data path (ALL of it gated on g_PsxUsePgxp; the OFF path never allocates the
 * table and every entry point is a no-op, so PGXP off is byte-identical to the
 * legacy affine renderer):
 *   1. GTE store  (PsyX_GTE.cpp PGXP_StoreAddr, gate `if (g_PsxUsePgxp)`) ->
 *      Shadow_Store(&screenXy, fx, fy, w, word)          [keyed by native addr]
 *   2. drawer copy (shared bodyprog_80055028.c / water.c SH_PGXP_PROP macros,
 *      called unconditionally) -> Shadow_Copy(&poly->xN, &screenXy)   [propagate]
 *   3. GPU draw   (gpu_xbox.c ProcessPoly) -> Pgxp_GetPreciseVertex(&poly->xN,
 *      word) -> precise sub-pixel X/Y + W, fed to the NV2A so its fixed-function
 *      pipe does HW perspective-correct texture interpolation (Star Fox proves a
 *      per-vertex W works with no pixel-shader change).
 *
 * The NV2A depth test is OFF and the OT is walked painter's-order far->near
 * (exactly like PSX), so this is texture-only PGXP: sub-pixel positions kill
 * vertex wobble and the per-vertex W makes textures perspective-correct. The
 * whole PC PGXP depth-buffer / z-fighting saga is irrelevant here.
 *
 * Table sizing for a 64 MB console: the PC port used 2^21 entries (~48 MB on a
 * 32-bit build) because it draws the WHOLE map resident. Xbox streams like PSX,
 * so a frame holds far fewer live vertex words (~6-10k: one per drawn prim-field
 * plus the small reused GTE scratch). 2^17 (131072) entries * 24 B = ~3 MB gives
 * a <0.1 load factor with the 16-probe open addressing, and is allocated LAZILY
 * on the first store (only ever when PGXP is actually enabled), so it costs zero
 * bytes in the default OFF configuration.
 */
#include <stdlib.h>   /* calloc */
#include <stdint.h>   /* uintptr_t */

#include "sh_log.h"

/* Config-driven master flag (defined in pgxp_stub.c, set from g_PcConfig.usePgxp
 * at boot by XboxConfig_ApplyOverrides). Default 0. */
extern int g_PsxUsePgxp;

/* Frame generation: bumped once per frame (Pgxp_FrameAdvance, from
 * GpuNv2a_FrameEnd) so a shadow entry left at a packet address a later frame
 * reuses is rejected on lookup (gen mismatch) and its slot is treated as free.
 * The bump is also the table's "clear" - there is no per-frame memset. Starts at
 * 1 so freshly calloc'd (gen==0) slots read as empty/stale. */
static unsigned s_pgxpGen = 1;

/* Shadow entry: the precise projection of the vertex word living at `key`.
 * value = the packed integer (s16 x | s16 y<<16) that word held when stored; a
 * draw that reused the address with a different word falls through to affine. */
typedef struct {
    uintptr_t key;
    unsigned  gen;
    unsigned  value;
    float     x, y, w;
} ShadowEntry;

#define SHADOW_BITS 17
#define SHADOW_SIZE (1u << SHADOW_BITS)
#define SHADOW_MASK (SHADOW_SIZE - 1u)

static ShadowEntry* s_shadow = 0;   /* lazily allocated on first store */

/* Allocate the table on first use (only reached when g_PsxUsePgxp). On OOM the
 * pointer stays 0 and every op degrades to affine - PGXP silently disables
 * itself rather than crashing on a 64 MB console. */
static void Pgxp_EnsureTable(void)
{
    if (s_shadow)
        return;
    s_shadow = (ShadowEntry*)calloc(SHADOW_SIZE, sizeof(ShadowEntry));
    if (s_shadow)
        SH_DBG("[PGXP] shadow table %u entries, %u KB (texture-only, ON)",
               SHADOW_SIZE, (unsigned)(SHADOW_SIZE * sizeof(ShadowEntry) / 1024));
    else
        SH_DBG("[PGXP] shadow table alloc FAILED - PGXP affine-only this session");
}

static inline unsigned ShadowHash(uintptr_t k)
{
    return (unsigned)((k >> 2) * 2654435761u) & SHADOW_MASK;
}

static void Shadow_Put(void* addr, float x, float y, float w, unsigned value)
{
    uintptr_t    k = (uintptr_t)addr;
    unsigned     s = ShadowHash(k);
    ShadowEntry* e;
    int          i;

    for (i = 0; i < 16; i++) {
        e = &s_shadow[(s + i) & SHADOW_MASK];
        if (e->key == k || e->key == 0 || e->gen != s_pgxpGen) {
            e->key = k; e->gen = s_pgxpGen; e->value = value;
            e->x = x; e->y = y; e->w = w;
            return;
        }
    }
    /* Probe window exhausted (should never happen at this load factor): overwrite
     * the base slot rather than grow. Worst case the loser draws affine. */
    e = &s_shadow[s];
    e->key = k; e->gen = s_pgxpGen; e->value = value;
    e->x = x; e->y = y; e->w = w;
}

static const ShadowEntry* Shadow_Get(const void* addr)
{
    uintptr_t k = (uintptr_t)addr;
    unsigned  s = ShadowHash(k);
    int       i;

    for (i = 0; i < 16; i++) {
        const ShadowEntry* e = &s_shadow[(s + i) & SHADOW_MASK];
        if (e->key == k)
            return (e->gen == s_pgxpGen) ? e : 0;
        if (e->key == 0)
            return 0;
    }
    return 0;
}

/* ---- hooks called from the reused software GTE / shared drawer code -------- */

/* GTE store (PsyX_GTE.cpp PGXP_StoreAddr, only when g_PsxUsePgxp): record the
 * precise projection of the vertex word just written to `addr`. */
void Shadow_Store(void* addr, float x, float y, float w, unsigned value)
{
    Pgxp_EnsureTable();
    if (s_shadow)
        Shadow_Put(addr, x, y, w, value);
}

/* Drawer copy (shared bodyprog_80055028.c / water.c SH_PGXP_PROP macros): the
 * game just did *dst = *src (a vertex word moving from a GTE scratch slot into a
 * prim field). Propagate the shadow so the GPU can resolve the prim-field
 * address. Called UNCONDITIONALLY from shared code, so it must gate itself:
 * byte-identical no-op when PGXP is off (matches the PC Shadow_Copy, which
 * reduces to an immediate return when g_PsxUsePgxp is 0). */
void Shadow_Copy(void* dst, const void* src)
{
    const ShadowEntry* e;

    if (!g_PsxUsePgxp)
        return;
    Pgxp_EnsureTable();
    if (!s_shadow)
        return;
    e = Shadow_Get(src);
    /* Value-validate the SOURCE before propagating (PC Vs_Get lesson): a stale
     * same-address entry from an earlier emitter's scratch reuse would otherwise
     * propagate a wrong precise position onto this prim's field. */
    if (e && e->value == *(const unsigned*)src)
        Shadow_Put(dst, e->x, e->y, e->w, *(const unsigned*)dst);
}

/* View-space store: the GTE calls this whenever g_PsxUsePgxp (it feeds the PC
 * per-pixel flashlight AND the PC near-plane clipper). The NV2A texture-only
 * path has neither - no view-space shadow is ever read back here - so it is a
 * pure no-op that exists only to satisfy the link. */
void VShadow_Store(void* addr, float vx, float vy, float vz)
{
    (void)addr; (void)vx; (void)vy; (void)vz;
}

/* ---- draw-time resolve (gpu_xbox.c ProcessPoly) --------------------------- */

/* Return 1 + the precise RAW screen X/Y (pre-draw-env-offset, i.e. the same
 * space as the integer coord stored in the prim, so the caller's PutVert
 * transform applies unchanged) and the per-vertex W, when the vertex at `addr`
 * is tracked, its word still matches `value`, and it is in front of the near
 * plane (W>0). Otherwise return 0 and the caller keeps its affine vertex.
 * `value` must be read the same way Shadow_Copy stored it: *(u32*)&prim->xN. */
int Pgxp_GetPreciseVertex(const void* addr, unsigned value, float* ox, float* oy, float* ow)
{
    const ShadowEntry* e;

    if (!s_shadow)
        return 0;
    e = Shadow_Get(addr);
    if (e && e->value == value && e->w > 0.0f) {
        *ox = e->x;
        *oy = e->y;
        *ow = e->w;
        return 1;
    }
    return 0;
}

/* Advance the frame generation once per presented frame (GpuNv2a_FrameEnd). By
 * then every DrawOTag of the frame has resolved (all Pgxp_GetPreciseVertex reads
 * are done) and the next frame's GTE stores have not begun, so this is the clean
 * boundary at which last frame's entries become stale. */
void Pgxp_FrameAdvance(void)
{
    s_pgxpGen++;
}
