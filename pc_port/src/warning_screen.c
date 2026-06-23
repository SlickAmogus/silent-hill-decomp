/* warning_screen.c — PC-port "violent and disturbing images" warning.
 *
 * Renders the 2ZANKO_E warning image full-screen with a fade-in, mirroring
 * the PSX main()'s boot screen. Implementation follows the same Screen_Init
 * + centered-coord-system pattern as GameState_KonamiLogo_Update so the
 * image actually lands centered in the visible 4:3 area; the previous
 * approach of pushing an explicit 320×240 disp/draw env without calling
 * Screen_Init left activeDispEnv/drawenv in a hybrid state with PsyCross
 * (gs_screen_w/h still at the static defaults from libgs_stub.c init)
 * and the image rendered shrunk into one quadrant.
 *
 * Skipped if skip_intros = 1 in config.cfg.
 */
#include "common.h"
#include "game.h"
#include <psyq/libetc.h>
#include <psyq/libapi.h>
#include <SDL_scancode.h>
#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "screens/b_konami/b_konami.h"
#include "sh_log.h"
#include "pc_config.h"

extern void PsyX_EndScene(void); /* forward decl — defined in PsyX_main.cpp */
extern const unsigned char* g_sdlKeyboardState;
extern int PsyX_Pad_SkipButtonHeld(void);

/* QOL: any confirm/start key or gamepad A/Start skips the warning. Read raw SDL
 * directly — the controller mapping isn't wired up this early in boot, so the game's
 * button flags are all 0 here. Warn_SwapAndDraw pumps SDL events via VSync/PsyX_EndScene,
 * so this state is refreshed every frame. */
static int Warn_SkipPressed(void)
{
    if (g_sdlKeyboardState != NULL &&
        (g_sdlKeyboardState[SDL_SCANCODE_RETURN]   ||
         g_sdlKeyboardState[SDL_SCANCODE_KP_ENTER] ||
         g_sdlKeyboardState[SDL_SCANCODE_RETURN2]  ||
         g_sdlKeyboardState[SDL_SCANCODE_SPACE]    ||
         g_sdlKeyboardState[SDL_SCANCODE_C]        ||
         g_sdlKeyboardState[SDL_SCANCODE_V]))
    {
        return 1;
    }

    return PsyX_Pad_SkipButtonHeld();
}

/* TIM lives at VRAM tpage row 0 columns 13/14/15, CLUT at (768, 480). */
static s_FsImageDesc s_WarnImg = {
    .tPage = { 1, 13 },
    .u     = 32,
    .v     = 0,
    .clutX = 768,
    .clutY = 480
};

/* Draw the 2ZANKO image stretched to fill the entire framebuffer.
 *
 * The image is 384 px wide × 224 px tall, stored across 3 tpages (13/14/15)
 * with 128 px of real data per tpage. SPRT renders 1:1 source-to-dest, so
 * SPRTs would only fill 384/640 ≈ 60% of the framebuffer width. Use
 * POLY_FT4 quads instead — they decouple vertex positions from UVs, so
 * we can sample the 128×224 source region of each tpage and render it
 * stretched to ~213×240 in framebuffer space, edge-to-edge across the
 * full 640×240 fb. With non-PGXP ortho mapping fb (0..disp.w) → NDC ±1,
 * fb 0..640 fills the full visible window width.
 *
 * After Screen_Init(SCREEN_WIDTH * 2, true) the drawenv ofs is at
 * (320, 224). Raw quad coords -320..+320 horizontally translate to
 * fb 0..640 (full screen width). Vertically, raw -120..+120 translates
 * to fb 104..344 — slightly off-center to land in the visible 0..240
 * area; the fade tile covers anything outside.
 */
