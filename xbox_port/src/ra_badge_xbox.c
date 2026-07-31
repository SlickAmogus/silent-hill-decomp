/*
 * ra_badge_xbox.c - RetroAchievements unlock BADGE image for the Original Xbox
 * port. Complements the text toast (dbg_overlay_xbox.c) + unlock chime
 * (ra_xbox.c): on an achievement unlock we fetch the badge PNG over plain HTTP,
 * decode it (stb_image, see stb_image_impl.c), upload it as a small NV2A texture
 * and draw it as a direct textured quad ON TOP of the frame, next to the toast.
 *
 * Two entry points, both main-thread (rc_client is single-threaded on Xbox):
 *   int  RaBadge_Fetch(const char* badge_name)  - one-shot, BLOCKING. Called from
 *        ra_xbox.c's ACHIEVEMENT_TRIGGERED handler (settled gameplay, rare), so
 *        the brief HTTP hitch on unlock is acceptable. Decodes + uploads + arms
 *        the display timer. Any failure (RA off, no net, bad PNG, oversize) is a
 *        silent skip - the toast text + chime still fire.
 *   void RaBadge_RenderDirect(void)             - per-present, from VSync() in
 *        psx_libgpu_xbox.c right before GpuNv2a_FrameEnd(). Emits the badge quad
 *        into the frame batch AFTER the OT walk (world OT0 + toast OT2 text), so
 *        it composites on top; a cheap no-op while no badge is active/expired.
 *
 * The badge is a direct NV2A quad (like fmv_xbox.c's movie frame), NOT an OT
 * primitive, because the toast text is already OT-based and the badge must land
 * on top at present time. Vertex/texture path is copied from fmv_xbox.c
 * (Fmv_DrawFrame / Fmv_UploadRgb): screen-space ShVertex quad, col=1 (pixel
 * shader = tex*col), pos[3]=1 (affine W), A8R8G8B8 texels, blend off.
 *
 * Memory: the GPU texture buffer (contiguous write-combined) is allocated ONCE
 * (GpuNv2a_AllocTexMem has no free on nxdk) at a fixed 128x128 cap and reused for
 * every unlock - a badge is 64x64. The HTTP body and the stb pixel buffer are
 * both freed right after the upload. Nothing here is per-frame allocated.
 *
 * Xbox-only (xbox_port/src): no #ifdef needed. It does not depend on
 * SH_RETROACHIEVEMENTS - RaBadge_Fetch is simply never called when RA is off, and
 * RaBadge_RenderDirect no-ops with no badge armed.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>   /* KeTickCount: kernel ms-since-boot clock */

#include "gpu_nv2a.h"            /* ShVertex + GpuNv2a_* (clean header, no pbkit) */
#include "net_xbox.h"           /* Net_XboxHttpRequest (blocking plain HTTP GET) */
#include "sh_log.h"

/* stb_image (real decoder in stb_image_impl.c). Forward-declared here rather than
 * including stb_image.h so this TU stays dependency-light; signatures match
 * stb_image.h v2.30 exactly. */
typedef unsigned char stbi_uc;
extern stbi_uc* stbi_load_from_memory(stbi_uc const* buffer, int len,
                                      int* x, int* y, int* channels_in_file,
                                      int desired_channels);
extern void     stbi_image_free(void* retval_from_stbi_load);

/* Content-rect geometry from gpu_nv2a.c (not in the header). The badge is placed
 * against a 640x480 authoring base so it lands next to the toast at any output
 * resolution (identity at 640x480, scaled up when pillarboxed at 720p). */
extern int g_Nv2aContentX, g_Nv2aContentW, g_Nv2aContentH;

/* Card layout owns the badge position (640x480 space); defined in
 * dbg_overlay_xbox.c so the image tracks the centered, bottom-pinned text. */
extern float g_RaBadgeX640, g_RaBadgeY640;

/* Placement + sizing, in the 640x480 UI base (top-left, beside the toast text). */
#define BADGE_POS_X     16.0f
#define BADGE_POS_Y     40.0f
#define BADGE_DRAW_PX   56.0f

/* Decode/upload cap. Badges are 64x64; 128 leaves headroom without a big buffer
 * (128*128*4 = 64 KB of contiguous WC memory, allocated once). */
#define BADGE_MAX_DIM   128

/* How long the badge stays on screen after an unlock (ms). Matched to the unlock
 * CARD lifetime (dbg_overlay_xbox.c UNLOCK_LIFE_MS = 5300) so the image and its
 * text fade out together. */
#define BADGE_SHOW_MS   5300u

static uint32_t* s_badgeTex;        /* reusable A8R8G8B8 GPU texture (128x128 cap) */
static int       s_badgeW;          /* current badge dims (texels)                */
static int       s_badgeH;
static uint32_t  s_badgeExpiryMs;   /* KeTickCount at which the badge expires      */
static int       s_badgeActive;     /* a badge is armed (and not yet expired)      */

/* Wrap-safe "still showing?" test. */
static int RaBadge_Live(void)
{
    if (!s_badgeActive)
        return 0;
    if ((int32_t)((uint32_t)KeTickCount - s_badgeExpiryMs) >= 0) {
        s_badgeActive = 0;          /* expired: latch off (also frees the no-op path) */
        return 0;
    }
    return 1;
}

