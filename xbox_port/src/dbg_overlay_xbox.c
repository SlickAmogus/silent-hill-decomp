/*
 * dbg_overlay_xbox.c - On-screen toast / notification overlay for the Silent
 * Hill 1 Original Xbox port.
 *
 * The shared decomp routes user-facing feedback (RetroAchievements unlocks via
 * ra_xbox.c, and every SH_DBG_ECHO in sh_log.h) through the function pointer
 * g_ShOverlayToastLine. On PC that points at dbg_overlay.c's DbgOverlay_ToastLine
 * (an SDL/OpenGL panel). Xbox has no GL overlay, so this file ports ONLY the
 * toast LOGIC from pc_port/src/dbg_overlay.c — the fixed-size line ring, the
 * FADE_IN/HOLD/FADE millisecond timers, and the append-newest behaviour — and
 * draws it with the game's OWN 12x16 font (Gfx_StringDraw), which already emits
 * into OT2 (the 2D-UI ordering table, drawn on top of the world). No new GPU
 * code, no textures: the toast is just more menu-style text.
 *
 * Interface (agreed with the 'options' and 'ra-effects' agents):
 *   void DbgOverlay_XboxToast (const char* line);  // push one line into the ring
 *   void DbgOverlay_XboxRender(void);              // draw active lines, once/frame
 * Boot wires  g_ShOverlayToastLine = DbgOverlay_XboxToast  (main_xbox.c).
 *
 * Timing uses KeTickCount (kernel ms-since-boot), the same clock xa_xbox.c uses.
 *
 * SAFETY: DbgOverlay_XboxRender is only called from the MainLoop draw path AFTER
 * GsClearOt(OT2) and BEFORE GsDrawOt(OT2), where the ordering table and the
 * shared packet buffer (GsOUT_PACKET_P) are always valid — so calling
 * Gfx_StringDraw here can never fault regardless of load state. The one thing
 * that must NOT happen early is sampling the 12x16 font atlas before it is
 * resident in VRAM (garbage glyphs during the boot logos); the call site in
 * game_main.c gates that on gameState >= GameState_MainMenu (font-readiness
 * guard). This function is additionally a cheap no-op whenever the ring is
 * empty, so an early/defensive call does nothing.
 *
 * Self-contained on purpose (only <string.h> + the kernel clock): pulling in
 * game.h here would drag the decomp type soup next to <xboxkrnl.h>; instead the
 * three font entry points and the two colour ids we use are forward-declared /
 * mirrored from bodyprog/text/text_draw.h. They are plain C-linkage symbols, so
 * this is ABI-identical to including the header.
 */
#include <string.h>
#include <stdint.h>
#include <xboxkrnl/xboxkrnl.h>   /* KeTickCount: kernel ms-since-boot clock */

/* --- game font drawer (bodyprog/text/text_draw.c) --------------------------
 * Real prototypes: void Gfx_StringSetPosition(s32,s32); void Gfx_StringSetColor(s16);
 *                  bool Gfx_StringDraw(char*, s32);
 * s32==int, s16==short on this target; the bool return is ignored, so declaring
 * it int here is ABI-safe (low byte in eax either way). */
extern void Gfx_StringSetPosition(int x, int y);
extern void Gfx_StringSetColor(short colorId);
extern int  Gfx_StringDraw(char* str, int strLength);

/* Storage for the shared toast hook (declared extern in sh_log.h; used by
 * ra_xbox.c and the SH_DBG_ECHO macro). It lived in xbox_pcfeature_stubs.c as a
 * permanent NULL "no overlay on Xbox" stub; now that there IS an overlay it is
 * owned here and wired to DbgOverlay_XboxToast at boot (main_xbox.c). Defined
 * = NULL, exactly like main_pc.c, and assigned at runtime. */
void (*g_ShOverlayToastLine)(const char* line) = NULL;

/* e_StringColorId values (text_draw.h): index into STRING_COLORS. */
#define TOAST_COLOR_BRIGHT 7 /* StringColorId_White    */
#define TOAST_COLOR_DIM    6 /* StringColorId_LightGrey */
#define TOAST_COLOR_FAINT  1 /* StringColorId_DarkGrey  */

/* --- toast ring (ported from dbg_overlay.c's s_toast) ---------------------- */
#define TOAST_MAX          4      /* lines kept on screen at once            */
#define TOAST_LINE_LEN     64     /* per-line storage incl. NUL              */
#define TOAST_DRAW_GLYPHS  40     /* Gfx_StringDraw glyph budget (clips wide) */

/* Fade envelope, milliseconds. FADE_IN/HOLD/FADE mirror dbg_overlay.c; HOLD is
 * a touch longer than PC's 1800 so a toast is readable across a living room. */