static void Warn_DrawImage(void)
{
    /* The 2ZANKO_E image is 320 px wide total. s_WarnImg.u=32 is in 16-bit
     * VRAM units, placing the TIM at VRAM X = 32 + (13<<6) = 864. Tpage 13
     * base = 13*64 = 832 VRAM units = 13*128 = 1664 8bpp texels. The TIM
     * pixel data starts at VRAM X=864, which is 864-832=32 VRAM units =
     * 64 8bpp texels from tpage 13's base.
     *
     * Layout in VRAM (8-bit color, tpage width = 128 8bpp texels):
     *   tpage 13: u=0..63  = empty VRAM (before TIM data)
     *             u=64..127 = image px 0..63
     *   tpage 14: u=0..127  = image px 64..191
     *   tpage 15: u=0..127  = image px 192..319
     * Total visible image width = 64 + 128 + 128 = 320 source px.
     *
     * Quad widths are PROPORTIONAL to source-width per tpage so the stretch
     * factor is uniform across all three quads. Total stretched to fb 640:
     *   64/320 × 640 = 128  (quad 0 — narrowed because only 64 source px)
     *   128/320 × 640 = 256 (quad 1)
     *   128/320 × 640 = 256 (quad 2) */
    static const s16 s_quadX[3] = { -320, -192,  +64 };
    static const s16 s_quadW[3] = {  128,  256,  256 };
    /* Per-quad source UV start. Quad 0 skips the 64 px of empty VRAM
     * by sampling from u=64. Quads 1 and 2 start at u=0 (full tpage). */
    static const u8  s_quadU0[3] = { 64, 0, 0 };
    static const u8  s_quadU1[3] = { 128, 128, 128 };
    /* Vertical: cover fb 0..480 (matches the fade tile's
     * setWH(SCREEN_WIDTH*2, SCREEN_HEIGHT*2) at xy(-SCREEN_WIDTH,-SCREEN_HEIGHT)).
     * Screen_Init(640, true) puts gs_screen_h at 448 (interlaced) and
     * the GR_Ortho2D maps fb y 0..disp.h to NDC ±1, so to fill the
     * full window vertically the quad needs to reach disp.h=448 (and a
     * bit more is fine — clipping is forgiving). With ofs.y=224, raw
     * -240..+240 maps to fb -16..464. Image source is 224 rows; we
     * stretch 224 source rows to 480 fb rows (~2.14× vertical upscale). */
    static const s16 s_quadY    = -240;
    static const s16 s_quadH    = 480;
    s32 i;
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0xF];

    for (i = 0; i < 3; i++)
    {
        POLY_FT4*  poly = (POLY_FT4*)GsOUT_PACKET_P;
        s16 x0 = s_quadX[i];
        s16 x1 = (s16)(s_quadX[i] + s_quadW[i]);
        s16 y0 = s_quadY;
        s16 y1 = (s16)(s_quadY + s_quadH);
        u8  u0 = s_quadU0[i];
        u8  u1 = s_quadU1[i];

        addPrimFast(addr, poly, 9);
        setPolyFT4(poly);
        setRGB0(poly, 0x80, 0x80, 0x80);
        /* Vertex layout for POLY_FT4: 0=TL, 1=TR, 2=BL, 3=BR. */
        poly->x0 = x0; poly->y0 = y0;
        poly->x1 = x1; poly->y1 = y0;
        poly->x2 = x0; poly->y2 = y1;
        poly->x3 = x1; poly->y3 = y1;
        /* Sample each tpage's image data (skipping padding on quad 0). */
        setUV4(poly, u0, 0,  u1, 0,  u0, 224,  u1, 224);
        setClut(poly, s_WarnImg.clutX, s_WarnImg.clutY);
        /* POLY_FT4 reads its own tpage field (PsyX_GPU.cpp case 0xC sets
         * activeDrawEnv.tpage = poly->tpage), so a separate DR_TPAGE in
         * the OT chain wouldn't help — must be on the prim itself. */
        poly->tpage = getTPageN(1, 0, 13 + i, 0);

        GsOUT_PACKET_P = (PACKET*)((u8*)poly + sizeof(POLY_FT4));
    }
}

/* Subtractive-blend full-screen fade tile. Must render AFTER the image
 * quads (composite over them, subtracting `fade` from each pixel).
 *
 * Chain order with head-insertion: tile is added first, then DR_TPAGE.
 * That gives chain head-to-tail = [drMode, tile] which renders drMode
 * first (sets subtractive blend) and then the tile (drawn subtractively).
 * Matches the canonical Screen_FadeUpdate ordering. The earlier swapped
 * order [tile, drMode] left the tile drawn with the *previous* draw mode
 * (opaque white) before the DR_TPAGE took effect — that opaque white was
 * what the image's transparent texels (STP=1, the silhouette in the
 * middle of 2ZANKO_E) showed through as a white blob. */