/* Fetch + decode + upload the badge for `badge_name` (rc_client achievement
 * badge_name, e.g. "12345"). BLOCKING one-shot; returns 0 on success, -1 on any
 * skip. Never crashes: every failure path frees what it owns and returns -1. */
int RaBadge_Fetch(const char* badge_name)
{
    char     url[128];
    char*    body = NULL;
    int      len  = 0;
    int      status;
    stbi_uc* px;
    int      w = 0, h = 0, comp = 0;
    int      i, n;

    if (!badge_name || !badge_name[0])
        return -1;

    /* Lazily claim the single reusable GPU texture buffer (contiguous WC; nxdk
     * has no free for it, so allocate ONCE and re-upload each unlock). */
    if (!s_badgeTex) {
        s_badgeTex = (uint32_t*)GpuNv2a_AllocTexMem(BADGE_MAX_DIM * BADGE_MAX_DIM * 4);
        if (!s_badgeTex) {
            SH_DBG("[RABADGE] tex alloc failed - badge image disabled");
            return -1;
        }
    }

    snprintf(url, sizeof(url),
             "http://media.retroachievements.org/Badge/%s.png", badge_name);

    /* post=NULL -> GET. *body is malloc'd (we free it); len is the BINARY length
     * (the PNG may contain NULs), exactly what stbi wants. */
    status = Net_XboxHttpRequest(url, NULL, &body, &len);
    if (status != 200 || !body || len <= 0) {
        SH_DBG("[RABADGE] fetch %s http=%d len=%d - skip", badge_name, status, len);
        if (body)
            free(body);
        return -1;
    }

    px = stbi_load_from_memory((const stbi_uc*)body, len, &w, &h, &comp, 4);
    free(body);
    if (!px) {
        SH_DBG("[RABADGE] png decode failed for %s", badge_name);
        return -1;
    }
    if (w <= 0 || h <= 0 || w > BADGE_MAX_DIM || h > BADGE_MAX_DIM) {
        SH_DBG("[RABADGE] badge %dx%d out of range - skip", w, h);
        stbi_image_free(px);
        return -1;
    }

    /* RGBA8 (stb) -> A8R8G8B8 (NV2A), alpha forced opaque so the render state's
     * alpha-test (GEQUAL 1) can never drop a texel; badges are solid squares.
     * sfence flushes the WC stores before the GPU DMA-reads them (fmv parity). */
    n = w * h;
    for (i = 0; i < n; i++) {
        uint32_t r = px[i * 4 + 0];
        uint32_t g = px[i * 4 + 1];
        uint32_t b = px[i * 4 + 2];
        s_badgeTex[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    stbi_image_free(px);

    s_badgeW        = w;
    s_badgeH        = h;
    s_badgeExpiryMs = (uint32_t)KeTickCount + BADGE_SHOW_MS;
    s_badgeActive   = 1;
    SH_DBG("[RABADGE] shown %s %dx%d", badge_name, w, h);
    return 0;
}

/* Draw the armed badge as a direct textured quad, top-left, on top of the frame.
 * Called from VSync() right before the present flush, so it lands over the OT
 * walk (world + toast text). No-op when nothing is armed. Crash-safe. */
void RaBadge_RenderDirect(void)
{
    ShVertex v[6];
    float    sx, sy, x0, y0, x1, y1;
    int      i;

    if (!RaBadge_Live())
        return;
    if (!s_badgeTex || s_badgeW <= 0 || s_badgeH <= 0)
        return;

    sx = (float)g_Nv2aContentW / 640.0f;
    sy = (float)g_Nv2aContentH / 480.0f;
    /* Position comes from the overlay's card layout (dbg_overlay_xbox.c) so the
     * image lines up with the centered, bottom-pinned text. */
    x0 = (float)g_Nv2aContentX + g_RaBadgeX640 * sx;
    y0 = g_RaBadgeY640 * sy;
    x1 = x0 + BADGE_DRAW_PX * sx;
    y1 = y0 + BADGE_DRAW_PX * sy;

    memset(v, 0, sizeof(v));
    for (i = 0; i < 6; i++) {
        v[i].col[0] = 1.0f;
        v[i].col[1] = 1.0f;
        v[i].col[2] = 1.0f;
        v[i].col[3] = 1.0f;
        v[i].pos[3] = 1.0f;   /* affine W (memset left it 0 -> div-by-0 quad) */
    }
    v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].tex[0] = 0.0f;             v[0].tex[1] = 0.0f;
    v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].tex[0] = (float)s_badgeW;  v[1].tex[1] = 0.0f;
    v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].tex[0] = 0.0f;             v[2].tex[1] = (float)s_badgeH;
    v[3] = v[1];
    v[4].pos[0] = x1; v[4].pos[1] = y1; v[4].tex[0] = (float)s_badgeW;  v[4].tex[1] = (float)s_badgeH;
    v[5] = v[2];

    /* Opaque, full content rect. FrameBegin re-establishes state next frame, so
     * leaving the badge texture bound here needs no cleanup (fmv parity). */
    GpuNv2a_SetBlendMode(0);
    GpuNv2a_SetScissor(0, 0, 0, 0);
    GpuNv2a_BindTexture(s_badgeTex, s_badgeW, s_badgeH);
    GpuNv2a_EmitTris(v, 6);
}