#define TOAST_FADE_IN_MS   120u
#define TOAST_HOLD_MS      2500u
#define TOAST_FADE_MS      700u
#define TOAST_LIFE_MS      (TOAST_FADE_IN_MS + TOAST_HOLD_MS + TOAST_FADE_MS)

/* Screen placement (Gfx_StringSetPosition takes screen-absolute coords on the
 * game's 320x240 center-origin UI). Top-left, one line per row. */
#define TOAST_POS_X        16
#define TOAST_POS_Y0       20
#define TOAST_LINE_STEP_Y  16

static char     s_toast[TOAST_MAX][TOAST_LINE_LEN];
static uint32_t s_pushMs[TOAST_MAX];
static int      s_count = 0;

/* Copy `src` into `dst` mapped to what the 12x16 atlas can actually render:
 *   - real space (0x20)      -> '_'  (the drawer treats '_' as a space advance)
 *   - '!' and '&'            -> kept ('!' -> '\\', '&' -> '^' inside the drawer)
 *   - 0x27..0x7A ('..z)      -> kept (the drawer's ASCII glyph range)
 *   - anything else          -> '?'  (in-range placeholder; no missing-glyph junk)
 * This is what makes arbitrary RA / SH_DBG_ECHO text (spaces, mixed case,
 * punctuation) render cleanly instead of sampling out-of-atlas cells. */
static void Toast_Sanitize(char* dst, const char* src)
{
    int i;

    for (i = 0; i < TOAST_LINE_LEN - 1 && src[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ')
            dst[i] = '_';
        else if (c == '!' || c == '&')
            dst[i] = (char)c;
        else if (c >= 0x27 && c <= 0x7A)
            dst[i] = (char)c;
        else
            dst[i] = '?';
    }
    dst[i] = '\0';
}

/* Push one line into the ring (newest at the bottom). When full, the oldest
 * line is dropped and the rest shift up — matching dbg_overlay.c's overflow. */
void DbgOverlay_XboxToast(const char* line)
{
    if (line == NULL)
        return;

    if (s_count >= TOAST_MAX)
    {
        int i;
        for (i = 0; i < TOAST_MAX - 1; i++)
        {
            memcpy(s_toast[i], s_toast[i + 1], TOAST_LINE_LEN);
            s_pushMs[i] = s_pushMs[i + 1];
        }
        s_count = TOAST_MAX - 1;
    }

    Toast_Sanitize(s_toast[s_count], line);
    s_pushMs[s_count] = (uint32_t)KeTickCount;
    s_count++;
}

/* Per-frame: expire finished lines, then draw the survivors top-left. Called
 * from the MainLoop draw path (game_main.c) after OT0 is drawn and before OT2
 * is walked, so the text lands in OT2 on top of the world in EVERY game state
 * (gameplay, menus, cutscenes). The game font has no per-glyph alpha, so the
 * fade is stepped through colour: bright -> dim -> faint -> gone. */
void DbgOverlay_XboxRender(void)
{
    uint32_t now = (uint32_t)KeTickCount;
    int      i, w;

    /* Drop lines whose full lifetime has elapsed; compact the rest to the front
     * (they age oldest-first, so this preserves order). */
    w = 0;
    for (i = 0; i < s_count; i++)
    {
        if ((uint32_t)(now - s_pushMs[i]) < TOAST_LIFE_MS)
        {
            if (w != i)
            {
                memcpy(s_toast[w], s_toast[i], TOAST_LINE_LEN);
                s_pushMs[w] = s_pushMs[i];
            }
            w++;
        }
    }
    s_count = w;

    if (s_count == 0)
        return; /* nothing live — cheap no-op (also the safe early-boot path) */

    for (i = 0; i < s_count; i++)
    {
        uint32_t age = now - s_pushMs[i];
        short    colorId;

        if (age < TOAST_FADE_IN_MS + TOAST_HOLD_MS)
            colorId = TOAST_COLOR_BRIGHT;                       /* fade-in + hold */
        else if (age < TOAST_FADE_IN_MS + TOAST_HOLD_MS + (TOAST_FADE_MS / 2))
            colorId = TOAST_COLOR_DIM;                          /* fade, first half */
        else
            colorId = TOAST_COLOR_FAINT;                        /* fade, tail       */

        Gfx_StringSetColor(colorId);
        Gfx_StringSetPosition(TOAST_POS_X, TOAST_POS_Y0 + (i * TOAST_LINE_STEP_Y));
        /* Glyph budget bounds packet use / off-screen spill; the line is NUL-
         * terminated so short lines stop early anyway. */
        Gfx_StringDraw(s_toast[i], TOAST_DRAW_GLYPHS);
    }
}