static void Warn_DrawFadeTile(s32 fade)
{
    /* Last bucket (0xF) of OT2 — same as Konami fade tile. */
    GsOT_TAG* addr = &g_OtTags0[g_ActiveBufferIdx][0xF];
    TILE*     tile = (TILE*)GsOUT_PACKET_P;
    DR_TPAGE* tp;

    /* Tile primitive first. setTile/setSemiTrans/setRGB0/setWH/setXY0
     * configure the prim; AddPrim places it on the chain (head). */
    addPrimFast(addr, tile, 3);
    setTile(tile);
    setSemiTrans(tile, 1);
    setRGB0(tile, fade, fade, fade);
    setWH(tile, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
    setXY0(tile, -SCREEN_WIDTH, -SCREEN_HEIGHT);

    /* DR_TPAGE second — head-inserted so it ends up BEFORE the tile in
     * render order. Sets subtractive blend mode for the tile draw. */
    tp = (DR_TPAGE*)((u8*)tile + sizeof(TILE));
    setDrawTPage(tp, 0, 1, getTPageN(0, 2, 0, 0));
    AddPrim(addr, tp);

    GsOUT_PACKET_P = (PACKET*)((u8*)tp + sizeof(DR_TPAGE));
}

static void Warn_SwapAndDraw(void)
{
    VSync(SyncMode_Wait);
    GsSwapDispBuff();
    /* Draw via OT2 (where g_OtTags0 lives) — matches the OT bucket the
     * SPRTs and fade tile were pushed into. KonamiLogo uses the same OT. */
    GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
    PsyX_EndScene();

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
    GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);
}

void Pc_PlayWarningScreen(void)
{
    s32 fade;
    s32 holdFrame;

    if (g_PcConfig.skipIntros)
    {
        return;
    }

    {
        extern int g_PcHorPlusEnabled;
    }

    /* Match the Konami-logo screen setup exactly: 640×480 interlaced
     * progressive-after-Screen_Init clip (drawenv.clip.h forced to 224).
     * Screen_Init internally calls GsInitGraph2 + GsDefDispBuff2 which
     * sets gs_screen_w/h to (640,480) and drawenv ofs to (320,240). */
    Screen_Init(SCREEN_WIDTH * 2, true);

    g_ActiveBufferIdx = GsGetActiveBuff();
    GsOUT_PACKET_P    = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
    GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
    GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

    Fs_QueueStartReadTim(FILE_1ST_2ZANKO_E_TIM, FS_BUFFER_0, &s_WarnImg);
    while (Fs_QueueGetLength() > 0)
    {
        Fs_QueueUpdate();
        VSync(SyncMode_Wait);
    }

    SetDispMask(1);

    /* Lock frame pacing to 60fps for the warning. Whatever boot stage
     * ran before may have left g_IntervalVBlanks at 2 (30fps), which
     * doubles the loop durations below. */
    g_IntervalVBlanks = 1;

    /* `skipped` tracks an early exit (boot-skip key) from the fade-in or hold so the
     * fade-out below CONTINUES darkening from the current brightness instead of
     * snapping the image back to full-bright and fading THAT out — which looked like
     * the warning "playing a second time" when a held skip key caught the fade-in. */
    s32 skipped = 0;

    /* Fade-in: ~0.5s at 60fps. fade goes 255 → 0 (image appears). */
    fade = 255;
    while (fade >= 0)
    {
        Warn_DrawFadeTile(fade);
        Warn_DrawImage();
        Warn_SwapAndDraw();
        if (Warn_SkipPressed())
        {
            skipped = 1;
            break;
        }
        fade -= 8;
    }

    /* Hold the fully-faded-in image — only if the fade-in completed (no skip). */
    if (!skipped)
    {
        fade = 0; /* fully visible */
        for (holdFrame = 0; holdFrame < 180; holdFrame++)
        {
            Warn_DrawImage();
            Warn_SwapAndDraw();
            if (Warn_SkipPressed())
            {
                break;
            }
        }
    }
    if (fade < 0)
    {
        fade = 0;
    }

    /* Fade-out to solid black, CONTINUING from the current fade level (so an early
     * skip darkens smoothly from where it was rather than re-showing the full image).
     * The next boot screen fades in from black. */
    while (fade < 255)
    {
        Warn_DrawFadeTile(fade);
        Warn_DrawImage();
        Warn_SwapAndDraw();
        fade += 8;
    }
    /* Flush several solid-black frames before returning. Warn_SwapAndDraw
     * swaps the display BEFORE drawing the current OT, so the last frame built
     * is shown one iteration later — a single final frame leaves the DISPLAYED
     * buffer at the next-to-last fade level (near-black but a few RGB of the
     * brightest image pixels still show), and the konami transition then briefly
     * exposes a stale framebuffer = the warning "flashing back" after the fade.
     * Drawing black (image - 255 = 0) to BOTH display buffers + the presented
     * frame guarantees a clean black hand-off with no stale warning frame. */
    {
        s32 blackFrame;
        for (blackFrame = 0; blackFrame < 3; blackFrame++)
        {
            Warn_DrawFadeTile(255);
            Warn_DrawImage();
            Warn_SwapAndDraw();
        }
    }
}
