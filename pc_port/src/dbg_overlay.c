#include "game.h"

#ifdef SH_PC_PORT

#include <stdio.h>
#include <string.h>
#include <SDL_scancode.h>
#include <SDL_keyboard.h>
#include <SDL_timer.h>
#include <SDL_mouse.h>     /* console pointer + click-drag selection */
#include <SDL_clipboard.h> /* console Ctrl+C / Ctrl+V */
#include <stdlib.h> /* exit() for the console `quit` command */
#include "bodyprog/bodyprog.h"
#include "sh_log.h"
#include "dbg_overlay.h"
#include "pc_config.h"

#include <PsyX/common/glad.h>

extern int g_windowWidth;
extern int g_windowHeight;
extern void vcGetNowCamPos(VECTOR3* cam_pos);
extern void Collision_SurfaceGet(s_CollisionSurface* coll, q19_12 posX, q19_12 posZ);

/* Keyframe inspector state (game_main.c). The K key toggles g_DebugAnimKfView and
 * , / . step g_DebugAnimKf; player_control.c poses Harry on that frame and publishes
 * the active anim header's keyframe count as g_DebugAnimKfMax. The anim info panel
 * below mirrors the collision panel, shown while the inspector is on. */
extern int g_DebugAnimKfView;
extern int g_DebugAnimKf;
extern int g_DebugAnimKfMax;

/* Continuous scrollback: the ring keeps this many lines; the panel shows the
 * newest slice (or wherever the user scrolled). [0] = newest. */
#define MAX_CONSOLE 256
#define LINE_LEN    64
#define GLYPH_W     8
#define GLYPH_H     8
#define SCALE       2

/* Console backlog/text width, in glyph columns. Wide enough to fill a 4K window
 * at 2x (3840 / (8*2) = 240) so text spans the whole window instead of stopping
 * at a fixed panel edge, and long lines wrap onto extra rows rather than being
 * clipped. Kept separate from LINE_LEN (which still sizes the short toast and the
 * command-input line). */
#define CON_COLS    240

/* Most text rows the PANEL can ever show at once (+1 for the input prompt).
 * The actual row count is derived from the window height each frame. */
#define CONSOLE_VIS_MAX 62

#define TEX_W (CON_COLS * GLYPH_W)               /* 1920 */
#define TEX_H ((CONSOLE_VIS_MAX + 1) * GLYPH_H)  /* +1 row for the input prompt */

static char s_console[MAX_CONSOLE][CON_COLS];
static int  s_console_count  = 0;
static int  s_console_dirty  = 0;
static int  s_prev_a = 0;
static int  s_prev_b = 0;

/* ---- Interactive console (Quake style) ----
 * `~` toggles the console: open = panel slides in, game time + controller
 * input freeze (g_PcConsoleInputActive) and the "> _" prompt is live
 * immediately. `~` again closes and unfreezes. While open, PgUp/PgDn and the
 * mouse wheel scroll the backlog; Up/Down recall command history; Enter
 * submits (game runs un-frozen for a short apply window so the command's
 * effect shows, then freezes again). */
int g_PcConsoleInputActive = 0;

/* Console open/closed (replaces the old show_console bit-2 overlay toggle —
 * the config key now only controls the EXTERNAL console window). */
static int s_console_open = 0;
/* Scrollback offset: 0 = pinned to newest; +N = N lines back in the ring. */
static int s_scroll   = 0;
/* Text rows currently shown (derived from window height in Render). */
static int s_vis_rows = 24;

/* ---- Console mouse: pointer + click-drag selection + clipboard ----
 * A selection endpoint is (line, col): line = ring index into s_console
 * (bigger = older = higher on screen), -1 = the prompt row, col = character
 * boundary 0..CON_COLS. Endpoints track the ring in push_console so the
 * selected TEXT stays selected as new lines arrive. The highlight is drawn
 * as quads between the backdrop and the glyph texture; Ctrl+C joins the
 * covered lines (older→newer) into the system clipboard, Ctrl+V appends the
 * clipboard's first line to the prompt (letters uppercased — commands arrive
 * uppercase by convention). The pointer is drawn by the overlay itself (the
 * OS cursor is force-hidden every frame by PsyX) using the game's own arrow
 * sprite baked into dbg_cursor.inc. */
static int s_sel_valid  = 0; /* a selection exists */
static int s_sel_active = 0; /* left button held, drag in progress */
static int s_sel_a_line, s_sel_a_col; /* anchor */
static int s_sel_b_line, s_sel_b_col; /* drag end */
static int s_prev_lmb = 0;
/* Panel hit-test geometry: the GL viewport the console was last drawn in
 * (window pixels, GL bottom-left origin). Captured in Render. */
static int s_hit_vp[4];
static int s_hit_valid = 0;

/* Set when input mode ends (submit or ~ exit) and held until the keys are
 * physically released: DbgOverlay_Update runs BEFORE the game's input parse,
 * so without this the submitting Enter reached the game the same frame as
 * Start — on the main menu that activated Continue with no save and crashed
 * in AddPrim on a NO_VALUE-derived OT pointer. game_main suppresses
 * controller input while this or input mode is active. */
int g_PcConsoleSwallowInput = 0;

#define INPUT_BUF_CAP    (LINE_LEN - 4) /* room for "> " + "_" */
static char          s_input_buf[INPUT_BUF_CAP];
static int           s_input_len = 0;

/* Console command history: Up/Down recall recently entered commands. */
#define CONSOLE_HIST_MAX 8
static char s_hist[CONSOLE_HIST_MAX][INPUT_BUF_CAP];
static int  s_hist_count = 0;   /* number stored (<= MAX)                          */
static int  s_hist_write = 0;   /* next circular write slot                        */
static int  s_hist_nav   = -1;  /* -1 = live edit; 0 = newest .. count-1 = oldest  */
static char s_hist_edit[INPUT_BUF_CAP];  /* live input parked while browsing       */
static void Console_LoadHist(int nav) {
    int idx = (s_hist_write - 1 - nav + 2 * CONSOLE_HIST_MAX) % CONSOLE_HIST_MAX;
    strncpy(s_input_buf, s_hist[idx], INPUT_BUF_CAP - 1);
    s_input_buf[INPUT_BUF_CAP - 1] = '\0';
    s_input_len = (int)strlen(s_input_buf);
}
static unsigned char s_prev_keys[128]; /* must cover arrow keys (scancodes 79-82) */

/* Slide animation: 0 = fully off-screen above the top edge, 1 = at rest.
 * Eased toward the target each frame in DbgOverlay_Update; the continuous value
 * makes mashing `~` reverse smoothly instead of snapping. */
static float s_console_slide = 0.0f;
#define CONSOLE_SLIDE_STEP 0.18f

/* After a console command, the game runs unfrozen for this long so the command's
 * effect renders/animates, then input mode resumes (frozen) for the next command.
 * 0 = not in an apply window. */
#define CONSOLE_APPLY_MS 175u
static unsigned s_console_apply_until = 0;

/* GL overlay resources, initialised once on first Render call */
static GLuint s_prog = 0;
static GLuint s_vao  = 0;
static GLuint s_vbo  = 0;
static GLuint s_tex  = 0;
static GLuint s_bg_tex = 0; /* 2x2 translucent black, stretched as the console backdrop */
static GLuint s_sel_tex = 0;    /* 2x2 translucent blue: selection highlight */
static GLuint s_cursor_tex = 0; /* 32x32 arrow pointer (dbg_cursor.inc) */

#include "dbg_cursor.inc"
static GLint  s_u_tex = -1;
static int    s_gl_inited = 0;

/* ---- Collision visualizer panel (toggled by ') ----
 * A fixed live panel (bottom-right) that queries the floor-collision function
 * at the player and 4 probe points each frame, surfacing what Collision_SurfaceGet
 * returns (ground height, slope fields, valid-point count) for the decomp team. */
#define COLL_COLS  32
#define COLL_LINES 16
#define COLL_TEX_W (COLL_COLS * GLYPH_W)
#define COLL_TEX_H (COLL_LINES * GLYPH_H)
static char   s_coll_lines[COLL_LINES][COLL_COLS];
static int    s_coll_count = 0;
static int    s_coll_on    = 0;
static int    s_prev_apos  = 0;
static GLuint s_coll_tex   = 0;

/* Read by collision.c (func_8006B318) to capture the wall segments the player's
 * collision evaluates while the visualizer is on. Set by the ' toggle. */
int g_CollVisEnabled = 0;

/* ---- Animation inspector panel (shown while the K keyframe inspector is on) ----
 * Mirrors the collision panel: a live text panel surfacing the player's current
 * anim state (the forced keyframe + its max, the active anim index/flags, control
 * state) so the keyframe being scrubbed is readable next to Harry's pose. */
#define ANIM_COLS  28
#define ANIM_LINES 8
#define ANIM_TEX_W (ANIM_COLS * GLYPH_W)
#define ANIM_TEX_H (ANIM_LINES * GLYPH_H)
static char   s_anim_lines[ANIM_LINES][ANIM_COLS];
static int    s_anim_count = 0;
static GLuint s_anim_tex   = 0;

/* ---- Randomizer score (left, vertically centred) ----
 * Unlike the panels above this is NOT a debug overlay: it is the gamemode's HUD,
 * so it is gated on a live randomizer run rather than on allow_debug_controls. */
#define SCORE_COLS 16
#define SCORE_TEX_W (SCORE_COLS * GLYPH_W)
#define SCORE_TEX_H (1 * GLYPH_H)
static char   s_score_line[SCORE_COLS];
static int    s_score_on  = 0;
static GLuint s_score_tex = 0;

/* ---- System-message toast (top-left) ----
 * When the ingame console overlay is hidden, the newest line(s) pushed to it
 * (cheat toggles, graphics-setting changes) still flash briefly in the top-left,
 * fading out, so the user gets feedback without opening the console. Armed only
 * once MainLoop is running (first DbgOverlay_Update) so the boot-time SH_LOG init
 * spam — pushed before MainLoop — never shows. */
#define TOAST_MAX        4
#define TOAST_TEX_W      (LINE_LEN * GLYPH_W)   /* 512 */
#define TOAST_TEX_H      (TOAST_MAX * GLYPH_H)
#define TOAST_FADE_IN_MS 120u
#define TOAST_HOLD_MS    1800u
#define TOAST_FADE_MS    600u
static char   s_toast[TOAST_MAX][LINE_LEN];
static int    s_toast_count   = 0;
static Uint32 s_toast_last_ms = 0;
static int    s_toast_armed   = 0;
static GLuint s_toast_tex     = 0;

/* ---- Collision wireframe: world-space segments captured during the frame's
 * collision pass, projected and drawn as GL lines (no depth test → visible
 * through walls, RE4-style). Cleared after each render. ---- */
extern MATRIX VbWvsMatrix;          /* world->view rotation (Q12), vw_calc.c */
extern long   ReadGeomScreen(void); /* GTE projection distance H */
extern void   CollVis_CaptureCell(q19_12 px, q19_12 pz); /* full-cell wall capture, collision.c */

#define CV_MAX_SEGS 2048
#define CV_MAX_CYLS 256
#define CV_MAX_HITS 256
/* worst-case GL vertices: cell segs + hit segs (1 line) + cylinders (12-edge box)
 * + per-frame hit cylinders (32, 12-edge box). */
#define CV_VERT_CAP (((CV_MAX_SEGS + CV_MAX_HITS) * 2) + (CV_MAX_CYLS * 12 * 2) + (32 * 12 * 2))
typedef struct { s32 ax, ay, az, bx, by, bz; int hit; } s_CvSeg;

/* Cell geometry (green) — cached; only re-walked when Harry's collision cell
 * changes (CollVis_ClearCell). Re-projected every frame, but not re-iterated. */
static s_CvSeg s_cvSegs[CV_MAX_SEGS];
static int     s_cvSegCount = 0;

/* Per-frame contacted faces (red) — rebuilt every frame from func_8006B318. */
static s_CvSeg s_cvHits[CV_MAX_HITS];
static int     s_cvHitCount = 0;

/* ptr_18 cylinder colliders (trees/poles): center + radius, drawn as a box.
 * Cached with the cell geometry. */
typedef struct { s32 cx, cy, cz, r; } s_CvCyl;
static s_CvCyl s_cvCyls[CV_MAX_CYLS];
static int     s_cvCylCount = 0;
static s32     s_cvFloorY   = 0; /* player ground Y (Q12), box base for cylinders */

/* Per-frame RED cylinders for the obstacle/NPC actually BLOCKING Harry this
 * frame, captured at the collision point in collision.c (any chunk, not just
 * Harry's current cell — that's why the cached cyan cylinders miss the one that
 * stops you when running fast into an adjacent cell). cy = the collider's own Y. */
#define CV_MAX_HITCYLS 32
static s_CvCyl s_cvHitCyls[CV_MAX_HITCYLS];
static int     s_cvHitCylCount = 0;

/* collState snapshot from the player's func_8006A4A8 pass (filled by collision.c). */
s_CollStateDbg g_CollStateDbg = {0};

/* GL line resources */
static GLuint  s_line_prog = 0;
static GLuint  s_line_vao  = 0;
static GLuint  s_line_vbo  = 0;

/* Cell geometry segment (green). hit param kept for signature stability; cell
 * segs are always non-contacting (contacts go to CollVis_CaptureHit). */
void CollVis_CaptureSeg(s32 ax, s32 ay, s32 az, s32 bx, s32 by, s32 bz, int hit)
{
    if (s_cvSegCount >= CV_MAX_SEGS)
        return;
    s_cvSegs[s_cvSegCount].ax = ax; s_cvSegs[s_cvSegCount].ay = ay; s_cvSegs[s_cvSegCount].az = az;
    s_cvSegs[s_cvSegCount].bx = bx; s_cvSegs[s_cvSegCount].by = by; s_cvSegs[s_cvSegCount].bz = bz;
    s_cvSegs[s_cvSegCount].hit = hit;
    s_cvSegCount++;
}

/* A face the player is actually colliding with this frame (drawn red). */
void CollVis_CaptureHit(s32 ax, s32 ay, s32 az, s32 bx, s32 by, s32 bz)
{
    if (s_cvHitCount >= CV_MAX_HITS)
        return;
    s_cvHits[s_cvHitCount].ax = ax; s_cvHits[s_cvHitCount].ay = ay; s_cvHits[s_cvHitCount].az = az;
    s_cvHits[s_cvHitCount].bx = bx; s_cvHits[s_cvHitCount].by = by; s_cvHits[s_cvHitCount].bz = bz;
    s_cvHits[s_cvHitCount].hit = 1;
    s_cvHitCount++;
}

void CollVis_CaptureCylinder(s32 cx, s32 cy, s32 cz, s32 r)
{
    if (s_cvCylCount >= CV_MAX_CYLS)
        return;
    s_cvCyls[s_cvCylCount].cx = cx; s_cvCyls[s_cvCylCount].cy = cy;
    s_cvCyls[s_cvCylCount].cz = cz; s_cvCyls[s_cvCylCount].r  = r;
    s_cvCylCount++;
}

/* The obstacle/NPC that actually blocked Harry this frame (drawn RED). Called
 * from collision.c at the block site with WORLD coords (origin + offset) << 4. */
void CollVis_CaptureHitCylinder(s32 cx, s32 cy, s32 cz, s32 r)
{
    if (s_cvHitCylCount >= CV_MAX_HITCYLS)
        return;
    s_cvHitCyls[s_cvHitCylCount].cx = cx; s_cvHitCyls[s_cvHitCylCount].cy = cy;
    s_cvHitCyls[s_cvHitCylCount].cz = cz; s_cvHitCyls[s_cvHitCylCount].r  = r;
    s_cvHitCylCount++;
}

/* Clear the cached cell geometry — called by collision.c when Harry's cell
 * changes, right before re-walking the new cell's arrays. */
void CollVis_ClearCell(void)
{
    s_cvSegCount = 0;
    s_cvCylCount = 0;
}

/* IBM PC 8x8 bitmap font — public domain.
 * One byte per row, MSB = leftmost pixel.
 * Chars below 0x20 and above 0x7E are left zeroed (blank). */
static const unsigned char s_font[128][8] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x21] = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    [0x22] = {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x23] = {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    [0x24] = {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    [0x25] = {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    [0x26] = {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    [0x27] = {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    [0x28] = {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    [0x29] = {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    [0x2A] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    [0x2B] = {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    [0x2C] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},
    [0x2D] = {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    [0x2F] = {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    [0x30] = {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    [0x31] = {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    [0x32] = {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    [0x33] = {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    [0x34] = {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    [0x35] = {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    [0x36] = {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    [0x37] = {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    [0x38] = {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    [0x39] = {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    [0x3A] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
    [0x3B] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    [0x3C] = {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
    [0x3D] = {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    [0x3E] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    [0x3F] = {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    [0x40] = {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    [0x41] = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    [0x42] = {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    [0x43] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    [0x44] = {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    [0x45] = {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    [0x46] = {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    [0x47] = {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    [0x48] = {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    [0x49] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    [0x4A] = {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    [0x4B] = {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    [0x4C] = {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    [0x4D] = {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    [0x4E] = {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    [0x4F] = {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    [0x50] = {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    [0x51] = {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    [0x52] = {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    [0x53] = {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    [0x54] = {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    [0x55] = {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    [0x56] = {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    [0x57] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    [0x58] = {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    [0x59] = {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    [0x5A] = {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    [0x5B] = {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    [0x5C] = {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    [0x5D] = {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    [0x5E] = {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    [0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    [0x60] = {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    [0x61] = {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    [0x62] = {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    [0x63] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    [0x64] = {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    [0x65] = {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    [0x66] = {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},
    [0x67] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    [0x68] = {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    [0x69] = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    [0x6A] = {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    [0x6B] = {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    [0x6C] = {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    [0x6D] = {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    [0x6E] = {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    [0x6F] = {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    [0x70] = {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    [0x71] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    [0x72] = {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    [0x73] = {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    [0x74] = {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    [0x75] = {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    [0x76] = {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    [0x77] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    [0x78] = {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    [0x79] = {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    [0x7A] = {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    [0x7B] = {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    [0x7C] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    [0x7D] = {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    [0x7E] = {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

static void push_console_raw(const char* line)
{
    int shift = (s_console_count < MAX_CONSOLE) ? s_console_count : MAX_CONSOLE - 1;
    int i;
    for (i = shift; i > 0; i--)
        memcpy(s_console[i], s_console[i - 1], CON_COLS);
    strncpy(s_console[0], line, CON_COLS - 1);
    s_console[0][CON_COLS - 1] = '\0';
    if (s_console_count < MAX_CONSOLE)
        s_console_count++;
    /* While scrolled back, keep the viewed lines stable as new ones arrive
     * (the ring shifted under the view); clamped in the texture build. */
    if (s_scroll > 0 && s_scroll < MAX_CONSOLE - 1)
        s_scroll++;
    /* Selection endpoints ride the ring the same way (prompt row -1 stays). */
    if (s_sel_valid) {
        if (s_sel_a_line >= 0) s_sel_a_line++;
        if (s_sel_b_line >= 0) s_sel_b_line++;
        if (s_sel_a_line >= MAX_CONSOLE || s_sel_b_line >= MAX_CONSOLE) {
            s_sel_valid  = 0;
            s_sel_active = 0;
        }
    }
}

/* Push a log line, wrapping it onto extra rows so nothing is clipped at the
 * window's right edge. The wrap column follows the live window width (each glyph
 * is GLYPH_W*SCALE device px) so text fills whatever width the window has; chunks
 * are pushed head-first so a wrapped message still reads top-to-bottom (oldest
 * chunk highest, newest chunk on the bottom row). */
static void push_console(const char* line)
{
    int wrapCols = g_windowWidth / (GLYPH_W * SCALE) - 1;
    int len      = (int)strlen(line);

    if (wrapCols < 16)           wrapCols = 16;
    if (wrapCols > CON_COLS - 1) wrapCols = CON_COLS - 1;

    while (len > wrapCols) {
        char chunk[CON_COLS];
        int  brk = wrapCols;
        int  i;
        /* Prefer to break on the last space within the row so words stay whole. */
        for (i = wrapCols; i > 0; i--) {
            if (line[i] == ' ') { brk = i; break; }
        }
        memcpy(chunk, line, brk);
        chunk[brk] = '\0';
        push_console_raw(chunk);
        if (line[brk] == ' ') brk++; /* swallow the space we split on */
        line += brk;
        len  -= brk;
    }
    push_console_raw(line);
}

/* The console panel is open (frozen-input state or the brief command apply
 * window). The menu mouse-cursor system gates itself off on this so clicks
 * in the console never leak hover/click injections into whatever menu is
 * underneath. */
int Pc_ConsoleIsOpen(void)
{
    return s_console_open;
}

/* Text a selection endpoint's line refers to (-1 = the live prompt input). */
static const char* console_sel_text(int line)
{
    return (line < 0) ? s_input_buf : s_console[line];
}

/* Order the two selection endpoints visually: FIRST = higher on screen =
 * larger ring index (the prompt, -1, is always last). */
static void console_sel_order(int* fl, int* fc, int* ll, int* lc)
{
    if (s_sel_a_line > s_sel_b_line ||
        (s_sel_a_line == s_sel_b_line && s_sel_a_col <= s_sel_b_col)) {
        *fl = s_sel_a_line; *fc = s_sel_a_col;
        *ll = s_sel_b_line; *lc = s_sel_b_col;
    } else {
        *fl = s_sel_b_line; *fc = s_sel_b_col;
        *ll = s_sel_a_line; *lc = s_sel_a_col;
    }
}

static void Console_CopySelection(void)
{
    /* Worst case: every ring line + newlines. */
    static char out[MAX_CONSOLE * (CON_COLS + 1) + 4];
    int fl, fc, ll, lc, cur, n = 0;

    if (!s_sel_valid)
        return;
    console_sel_order(&fl, &fc, &ll, &lc);
    if (fl == ll && fc == lc)
        return; /* empty click, nothing to copy */

    for (cur = fl; cur >= ll && cur >= -1; cur--) {
        const char* s  = console_sel_text(cur);
        int         len = (int)strlen(s);
        int         c0  = (cur == fl) ? fc : 0;
        int         c1  = (cur == ll) ? lc : len;

        /* The prompt row renders as "> text_": shift its screen columns past
         * the 2-char prefix so they index the input buffer. */
        if (cur == -1) {
            c0 -= 2;
            c1 -= 2;
            if (c0 < 0) c0 = 0;
            if (c1 < 0) c1 = 0;
        }
        if (c0 > len) c0 = len;
        if (c1 > len) c1 = len;
        if (c1 > c0) {
            memcpy(out + n, s + c0, (size_t)(c1 - c0));
            n += c1 - c0;
        }
        if (cur != ll)
            out[n++] = '\n';
    }
    out[n] = '\0';
    SDL_SetClipboardText(out);
}

static void Console_Paste(void)
{
    char*       clip = SDL_GetClipboardText();
    const char* p;

    if (!clip)
        return;
    for (p = clip; *p && s_input_len < INPUT_BUF_CAP - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n')
            break; /* paste the first line only */
        if (c < 32 || c > 126)
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32; /* commands arrive uppercase */
        s_input_buf[s_input_len++] = (char)c;
    }
    s_input_buf[s_input_len] = '\0';
    s_console_dirty = 1;
    SDL_free(clip);
}

static void overlay_gl_init(void)
{
    GLuint vs, fs;
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    v_uv = a_uv;\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    static const char* fs_src =
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(u_tex, v_uv);\n"
        "}\n";

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);

    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    s_u_tex = glGetUniformLocation(s_prog, "u_tex");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &s_coll_tex);
    glBindTexture(GL_TEXTURE_2D, s_coll_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, COLL_TEX_W, COLL_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &s_anim_tex);
    glBindTexture(GL_TEXTURE_2D, s_anim_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ANIM_TEX_W, ANIM_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &s_score_tex);
    glBindTexture(GL_TEXTURE_2D, s_score_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SCORE_TEX_W, SCORE_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenTextures(1, &s_toast_tex);
    glBindTexture(GL_TEXTURE_2D, s_toast_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TOAST_TEX_W, TOAST_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* Console backdrop: a flat translucent black quad behind the text so the
     * full-width panel is readable over any scene. */
    {
        static const unsigned char bg[2][2][4] = {
            { { 0, 0, 0, 205 }, { 0, 0, 0, 205 } },
            { { 0, 0, 0, 205 }, { 0, 0, 0, 205 } },
        };
        glGenTextures(1, &s_bg_tex);
        glBindTexture(GL_TEXTURE_2D, s_bg_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Selection highlight: translucent blue, stretched per selected span. */
    {
        static const unsigned char sel[2][2][4] = {
            { { 90, 130, 220, 110 }, { 90, 130, 220, 110 } },
            { { 90, 130, 220, 110 }, { 90, 130, 220, 110 } },
        };
        glGenTextures(1, &s_sel_tex);
        glBindTexture(GL_TEXTURE_2D, s_sel_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, sel);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Mouse pointer: the baked 32x32 arrow (see dbg_cursor.inc). */
    {
        unsigned char rgba[32][32][4];
        int x, y;
        for (y = 0; y < 32; y++) {
            for (x = 0; x < 32; x++) {
                unsigned char b = CURSOR_PIX[y][x / 2];
                unsigned char n = (x & 1) ? (b >> 4) : (b & 0xF);
                rgba[y][x][0] = CURSOR_PAL[n][0];
                rgba[y][x][1] = CURSOR_PAL[n][1];
                rgba[y][x][2] = CURSOR_PAL[n][2];
                rgba[y][x][3] = CURSOR_PAL[n][3];
            }
        }
        glGenTextures(1, &s_cursor_tex);
        glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Colored-line program for the collision wireframe (a_pos = NDC, a_col = RGB). */
    {
        static const char* lvs_src =
            "attribute vec2 a_pos;\n"
            "attribute vec3 a_col;\n"
            "varying vec3 v_col;\n"
            "void main() { v_col = a_col; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        static const char* lfs_src =
            "varying vec3 v_col;\n"
            "void main() { gl_FragColor = vec4(v_col, 1.0); }\n";
        GLuint lvs = glCreateShader(GL_VERTEX_SHADER);
        GLuint lfs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(lvs, 1, &lvs_src, NULL); glCompileShader(lvs);
        glShaderSource(lfs, 1, &lfs_src, NULL); glCompileShader(lfs);
        s_line_prog = glCreateProgram();
        glAttachShader(s_line_prog, lvs);
        glAttachShader(s_line_prog, lfs);
        glBindAttribLocation(s_line_prog, 0, "a_pos");
        glBindAttribLocation(s_line_prog, 1, "a_col");
        glLinkProgram(s_line_prog);
        glDeleteShader(lvs); glDeleteShader(lfs);

        glGenVertexArrays(1, &s_line_vao);
        glGenBuffers(1, &s_line_vbo);
        glBindVertexArray(s_line_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_line_vbo);
        glBufferData(GL_ARRAY_BUFFER, CV_VERT_CAP * 5 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    s_gl_inited = 1;
}

/* Caller must have s_tex bound to GL_TEXTURE_2D before calling. */
static unsigned char s_pixels[TEX_H][TEX_W][4];

static void overlay_render_row(int rowIdx, const char* str)
{
    int cx, x, y;

    for (cx = 0; *str && cx < CON_COLS; cx++, str++) {
        unsigned int ch = (unsigned char)*str;
        if (ch >= 128) continue;
        for (y = 0; y < GLYPH_H; y++) {
            unsigned char row = s_font[ch][y];
            for (x = 0; x < GLYPH_W; x++) {
                if (row & (1u << x)) {
                    s_pixels[rowIdx * GLYPH_H + y][cx * GLYPH_W + x][0] = 255;
                    s_pixels[rowIdx * GLYPH_H + y][cx * GLYPH_W + x][1] = 255;
                    s_pixels[rowIdx * GLYPH_H + y][cx * GLYPH_W + x][2] = 255;
                    s_pixels[rowIdx * GLYPH_H + y][cx * GLYPH_W + x][3] = 255;
                }
            }
        }
    }
}

static void overlay_update_texture(void)
{
    /* Quake-style layout: prompt on the panel's LAST row, backlog above it
     * newest-at-bottom. s_scroll shifts the visible slice into older lines;
     * a marker replaces the top row while scrolled so it's obvious the view
     * isn't live. Only rows [0 .. s_vis_rows] of the texture are drawn (the
     * panel quad crops with a partial V range). */
    int r;
    int maxScroll = s_console_count - s_vis_rows;

    if (maxScroll < 0) maxScroll = 0;
    if (s_scroll > maxScroll) s_scroll = maxScroll;
    if (s_scroll < 0) s_scroll = 0;

    memset(s_pixels, 0, sizeof(s_pixels));

    for (r = 0; r < s_vis_rows; r++) {
        /* Bottom text row (r == s_vis_rows-1) shows s_console[s_scroll]. */
        int idx = s_scroll + (s_vis_rows - 1 - r);
        if (idx < 0 || idx >= s_console_count) continue;
        if (s_scroll > 0 && r == 0) continue; /* row 0 becomes the scroll marker */
        overlay_render_row(r, s_console[idx]);
    }

    if (s_scroll > 0) {
        char marker[LINE_LEN];
        snprintf(marker, LINE_LEN, "^^^ %d newer line(s) below - PgDn / wheel ^^^", s_scroll);
        overlay_render_row(0, marker);
    }

    {
        char prompt[LINE_LEN];
        snprintf(prompt, LINE_LEN, "> %s_", s_input_buf);
        overlay_render_row(s_vis_rows, prompt);
    }

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, s_pixels);
}

/* Query the floor-collision function at the player + 4 probe points and format
 * the results into the live panel. Collision_SurfaceGet is a read-only query (it finds
 * the IPD cell via func_800426E4 and walks it); calling it a few extra times per
 * frame is cheap next to the per-frame calls the game already makes. */
static void coll_gather(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    q19_12 px = player->position.vx;
    q19_12 py = player->position.vy;
    q19_12 pz = player->position.vz;
    const q19_12 D = 2 * 4096; /* 2.0 in Q19.12 */
    /* Floor-probe results are cached and refreshed every 4th frame — they change
     * slowly and each Collision_SurfaceGet is a full query, so this trims the per-frame
     * cost. The wireframe + collState panel still update every frame. */
    static s_CollisionSurface c0, cxp, cxn, czp, czn;
    static int s_floorThrottle = 0;
    int n = 0;

    /* Capture the local collision-cell geometry (green wireframe). Cheap on most
     * frames — it only re-walks the arrays when Harry's cell changes. */
    s_cvFloorY = py;
    CollVis_CaptureCell(px, pz);

    if ((s_floorThrottle++ & 3) == 0) {
        Collision_SurfaceGet(&c0,  px,     pz);
        Collision_SurfaceGet(&cxp, px + D, pz);
        Collision_SurfaceGet(&cxn, px - D, pz);
        Collision_SurfaceGet(&czp, px,     pz + D);
        Collision_SurfaceGet(&czn, px,     pz - D);
    }

#define CL(...) do { if (n < COLL_LINES) snprintf(s_coll_lines[n++], COLL_COLS, __VA_ARGS__); } while (0)
    /* All positional/height values are raw fixed-point (Q12, 4096 = 1.0u) — the
     * engine's real numbers, not float conversions. */
    CL("== COLLISION  (' toggle)  [Q12] ==");
    CL("pos %d,%d,%d", (int)px, (int)py, (int)pz);
    /* [WALLEDGE] latched wall-edge bump (the "ran into a wall" reaction, no
     * movement clamp). cnt>=3 = FIRED -> Harry bumped. hY vs gH shows whether his
     * Y lagged the ground (the falling-through-smoothing link). "Nt ago" persists
     * through a delayed mark. Kept near the top so it's on-screen. */
    {
        extern struct s_WallEdgeDbg { int active; s32 gH, bound, wallCount, harryY, tick; } g_WallEdgeDbg;
        extern s32 g_TickCount;
        if (g_WallEdgeDbg.active)
            CL("WALLEDGE gH=%d bnd=%d cnt=%d hY=%d %dt ago %s",
               g_WallEdgeDbg.gH, g_WallEdgeDbg.bound, g_WallEdgeDbg.wallCount,
               g_WallEdgeDbg.harryY, (int)(g_TickCount - g_WallEdgeDbg.tick),
               g_WallEdgeDbg.wallCount >= 3 ? "FIRED" : "");
        else
            CL("WALLEDGE (none yet)");
    }
    CL("ground H=%d", (int)c0.groundHeight);
    CL("tilt X=%d Z=%d type=%d", (int)c0.tiltAngleX, (int)c0.tiltAngleZ, (int)c0.groundType);
    CL("playerY-ground %+d", (int)(py - c0.groundHeight));
    CL("probe 2u(8192)  H / type");
    CL("+X %d/%d  -X %d/%d",
       (int)cxp.groundHeight, (int)cxp.groundType,
       (int)cxn.groundHeight, (int)cxn.groundType);
    CL("+Z %d/%d  -Z %d/%d",
       (int)czp.groundHeight, (int)czp.groundType,
       (int)czn.groundHeight, (int)czn.groundType);

    /* Labels are the decomp's struct-field paths (s_CollisionState), so the
     * overlay maps 1:1 onto the renamed fields. func_8006A4A8 kept its raw name
     * upstream (no semantic rename). Values are raw Q12. */
    CL("- collState func_8006A4A8 -");
    if (g_CollStateDbg.valid) {
        CL("point.subcellIdx=%d", g_CollStateDbg.faceIdx);
        CL("pt.f20.radiusCollDiff=%d", g_CollStateDbg.dist);
        CL("charaState.radius=%d", g_CollStateDbg.radius);
        CL("result.offset.vx/vz=%d,%d",
           (int)g_CollStateDbg.pushX, (int)g_CollStateDbg.pushZ);
        CL("f44.f0: act=%d ang=%d,%d",
           g_CollStateDbg.respActive, g_CollStateDbg.angMin, g_CollStateDbg.angMax);
        CL("heightDisabled=%d groundType=%d",
           g_CollStateDbg.groundFlag, g_CollStateDbg.groundType);
    } else {
        CL("(player pass not seen)");
    }
    /* Animation-driven collision cylinder OFFSET from Harry's visual center
     * (Collision_CharaCollisionSet, per keyframe). If this spikes large (|val| >>
     * 4096 = 1u) while sprinting, the collision body has drifted off Harry ->
     * the invisible-wall desync. r/h are the keyframe radius/height (also vary). */
    CL("cylOff=(%d,%d) r=%d h=%d",
       (int)g_SysWork.playerWork.player.collision.shapeOffsets.cylinder.vx,
       (int)g_SysWork.playerWork.player.collision.shapeOffsets.cylinder.vz,
       (int)g_SysWork.playerWork.player.collision.cylinder.radius,
       (int)g_SysWork.playerWork.player.collision.cylinder.field_2);
#undef CL

    /* Decrement the contact latch each frame so a held snapshot eventually
     * falls back to the idle (non-contact) state. */
    if (g_CollStateDbg.hold > 0)
        g_CollStateDbg.hold--;
    s_coll_count = n;
}

/* Build the collision panel texture from s_coll_lines (line 0 at top). Tinted
 * green to distinguish it from the white console. Binds s_coll_tex. */
static void coll_build_texture(void)
{
    static unsigned char pixels[COLL_TEX_H][COLL_TEX_W][4];
    int line, cx, x, y;

    memset(pixels, 0, sizeof(pixels));

    for (line = 0; line < s_coll_count; line++) {
        const char* str = s_coll_lines[line];
        for (cx = 0; *str && cx < COLL_COLS; cx++, str++) {
            unsigned int ch = (unsigned char)*str;
            if (ch >= 128) continue;
            for (y = 0; y < GLYPH_H; y++) {
                unsigned char row = s_font[ch][y];
                for (x = 0; x < GLYPH_W; x++) {
                    if (row & (1u << x)) {
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][0] = 130;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][1] = 255;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][2] = 130;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][3] = 255;
                    }
                }
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, s_coll_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, COLL_TEX_W, COLL_TEX_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

/* Fill the anim inspector panel from the player's live model anim state and the
 * K-inspector's forced keyframe. Read-only; the values are the engine's raw
 * fields (status is the packed anim index/active flag, see s_ModelAnim). */
static void anim_gather(void)
{
    s_Model* m  = &g_SysWork.playerWork.extra.model;
    u8       st = m->anim.status;
    int      n  = 0;
#define AL(...) do { if (n < ANIM_LINES) snprintf(s_anim_lines[n++], ANIM_COLS, __VA_ARGS__); } while (0)
    AL("== ANIM (K) ,. / step ==");
    AL("KF %d / %d", g_DebugAnimKf, g_DebugAnimKfMax > 0 ? g_DebugAnimKfMax - 1 : 0);
    /* Which authored anim's keyframe range contains the inspected frame ( / jumps
     * to the next anim's start). HARRY_BASE_ANIM_INFOS playback entries carry the
     * [start,end] range; -1 starts are blend entries (skipped). */
    {
        int i, animIdx = -1, s0 = 0, e0 = 0;
        for (i = 0; i < 256; i++) {
            int sk = HARRY_BASE_ANIM_INFOS[i].startKeyframeIdx;
            int ek = HARRY_BASE_ANIM_INFOS[i].endKeyframeIdx;
            if (sk < 0 || ek < sk) continue;
            if (g_DebugAnimKf >= sk && g_DebugAnimKf <= ek) { animIdx = i; s0 = sk; e0 = ek; break; }
        }
        if (animIdx >= 0) AL("in anim %d [%d-%d]", animIdx, s0, e0);
        else              AL("in anim --  (/ next)");
    }
    AL("anim %d  active %d",
       (int)ANIM_STATUS_IDX_GET(st), (int)(ANIM_STATUS_IS_ACTIVE(st) ? 1 : 0));
    AL("status 0x%02X flg 0x%X", (unsigned)st, (unsigned)m->anim.flags);
    AL("ctrl %d  step %d", (int)m->controlState, (int)m->stateStep);
    AL("upperBody %d", (int)g_SysWork.playerWork.extra.upperBodyState);
    s_anim_count = n;
#undef AL
}

/* Build the anim panel texture from s_anim_lines (line 0 at top). Tinted amber
 * to distinguish it from the green collision panel + the white console. */
static void anim_build_texture(void)
{
    static unsigned char pixels[ANIM_TEX_H][ANIM_TEX_W][4];
    int line, cx, x, y;

    memset(pixels, 0, sizeof(pixels));

    for (line = 0; line < s_anim_count; line++) {
        const char* str = s_anim_lines[line];
        for (cx = 0; *str && cx < ANIM_COLS; cx++, str++) {
            unsigned int ch = (unsigned char)*str;
            if (ch >= 128) continue;
            for (y = 0; y < GLYPH_H; y++) {
                unsigned char row = s_font[ch][y];
                for (x = 0; x < GLYPH_W; x++) {
                    if (row & (1u << x)) {
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][0] = 255;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][1] = 210;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][2] = 110;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][3] = 255;
                    }
                }
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, s_anim_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ANIM_TEX_W, ANIM_TEX_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void score_build_texture(void)
{
    static unsigned char pixels[SCORE_TEX_H][SCORE_TEX_W][4];
    const char* str = s_score_line;
    int cx, x, y;

    memset(pixels, 0, sizeof(pixels));

    for (cx = 0; *str && cx < SCORE_COLS; cx++, str++) {
        unsigned int ch = (unsigned char)*str;
        if (ch >= 128) continue;
        for (y = 0; y < GLYPH_H; y++) {
            unsigned char row = s_font[ch][y];
            for (x = 0; x < GLYPH_W; x++) {
                if (row & (1u << x)) {
                    pixels[y][cx * GLYPH_W + x][0] = 235;
                    pixels[y][cx * GLYPH_W + x][1] = 235;
                    pixels[y][cx * GLYPH_W + x][2] = 235;
                    pixels[y][cx * GLYPH_W + x][3] = 255;
                }
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, s_score_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCORE_TEX_W, SCORE_TEX_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

/* Build the toast texture from s_toast (line 0 at top), white glyphs, with the
 * current fade baked into pixel alpha so the existing SRC_ALPHA blend fades it. */
static void toast_build_texture(int alpha)
{
    static unsigned char pixels[TOAST_TEX_H][TOAST_TEX_W][4];
    int line, cx, x, y;
    unsigned char a = (unsigned char)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));

    memset(pixels, 0, sizeof(pixels));

    for (line = 0; line < s_toast_count && line < TOAST_MAX; line++) {
        const char* str = s_toast[line];
        for (cx = 0; *str && cx < LINE_LEN; cx++, str++) {
            unsigned int ch = (unsigned char)*str;
            if (ch >= 128) continue;
            for (y = 0; y < GLYPH_H; y++) {
                unsigned char row = s_font[ch][y];
                for (x = 0; x < GLYPH_W; x++) {
                    if (row & (1u << x)) {
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][0] = 255;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][1] = 255;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][2] = 255;
                        pixels[line * GLYPH_H + y][cx * GLYPH_W + x][3] = a;
                    }
                }
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, s_toast_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TOAST_TEX_W, TOAST_TEX_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

/* Upload an NDC quad (top y0, bottom y1) for the bound program/VBO and draw it. */
/* Panel quad with a partial UV range: the console texture is sized for the
 * MAXIMUM row count, so the quad crops V to the rows actually in use. */
static void draw_panel_uv(GLuint tex, float x0, float y0, float x1, float y1,
                          float u1, float v1)
{
    float verts[6][4];
    verts[0][0] = x0; verts[0][1] = y0; verts[0][2] = 0.0f; verts[0][3] = 0.0f;
    verts[1][0] = x0; verts[1][1] = y1; verts[1][2] = 0.0f; verts[1][3] = v1;
    verts[2][0] = x1; verts[2][1] = y0; verts[2][2] = u1;   verts[2][3] = 0.0f;
    verts[3][0] = x1; verts[3][1] = y0; verts[3][2] = u1;   verts[3][3] = 0.0f;
    verts[4][0] = x0; verts[4][1] = y1; verts[4][2] = 0.0f; verts[4][3] = v1;
    verts[5][0] = x1; verts[5][1] = y1; verts[5][2] = u1;   verts[5][3] = v1;

    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void draw_panel(GLuint tex, float x0, float y0, float x1, float y1)
{
    draw_panel_uv(tex, x0, y0, x1, y1, 1.0f, 1.0f);
}

/* Project a world-space point (Q12) to NDC via the game's camera: view =
 * VbWvsMatrix * (P - camPos), then PSX perspective (psx = view.xy * H / view.z)
 * mapped to NDC. Returns 0 if behind the camera. halfW is the widescreen ortho
 * half-width (matches the PsyCross Hor+ ortho). */
static void collvis_view(const VECTOR3* cam, s32 wx, s32 wy, s32 wz,
                         float* vx, float* vy, float* vz)
{
    float dx = (float)(wx - cam->vx) / 4096.0f;
    float dy = (float)(wy - cam->vy) / 4096.0f;
    float dz = (float)(wz - cam->vz) / 4096.0f;
    *vx = (VbWvsMatrix.m[0][0] * dx + VbWvsMatrix.m[0][1] * dy + VbWvsMatrix.m[0][2] * dz) / 4096.0f;
    *vy = (VbWvsMatrix.m[1][0] * dx + VbWvsMatrix.m[1][1] * dy + VbWvsMatrix.m[1][2] * dz) / 4096.0f;
    *vz = (VbWvsMatrix.m[2][0] * dx + VbWvsMatrix.m[2][1] * dy + VbWvsMatrix.m[2][2] * dz) / 4096.0f;
}

/* Project a world segment to two NDC points, clipping at the near plane so a
 * segment with one endpoint behind the camera is shortened instead of dropped
 * (the "lines vanish when turning" artifact). Returns 0 if fully behind. */
static int collvis_proj_seg(const VECTOR3* cam, float H, float halfW,
                            s32 ax, s32 ay, s32 az, s32 bx, s32 by, s32 bz,
                            float* nax, float* nay, float* nbx, float* nby)
{
    const float NEARZ = 1.0f;
    float vax, vay, vaz, vbx, vby, vbz;

    collvis_view(cam, ax, ay, az, &vax, &vay, &vaz);
    collvis_view(cam, bx, by, bz, &vbx, &vby, &vbz);

    if (vaz < NEARZ && vbz < NEARZ)
        return 0;

    if (vaz < NEARZ) {
        float t = (NEARZ - vaz) / (vbz - vaz);
        vax += (vbx - vax) * t; vay += (vby - vay) * t; vaz = NEARZ;
    } else if (vbz < NEARZ) {
        float t = (NEARZ - vbz) / (vaz - vbz);
        vbx += (vax - vbx) * t; vby += (vay - vby) * t; vbz = NEARZ;
    }

    *nax =  (vax * H / vaz) / halfW;  *nay = -(vay * H / vaz) / 120.0f;
    *nbx =  (vbx * H / vbz) / halfW;  *nby = -(vby * H / vbz) / 120.0f;
    return 1;
}

/* Draw the captured collision segments as GL lines (no depth test, on top). */
/* ptr_18 cylinder colliders are drawn as boxes of this height (Q12, up = -Y). */
#define CV_CYL_H (5 * 4096)

static void collvis_render_lines(void)
{
    static float verts[CV_VERT_CAP * 5];
    VECTOR3 cam;
    float   H, halfW;
    int     i, nv = 0;

    if (s_cvSegCount == 0 && s_cvCylCount == 0 && s_cvHitCount == 0 && s_cvHitCylCount == 0)
        return;

    vcGetNowCamPos(&cam);
    H = (float)ReadGeomScreen();
    if (H < 1.0f)
        return;
    {
        const float psxAspect = 320.0f / 240.0f;
        const float winAspect = g_PcConfig.windowHeight > 0
            ? (float)g_PcConfig.windowWidth / (float)g_PcConfig.windowHeight : psxAspect;
        halfW = 160.0f * (winAspect / psxAspect);
    }

#define CV_PUSH_LINE(r,g,b, X0,Y0,Z0, X1,Y1,Z1) do {                              \
        float _pa, _pb, _pc, _pd;                                                 \
        if (nv + 10 <= CV_VERT_CAP * 5 &&                                         \
            collvis_proj_seg(&cam, H, halfW, (X0),(Y0),(Z0), (X1),(Y1),(Z1),      \
                             &_pa, &_pb, &_pc, &_pd)) {                           \
            verts[nv++]=_pa; verts[nv++]=_pb; verts[nv++]=(r); verts[nv++]=(g); verts[nv++]=(b); \
            verts[nv++]=_pc; verts[nv++]=_pd; verts[nv++]=(r); verts[nv++]=(g); verts[nv++]=(b); \
        } } while (0)

    /* Cached cell surface geometry (green, flat). */
    for (i = 0; i < s_cvSegCount; i++) {
        CV_PUSH_LINE(0.2f, 1.0f, 0.3f, s_cvSegs[i].ax, s_cvSegs[i].ay, s_cvSegs[i].az,
                                       s_cvSegs[i].bx, s_cvSegs[i].by, s_cvSegs[i].bz);
    }

    /* Per-frame contacted faces (red, on top). */
    for (i = 0; i < s_cvHitCount; i++) {
        CV_PUSH_LINE(1.0f, 0.15f, 0.15f, s_cvHits[i].ax, s_cvHits[i].ay, s_cvHits[i].az,
                                         s_cvHits[i].bx, s_cvHits[i].by, s_cvHits[i].bz);
    }

    /* Cylinder colliders (trees/poles) as cyan boxes, half-extent = radius,
     * standing on the floor (the collider's own Y isn't the ground). */
    for (i = 0; i < s_cvCylCount; i++) {
        s32 cx = s_cvCyls[i].cx, cz = s_cvCyls[i].cz, r = s_cvCyls[i].r;
        s32 fy = s_cvFloorY;            /* box base = floor */
        s32 ty = s_cvFloorY - CV_CYL_H; /* box top  = floor + height (up = -Y) */
        s32 cxn[4], czn[4], k;
        cxn[0] = cx - r; czn[0] = cz - r;
        cxn[1] = cx + r; czn[1] = cz - r;
        cxn[2] = cx + r; czn[2] = cz + r;
        cxn[3] = cx - r; czn[3] = cz + r;
        for (k = 0; k < 4; k++) {
            int k1 = (k + 1) & 3;
            CV_PUSH_LINE(0.2f,0.8f,1.0f, cxn[k],fy,czn[k], cxn[k1],fy,czn[k1]);  /* base */
            CV_PUSH_LINE(0.2f,0.8f,1.0f, cxn[k],ty,czn[k], cxn[k1],ty,czn[k1]);  /* top  */
            CV_PUSH_LINE(0.2f,0.8f,1.0f, cxn[k],fy,czn[k], cxn[k],ty,czn[k]);    /* vert */
        }
    }

    /* The actual blocker this frame (red box) — obstacle or NPC cylinder that
     * stopped Harry, captured at the collision point regardless of which cell. */
    for (i = 0; i < s_cvHitCylCount; i++) {
        s32 cx = s_cvHitCyls[i].cx, cz = s_cvHitCyls[i].cz, r = s_cvHitCyls[i].r;
        s32 fy = s_cvFloorY;
        s32 ty = s_cvFloorY - CV_CYL_H;
        s32 cxn[4], czn[4], k;
        cxn[0] = cx - r; czn[0] = cz - r;
        cxn[1] = cx + r; czn[1] = cz - r;
        cxn[2] = cx + r; czn[2] = cz + r;
        cxn[3] = cx - r; czn[3] = cz + r;
        for (k = 0; k < 4; k++) {
            int k1 = (k + 1) & 3;
            CV_PUSH_LINE(1.0f,0.1f,0.1f, cxn[k],fy,czn[k], cxn[k1],fy,czn[k1]);
            CV_PUSH_LINE(1.0f,0.1f,0.1f, cxn[k],ty,czn[k], cxn[k1],ty,czn[k1]);
            CV_PUSH_LINE(1.0f,0.1f,0.1f, cxn[k],fy,czn[k], cxn[k],ty,czn[k]);
        }
    }

#undef CV_PUSH_LINE

    if (nv == 0)
        return;

    glUseProgram(s_line_prog);
    glBindVertexArray(s_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_line_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv * sizeof(float), verts);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, nv / 5);
}

void DbgOverlay_PushLine(const char* line)
{
    push_console(line);
    s_console_dirty = 1;
}

/* Like DbgOverlay_PushLine, but the line is also eligible for the top-left toast
 * when the console overlay is hidden. Only SH_DBG_ECHO routes here (user-facing
 * feedback: setting changes, debug keys) — SH_LOG/SH_WARN and console-command
 * output use DbgOverlay_PushLine so system logging never shows as a toast. */
void DbgOverlay_ToastLine(const char* line)
{
    push_console(line);
    s_console_dirty = 1;

    /* Skipped while the console is open (it already displays the line) and until
     * MainLoop arms the toast (drops the boot-time init logging). */
    if (s_toast_armed && !s_console_open) {
        Uint32 now = SDL_GetTicks();
        if (s_toast_count == 0 || (now - s_toast_last_ms) > TOAST_HOLD_MS) {
            s_toast_count = 0; /* previous cluster has settled -> start fresh */
        } else if (s_toast_count >= TOAST_MAX) {
            int i;
            for (i = 0; i < TOAST_MAX - 1; i++)
                memcpy(s_toast[i], s_toast[i + 1], LINE_LEN);
            s_toast_count = TOAST_MAX - 1;
        }
        strncpy(s_toast[s_toast_count], line, LINE_LEN - 1);
        s_toast[s_toast_count][LINE_LEN - 1] = '\0';
        s_toast_count++;
        s_toast_last_ms = now;
    }
}

/* Auto-repeat with acceleration for held keys (mirrors game_main.c's keyframe
 * scrubber Kf_HoldRepeat): fires on the press edge, then after a short delay
 * repeats at an accelerating rate. pressMs/lastMs are per-key timers. */
static int Dbg_HoldRepeat(int cur, int prev, Uint32* pressMs, Uint32* lastMs)
{
    Uint32 now = SDL_GetTicks();
    if (cur && !prev) { *pressMs = now; *lastMs = now; return 1; }
    if (cur && prev) {
        Uint32 held = now - *pressMs;
        if (held >= 350) {
            int interval = 250 - (int)((held - 350) / 10);
            if (interval < 100) interval = 100;
            if ((now - *lastMs) >= (Uint32)interval) { *lastMs = now; return 1; }
        }
    }
    return 0;
}

/* Resolve a graphics-tuning key bind (key_gfx_*) to "active this frame". Handles
 * both a keyboard scancode name AND a mouse-wheel bind ("MouseWheelUp/Down"),
 * so the wheel can drive the effect cycle/adjust — SDL_GetScancodeFromName alone
 * returns UNKNOWN for wheel names, which is why a wheel bind did nothing. */
static int Dbg_GfxBindActive(const unsigned char* ks, const char* name)
{
    extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
    SDL_Scancode sc;
    if (!name || !name[0]) return 0;

    /* An open console OWNS the mouse and keyboard: the wheel is its scrollback
     * control and the keys are its prompt, so neither may also fire whatever
     * action they happen to be bound to. Typing the default `[` / `]` binds used
     * to adjust the flashlight WHILE entering them as text, and a wheel notch
     * both scrolled and fired.
     *
     * Gating the resolver itself covers every bind that routes through it
     * (gfx prev/next/cycle, Exit Game) for both input kinds at once. The other
     * two wheel consumers are already handled elsewhere: wheel-bound PSX buttons
     * die when MainLoop zeroes the pad, and the menu mouse cursor gates on
     * Pc_ConsoleIsOpen. */
    if (s_console_open) return 0;

    if (SDL_strcasecmp(name, "MouseWheelUp")   == 0) return g_PsyX_WheelUpFrames   != 0;
    if (SDL_strcasecmp(name, "MouseWheelDown") == 0) return g_PsyX_WheelDownFrames != 0;
    sc = SDL_GetScancodeFromName(name);
    return (sc != SDL_SCANCODE_UNKNOWN) ? ks[sc] : 0;
}

void DbgOverlay_Update(void)
{
    static int s_mark_a = 0;
    static int s_mark_b = 0;
    static int s_prev_tilde = 0;
    VECTOR3 hpos, cpos;
    s_SubCharacter* player;
    int cur_a, cur_b, cur_tilde;
    char line[LINE_LEN];

    extern int g_PcAllowDebugControls;

    const unsigned char* ks = SDL_GetKeyboardState(NULL);
    if (!ks) return;

    /* First Update runs inside MainLoop — arm the top-left message toast now so
     * the boot-time init logging (pushed before MainLoop) is never toasted. */
    s_toast_armed = 1;

    /* `~` toggles the console (debug builds only): one press opens the panel
     * with the prompt live and the game frozen; the next press closes it and
     * restores gameplay. No hold, no separate view/input states, no config
     * bit for the ingame overlay (show_console now only controls the EXTERNAL
     * console window). */
    /* Console toggle key is configurable (key_console; default tilde "`") so layouts
     * that can't reach tilde can rebind it. Resolved once: empty falls back to tilde
     * (old configs keep working); "NONE" yields UNKNOWN = keyboard-unbindable. */
    {
        static SDL_Scancode s_consoleSc = SDL_SCANCODE_GRAVE;
        static int          s_consoleScResolved = 0;
        if (!s_consoleScResolved) {
            s_consoleSc = (g_PcConfig.keyConsole[0] == '\0')
                            ? SDL_SCANCODE_GRAVE
                            : SDL_GetScancodeFromName(g_PcConfig.keyConsole);
            s_consoleScResolved = 1;
        }
        cur_tilde = ks[s_consoleSc];
    }
    if (g_PcAllowDebugControls) {
        if (cur_tilde && !s_prev_tilde) { /* press edge = toggle */
            s_console_open = !s_console_open;
            s_console_dirty = 1;
            if (s_console_open) {
                g_PcConsoleInputActive = 1;
                s_input_len            = 0;
                s_input_buf[0]         = '\0';
                s_scroll               = 0;
                s_console_apply_until  = 0;
            } else {
                g_PcConsoleInputActive  = 0;
                g_PcConsoleSwallowInput = 1; /* don't leak the toggle press */
                s_console_apply_until   = 0;
            }
        }
    }
    s_prev_tilde = cur_tilde;

    /* Console apply window: while it runs the game is unfrozen (so the last
     * command's effect shows) and pad input is suppressed; when it elapses, drop
     * back into input mode if the console is still open. */
    if (s_console_apply_until) {
        if (SDL_GetTicks() < s_console_apply_until) {
            g_PcConsoleSwallowInput = 1;
        } else {
            s_console_apply_until = 0;
            if (s_console_open) {
                g_PcConsoleInputActive = 1;
                s_console_dirty        = 1;
            }
        }
    }

    /* Scrollback while open: PgUp/PgDn (with hold-repeat) and the mouse wheel.
     * End jumps back to live. Clamped against the backlog in the texture build. */
    if (s_console_open) {
        extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
        static Uint32 s_pgupPress = 0, s_pgupLast = 0;
        static Uint32 s_pgdnPress = 0, s_pgdnLast = 0;
        int step = (s_vis_rows > 8) ? (s_vis_rows / 2) : 4;

        if (Dbg_HoldRepeat(ks[SDL_SCANCODE_PAGEUP], s_prev_keys[SDL_SCANCODE_PAGEUP],
                           &s_pgupPress, &s_pgupLast)) {
            s_scroll += step;
            s_console_dirty = 1;
        }
        if (Dbg_HoldRepeat(ks[SDL_SCANCODE_PAGEDOWN], s_prev_keys[SDL_SCANCODE_PAGEDOWN],
                           &s_pgdnPress, &s_pgdnLast)) {
            s_scroll -= step;
            if (s_scroll < 0) s_scroll = 0;
            s_console_dirty = 1;
        }
        if (ks[SDL_SCANCODE_END] && !s_prev_keys[SDL_SCANCODE_END]) {
            s_scroll = 0;
            s_console_dirty = 1;
        }
        if (g_PsyX_WheelUpFrames)   { s_scroll += 3; s_console_dirty = 1; }
        if (g_PsyX_WheelDownFrames) { s_scroll -= 3; if (s_scroll < 0) s_scroll = 0; s_console_dirty = 1; }

        /* Mouse: click-drag selects text. Only once the panel is at rest —
         * the glyph grid is fixed 16px cells from the viewport's top-left
         * (see Render), so hit-testing is a straight divide. Columns round
         * to the nearest character boundary like a text editor. */
        if (s_hit_valid && s_console_slide >= 1.0f) {
            int    wx, wy;
            Uint32 mb   = SDL_GetMouseState(&wx, &wy);
            int    lmb  = (mb & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
            int    vpTop = g_windowHeight - (s_hit_vp[1] + s_hit_vp[3]);
            int    px   = wx - s_hit_vp[0];
            int    py   = wy - vpTop;
            int    cw   = GLYPH_W * SCALE;
            int    chh  = GLYPH_H * SCALE;
            int    line = -2; /* -2 = not over console text */
            int    col  = 0;

            if (px >= 0 && py >= 0) {
                int row = py / chh;
                col = (px + cw / 2) / cw;
                if (col > CON_COLS) col = CON_COLS;
                if (row == s_vis_rows) {
                    line = -1; /* prompt row */
                } else if (row >= 0 && row < s_vis_rows &&
                           !(s_scroll > 0 && row == 0)) { /* row 0 = scroll marker */
                    int idx = s_scroll + (s_vis_rows - 1 - row);
                    if (idx >= 0 && idx < s_console_count)
                        line = idx;
                }
            }

            if (lmb && !s_prev_lmb) {
                if (line != -2) {
                    s_sel_a_line = s_sel_b_line = line;
                    s_sel_a_col  = s_sel_b_col  = col;
                    s_sel_valid  = 1;
                    s_sel_active = 1;
                } else {
                    s_sel_valid = 0; /* click off the text clears it */
                }
            } else if (lmb && s_sel_active) {
                if (line != -2) {
                    s_sel_b_line = line;
                    s_sel_b_col  = col;
                }
            } else {
                s_sel_active = 0;
            }
            s_prev_lmb = lmb;
        }
    }

    /* Interactive input: A-Z and 0-9 append, Backspace deletes, Enter
     * submits. Edge-detected against s_prev_keys. The submitted command is
     * echoed half-life style; `quit` exits the game, anything else prints
     * "Command not found!". Submitting or tapping `~` unpauses. */
    if (g_PcConsoleInputActive) {
        int sc;
        int ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];

        /* Ctrl+C copies the click-drag selection to the system clipboard;
         * Ctrl+V appends the clipboard's first line to the prompt. Chorded
         * keys must not ALSO type — the character loop below is Ctrl-gated. */
        if (ctrl && ks[SDL_SCANCODE_C] && !s_prev_keys[SDL_SCANCODE_C])
            Console_CopySelection();
        if (ctrl && ks[SDL_SCANCODE_V] && !s_prev_keys[SDL_SCANCODE_V])
            Console_Paste();

        if (!ctrl)
        for (sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_0; sc++) { /* A..Z, 1..9, 0 are contiguous */
            if (ks[sc] && !s_prev_keys[sc]) {
                char c;
                if (sc <= SDL_SCANCODE_Z)
                    c = (char)('A' + (sc - SDL_SCANCODE_A));
                else if (sc == SDL_SCANCODE_0)
                    c = '0';
                else
                    c = (char)('1' + (sc - SDL_SCANCODE_1));
                if (s_input_len < INPUT_BUF_CAP - 1) {
                    s_input_buf[s_input_len++] = c;
                    s_input_buf[s_input_len]   = '\0';
                    s_console_dirty            = 1;
                }
            }
        }
        /* Space separates command from argument; `-` types `_` (map and FMV
         * names use underscores, and the console has no shift handling). */
        if (ks[SDL_SCANCODE_SPACE] && !s_prev_keys[SDL_SCANCODE_SPACE] &&
            s_input_len > 0 && s_input_len < INPUT_BUF_CAP - 1) {
            s_input_buf[s_input_len++] = ' ';
            s_input_buf[s_input_len]   = '\0';
            s_console_dirty            = 1;
        }
        /* `-`/`_`, `=`/`+`, `.` for numeric and path args (e.g. `weld 2.5`,
         * `inveqy -50`, map names with `_`). Shift gives the upper glyph; there's
         * no full shift handling, just these three keys. */
        {
            int shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
            const struct { int sc; char lo, hi; } syms[] = {
                { SDL_SCANCODE_MINUS,  '-', '_' },
                { SDL_SCANCODE_EQUALS, '=', '+' },
                { SDL_SCANCODE_PERIOD, '.', '.' },
            };
            int i;
            for (i = 0; i < 3; i++) {
                if (ks[syms[i].sc] && !s_prev_keys[syms[i].sc] &&
                    s_input_len < INPUT_BUF_CAP - 1) {
                    s_input_buf[s_input_len++] = shift ? syms[i].hi : syms[i].lo;
                    s_input_buf[s_input_len]   = '\0';
                    s_console_dirty            = 1;
                }
            }
        }
        /* Backspace hold-repeats like a text editor: one delete on the press
         * edge, then after a short delay a steady 25/sec while held (faster
         * than Dbg_HoldRepeat's 100ms floor, which felt sluggish for text). */
        {
            static Uint32 s_bsPress = 0, s_bsLast = 0;
            int del = 0;

            if (ks[SDL_SCANCODE_BACKSPACE]) {
                Uint32 now = SDL_GetTicks();
                if (!s_prev_keys[SDL_SCANCODE_BACKSPACE]) {
                    s_bsPress = now;
                    s_bsLast  = now;
                    del       = 1;
                } else if (now - s_bsPress >= 350 && now - s_bsLast >= 40) {
                    s_bsLast = now;
                    del      = 1;
                }
            }
            if (del && s_input_len > 0) {
                s_input_buf[--s_input_len] = '\0';
                s_console_dirty            = 1;
            }
        }
        /* Up / Down: recall recently entered commands (most-recent first). */
        if (ks[SDL_SCANCODE_UP] && !s_prev_keys[SDL_SCANCODE_UP] && s_hist_count > 0) {
            if (s_hist_nav < 0) {  /* park the live edit before browsing into history */
                strncpy(s_hist_edit, s_input_buf, INPUT_BUF_CAP - 1);
                s_hist_edit[INPUT_BUF_CAP - 1] = '\0';
            }
            if (s_hist_nav < s_hist_count - 1) { Console_LoadHist(++s_hist_nav); s_console_dirty = 1; }
        }
        if (ks[SDL_SCANCODE_DOWN] && !s_prev_keys[SDL_SCANCODE_DOWN] && s_hist_nav >= 0) {
            if (s_hist_nav > 0) {
                Console_LoadHist(--s_hist_nav);
            } else {  /* stepped past the newest -> restore the parked live edit */
                s_hist_nav  = -1;
                strncpy(s_input_buf, s_hist_edit, INPUT_BUF_CAP - 1);
                s_input_buf[INPUT_BUF_CAP - 1] = '\0';
                s_input_len = (int)strlen(s_input_buf);
            }
            s_console_dirty = 1;
        }
        if (ks[SDL_SCANCODE_RETURN] && !s_prev_keys[SDL_SCANCODE_RETURN]) {
            if (s_input_len > 0) {
                extern void Pc_ConsoleExec(const char* line);
                char echo[LINE_LEN];
                snprintf(echo, LINE_LEN, "> %s", s_input_buf);
                push_console(echo);
                Pc_ConsoleExec(s_input_buf);
                /* Save to history (skip an immediate duplicate of the most recent). */
                {
                    int prev = (s_hist_write - 1 + CONSOLE_HIST_MAX) % CONSOLE_HIST_MAX;
                    if (s_hist_count == 0 || strcmp(s_hist[prev], s_input_buf) != 0) {
                        strncpy(s_hist[s_hist_write], s_input_buf, INPUT_BUF_CAP - 1);
                        s_hist[s_hist_write][INPUT_BUF_CAP - 1] = '\0';
                        s_hist_write = (s_hist_write + 1) % CONSOLE_HIST_MAX;
                        if (s_hist_count < CONSOLE_HIST_MAX) s_hist_count++;
                    }
                }
                s_hist_nav = -1;
                /* Keep the console OPEN: clear the line, snap the view back to
                 * live, and briefly unfreeze so the command's effect renders/
                 * animates, then drop back into input mode (apply-window check
                 * at the top of the function). Lets the user run several
                 * commands without reopening. */
                s_input_len    = 0;
                s_input_buf[0] = '\0';
                s_scroll       = 0;
                g_PcConsoleInputActive  = 0;          /* unfreeze for the window */
                g_PcConsoleSwallowInput = 1;          /* don't leak this Enter   */
                s_console_apply_until   = SDL_GetTicks() + CONSOLE_APPLY_MS;
            }
            s_console_dirty = 1;
        }
    }
    {
        int sc;
        for (sc = 0; sc < 128; sc++)
            s_prev_keys[sc] = ks[sc];
    }

    /* The swallow is released in MainLoop (game_main.c), next to the input
     * suppression: it must also see the PARSED pad state, which lags the SDL
     * array here by a frame — clearing on SDL alone leaked the submit Enter
     * as a Start click. */

    /* Ease the slide toward visible (console open) or hidden. Runs every
     * frame regardless of state so the console can animate back out. */
    {
        float target = s_console_open ? 1.0f : 0.0f;
        if (s_console_slide < target) {
            s_console_slide += CONSOLE_SLIDE_STEP;
            if (s_console_slide > target) s_console_slide = target;
        } else if (s_console_slide > target) {
            s_console_slide -= CONSOLE_SLIDE_STEP;
            if (s_console_slide < target) s_console_slide = target;
        }
    }

    /* `'` toggles the collision visualizer panel; gather data each frame while
     * on. Handled before the showConsole gate so it works independently of the
     * scrolling console's visibility. */
    {
        int cur_apos = ks[SDL_SCANCODE_APOSTROPHE];
        if (cur_apos && !s_prev_apos && g_PcAllowDebugControls) {
            s_coll_on = !s_coll_on;
            g_CollVisEnabled = s_coll_on;
            SH_DBG_ECHO("[DEBUG] ' Collision visualizer: %s", s_coll_on ? "ON" : "OFF");
        }
        s_prev_apos = cur_apos;
        if (s_coll_on)
            coll_gather();
    }

    /* Anim inspector panel: gather live data while the K keyframe inspector is on
     * (the K / , / . keys themselves are handled in game_main.c's DebugCamera_Update
     * next to the other debug toggles). */
    if (g_DebugAnimKfView)
        anim_gather();

    /* F1 toggles PGXP (perspective-correct rendering) on the fly. PGXP is a
     * user-facing graphics option, not a debug feature, so it is NOT gated on
     * g_PcAllowDebugControls — F1 always works. */
    {
        static int s_prev_f1 = 0;
        int cur_f1 = ks[SDL_SCANCODE_F1];
        if (cur_f1 && !s_prev_f1) {
            extern int g_PsxUsePgxp;
            g_PsxUsePgxp = !g_PsxUsePgxp;
            g_PcConfig.usePgxp = g_PsxUsePgxp ? 1 : 0;
            PcConfig_SaveKeyValue("use_pgxp", g_PsxUsePgxp ? "1" : "0");
            SH_DBG_ECHO("F1 PGXP: %s", g_PsxUsePgxp ? "ON" : "OFF");
        }
        s_prev_f1 = cur_f1;
    }

    /* F2 cycles the full-screen post-process look (0=Off..8). Like F1/PGXP this
     * is a user-facing graphics option, not a debug feature, so it is NOT gated
     * on g_PcAllowDebugControls. Order must match the post fragment shader. */
    {
        static int s_prev_f2 = 0;
        int cur_f2 = ks[SDL_SCANCODE_F2];
        if (cur_f2 && !s_prev_f2) {
            extern int g_cfg_postProcess;
            static const char* const s_postNames[] = {
                "Off", "CRT", "Scanlines", "Vignette", "Color Grade",
                "Film Grain", "Sharpen", "PSX Retro", "Cinematic"
            };
            const int count = (int)(sizeof(s_postNames) / sizeof(s_postNames[0]));
            g_cfg_postProcess = (g_cfg_postProcess + 1) % count;
            if (g_cfg_postProcess < 0) g_cfg_postProcess = 0;
            g_PcConfig.postProcess = g_cfg_postProcess;
            { char b[8]; snprintf(b, sizeof(b), "%d", g_cfg_postProcess); PcConfig_SaveKeyValue("post_process", b); }
            SH_DBG_ECHO("F2 Post-process: %s", s_postNames[g_cfg_postProcess]);
        }
        s_prev_f2 = cur_f2;
    }

    /* F3 cycles the tone-map operator (0=Off..3). User-facing graphics option,
     * not gated on g_PcAllowDebugControls. (PsyCross's old F3 bilinear toggle was
     * removed to free this key — filtering is set via the launcher.) */
    {
        static int s_prev_f3 = 0;
        int cur_f3 = ks[SDL_SCANCODE_F3];
        if (cur_f3 && !s_prev_f3) {
            extern int g_cfg_tonemap;
            static const char* const s_toneNames[] = { "Off", "Reinhard", "ACES", "Filmic" };
            const int count = (int)(sizeof(s_toneNames) / sizeof(s_toneNames[0]));
            g_cfg_tonemap = (g_cfg_tonemap + 1) % count;
            if (g_cfg_tonemap < 0) g_cfg_tonemap = 0;
            g_PcConfig.tonemap = g_cfg_tonemap;
            { char b[8]; snprintf(b, sizeof(b), "%d", g_cfg_tonemap); PcConfig_SaveKeyValue("tonemap", b); }
            SH_DBG_ECHO("F3 Tone mapping: %s", s_toneNames[g_cfg_tonemap]);
        }
        s_prev_f3 = cur_f3;
    }

    /* F4 cycles the flashlight mode: Classic -> Classic + Shadows -> Modern ->
     * Modern + Shadows. */
    {
        static int s_prev_f4 = 0;
        int cur_f4 = ks[SDL_SCANCODE_F4];
        if (cur_f4 && !s_prev_f4) {
            Pc_FlashlightModeApply((g_PcConfig.flashlightMode + 1) & 3, 1);
            SH_DBG_ECHO("F4 Flashlight: %s",
                        Pc_FlashlightModeLabel(g_PcConfig.flashlightMode));
        }
        s_prev_f4 = cur_f4;
    }

    /* Exit Game bind (default Escape) is NOT a debug control — always handled,
     * in every state. At the title / main menu it quits the game; otherwise
     * (gameplay or cutscene) it warm-reboots to the title (consumed by
     * MainLoop_ShouldWarmReset, which ignores it on the menu / during boot).
     * Suppressed only while typing in the console. User-bindable (launcher:
     * Keyboard Controls); "NONE"/unbound disables it entirely. */
    {
        static int s_prev_exit = 0;
        int cur_exit = Dbg_GfxBindActive(ks, g_PcConfig.keyExitGame);
        if (cur_exit && !s_prev_exit && !g_PcConsoleInputActive) {
            if (g_GameWork.gameState == GameState_MainMenu) {
                exit(0);
            } else {
                g_SysWork.sysFlags |= SysFlag_DoWarmReset;
                SH_DBG_ECHO("Exiting to title...");
            }
        }
        s_prev_exit = cur_exit;
    }

    /* ---- Live effect-intensity control (works during gameplay) -------------
     * [ lowers / ] raises the selected effect's intensity (auto-repeat
     * accelerates while held, like the keyframe inspector); \ cycles which
     * ENABLED effect is being adjusted. The value is echoed to the console and
     * saved to config on key release (avoids file I/O during a held repeat). */
    {
        extern int   g_PsyX_UsePerPixelFlashlight, g_cfg_postProcess, g_cfg_tonemap;
        extern int   g_PcFpsCam;
        extern float g_PsyX_FlashlightIntensity, g_cfg_postProcessIntensity, g_cfg_tonemapIntensity;
        extern float g_PsyX_FlashlightSize;
        extern float g_PsyX_FlashlightIntensityFps, g_PsyX_FlashlightSizeFps;
        extern void  PcConfig_SaveKeyValue(const char* key, const char* val);

        static int    s_fxSel   = 0; /* 0=flashlight 1=post 2=tonemap 3=flashlight size */
        static int    s_prev_bs = 0;
        static Uint32 s_lbP = 0, s_lbL = 0, s_rbP = 0, s_rbL = 0;

        int    en[4];
        float* vp[4];
        float  vmax[4];
        const char* vkey[4];
        const char* vlbl[4];
        int    curBS, anyEn, changed, n;

        /* In FPS mode, [ / ] tune the separate FPS flashlight set (dimmer/tighter). */
        en[0] = (g_PsyX_UsePerPixelFlashlight != 0); vp[0] = g_PcFpsCam ? &g_PsyX_FlashlightIntensityFps : &g_PsyX_FlashlightIntensity; vmax[0] = 3.0f; vkey[0] = g_PcFpsCam ? "flashlight_intensity_fps" : "flashlight_intensity"; vlbl[0] = g_PcFpsCam ? "flashlight (fps)" : "flashlight";
        en[1] = (g_cfg_postProcess != 0);            vp[1] = &g_cfg_postProcessIntensity; vmax[1] = 1.0f; vkey[1] = "post_process_intensity"; vlbl[1] = "post-process";
        en[2] = (g_cfg_tonemap != 0);                vp[2] = &g_cfg_tonemapIntensity;     vmax[2] = 1.0f; vkey[2] = "tonemap_intensity";      vlbl[2] = "tonemap";
        en[3] = (g_PsyX_UsePerPixelFlashlight != 0); vp[3] = g_PcFpsCam ? &g_PsyX_FlashlightSizeFps : &g_PsyX_FlashlightSize; vmax[3] = 3.0f; vkey[3] = g_PcFpsCam ? "flashlight_size_fps" : "flashlight_size"; vlbl[3] = g_PcFpsCam ? "flashlight size (fps)" : "flashlight size";

        /* Keys are user-bindable (launcher: Keyboard Controls). Defaults [ / ] /
         * \ ; resolved from config each frame so a rebind takes effect live. A
         * keyboard key OR a mouse-wheel bind works; unbound ("NONE"/blank) never
         * fires. Each wheel notch = one adjust step (the latch is ~2 frames, so
         * the edge/hold-repeat consumers below fire once per notch). */
        cur_a = Dbg_GfxBindActive(ks, g_PcConfig.keyGfxPrev);
        cur_b = Dbg_GfxBindActive(ks, g_PcConfig.keyGfxNext);
        curBS = Dbg_GfxBindActive(ks, g_PcConfig.keyGfxCycle);
        anyEn = en[0] || en[1] || en[2] || en[3];

        /* keep the selection on an enabled effect */
        if (anyEn && !en[s_fxSel]) { n = 0; do { s_fxSel = (s_fxSel + 1) % 4; } while (!en[s_fxSel] && ++n < 4); }

        /* \ : switch to the next enabled effect */
        if (curBS && !s_prev_bs && anyEn) {
            n = 0; do { s_fxSel = (s_fxSel + 1) % 4; } while (!en[s_fxSel] && ++n < 4);
            SH_DBG_ECHO("[FX] adjusting %s (%.2f)  ([ lower / ] raise)", vlbl[s_fxSel], *vp[s_fxSel]);
        }
        s_prev_bs = curBS;

        changed = 0;
        if (anyEn && en[s_fxSel]) {
            if (Dbg_HoldRepeat(cur_a, s_prev_a, &s_lbP, &s_lbL)) {
                *vp[s_fxSel] -= 0.05f; if (*vp[s_fxSel] < 0.0f) *vp[s_fxSel] = 0.0f; changed = 1;
            }
            if (Dbg_HoldRepeat(cur_b, s_prev_b, &s_rbP, &s_rbL)) {
                *vp[s_fxSel] += 0.05f; if (*vp[s_fxSel] > vmax[s_fxSel]) *vp[s_fxSel] = vmax[s_fxSel]; changed = 1;
            }
            if (changed)
                SH_DBG_ECHO("[FX] %s %.2f", vlbl[s_fxSel], *vp[s_fxSel]);
            if ((!cur_a && s_prev_a) || (!cur_b && s_prev_b)) { /* persist the landed value */
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", *vp[s_fxSel]);
                PcConfig_SaveKeyValue(vkey[s_fxSel], buf);
            }
        }
        s_prev_a = cur_a;
        s_prev_b = cur_b;
    }
}

void DbgOverlay_Render(void)
{
    GLint   vp[4];
    GLint   prev_prog, prev_tex, prev_vao, prev_vbo, prev_fb;
    GLint   prev_active_tex, prev_blend_src, prev_blend_dst;
    GLint   prev_blend_eq_rgb, prev_blend_eq_a;
    GLboolean prev_depth, prev_blend;
    int     drawConsole, drawColl, drawAnim, drawToast;
    int     toastAlpha = 0;

    /* Console is hidden once fully slid off-screen (toggled by `~`); the ring
     * buffer keeps filling while hidden. The collision panel draws whenever it's
     * toggled on (`'`), independent of the console. The anim panel draws while the
     * K keyframe inspector is on. */
    drawConsole = (s_console_slide > 0.0f && (s_console_count > 0 || s_console_open));
    drawColl    = (s_coll_on && s_coll_count > 0);
    drawAnim    = (g_DebugAnimKfView && s_anim_count > 0);

    /* Randomizer score: gamemode HUD, so it is gated on a live run, not on the
     * debug-controls flag the panels above use. */
    {
        extern int Pc_Rando_ScoreLine(char* buf, int cap);
        s_score_on = Pc_Rando_ScoreLine(s_score_line, sizeof(s_score_line));
    }

    /* Toast: newest hidden-console line(s) (SH_DBG_ECHO only), fading out.
     * Suppressed while the console panel itself is drawing / open. */
    drawToast = 0;
    if (s_toast_count > 0 && !drawConsole && !s_console_open) {
        Uint32 age = SDL_GetTicks() - s_toast_last_ms;
        if (age < TOAST_FADE_IN_MS)
            toastAlpha = (int)(255u * age / TOAST_FADE_IN_MS);
        else if (age < TOAST_HOLD_MS)
            toastAlpha = 255;
        else if (age < TOAST_HOLD_MS + TOAST_FADE_MS)
            toastAlpha = 255 - (int)(255u * (age - TOAST_HOLD_MS) / TOAST_FADE_MS);
        drawToast = (toastAlpha > 0);
    }

    if (!drawConsole && !drawColl && !s_coll_on && !drawAnim && !drawToast && !s_score_on) return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] == 0 || vp[3] == 0) return;

    /* Save ALL state before making any changes. */
    glGetIntegerv(GL_CURRENT_PROGRAM,      &prev_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE,       &prev_active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,   &prev_tex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,  &prev_fb);
    glGetIntegerv(GL_BLEND_SRC_RGB,        &prev_blend_src);
    glGetIntegerv(GL_BLEND_DST_RGB,        &prev_blend_dst);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &prev_blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prev_blend_eq_a);
    prev_depth = glIsEnabled(GL_DEPTH_TEST);
    prev_blend = glIsEnabled(GL_BLEND);

    if (!s_gl_inited)
        overlay_gl_init();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Force normal additive blending. The cutscene letterbox leaves the blend
     * equation as GL_FUNC_REVERSE_SUBTRACT (how its white bars darken); without
     * this the overlay text/lines would subtract from the scene and render black
     * during cutscenes. Restored below. */
    glBlendEquation(GL_FUNC_ADD);

    glUseProgram(s_prog);
    glUniform1i(s_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    if (drawConsole) {
        /* Quake-style drop-down: full window width, ~half the window height
         * (as many text rows as fit, capped by the texture), sliding down from
         * above the top edge. */
        int rowsFit = (int)((0.5f * (float)vp[3]) / (float)(GLYPH_H * SCALE)) - 1;
        float usedRows, hNdc, x0, y0, x1, y1, slideOfs, tx1, tv1;

        if (rowsFit < 4)               rowsFit = 4;
        if (rowsFit > CONSOLE_VIS_MAX) rowsFit = CONSOLE_VIS_MAX;
        if (rowsFit != s_vis_rows) {
            s_vis_rows      = rowsFit;
            s_console_dirty = 1;
        }

        usedRows = (float)(s_vis_rows + 1); /* + prompt row */
        hNdc     = 2.0f * (usedRows * (float)(GLYPH_H * SCALE)) / (float)vp[3];
        x0 = -1.0f;
        x1 =  1.0f;
        y0 =  1.0f;
        y1 =  1.0f - hNdc;
        slideOfs = (1.0f - s_console_slide) * hNdc;
        y0 += slideOfs;
        y1 += slideOfs;

        glBindTexture(GL_TEXTURE_2D, s_tex);
        if (s_console_dirty) {
            overlay_update_texture();
            s_console_dirty = 0;
        }

        /* Publish the hit-test geometry for the mouse handling in Update. */
        s_hit_vp[0] = vp[0]; s_hit_vp[1] = vp[1];
        s_hit_vp[2] = vp[2]; s_hit_vp[3] = vp[3];
        s_hit_valid = 1;

        /* Backdrop first (full width), then the selection highlight, then the
         * text at its natural glyph scale, cropping to the rows in use. */
        draw_panel(s_bg_tex, x0, y0, x1, y1);

        if (s_sel_valid) {
            int fl, fc, ll, lc, r;

            console_sel_order(&fl, &fc, &ll, &lc);
            if (!(fl == ll && fc == lc)) {
                for (r = 0; r <= s_vis_rows; r++) {
                    int line, c0, c1, len;

                    if (r == s_vis_rows)
                        line = -1; /* prompt row */
                    else if (s_scroll > 0 && r == 0)
                        continue;  /* scroll marker */
                    else {
                        line = s_scroll + (s_vis_rows - 1 - r);
                        if (line < 0 || line >= s_console_count)
                            continue;
                    }
                    if (line > fl || line < ll)
                        continue;

                    len = (int)strlen(console_sel_text(line));
                    if (line == -1)
                        len += 3; /* the prompt renders as "> text_" */
                    c0 = (line == fl) ? fc : 0;
                    c1 = (line == ll) ? lc : len;
                    if (c0 > len) c0 = len;
                    if (c1 > len) c1 = len;
                    if (c1 <= c0)
                        continue;

                    {
                        float sx0 = x0 + 2.0f * (float)(c0 * GLYPH_W * SCALE) / (float)vp[2];
                        float sx1 = x0 + 2.0f * (float)(c1 * GLYPH_W * SCALE) / (float)vp[2];
                        float sy0 = y0 - 2.0f * (float)(r * GLYPH_H * SCALE) / (float)vp[3];
                        float sy1 = sy0 - 2.0f * (float)(GLYPH_H * SCALE) / (float)vp[3];
                        draw_panel(s_sel_tex, sx0, sy0, sx1, sy1);
                    }
                }
            }
        }

        {
            /* Draw only the columns that fit the window at the natural 2x glyph
             * size, stretched from the left edge, so the text spans the full
             * window width instead of stopping at a fixed panel edge. The texture
             * holds CON_COLS columns; crop U to the fitted count. push_console
             * wraps lines to this same width so nothing is clipped on the right. */
            int   colsFit = vp[2] / (GLYPH_W * SCALE);
            float tu1;
            if (colsFit < 1)        colsFit = 1;
            if (colsFit > CON_COLS) colsFit = CON_COLS;
            tx1 = x0 + 2.0f * (float)(colsFit * GLYPH_W * SCALE) / (float)vp[2];
            tu1 = (float)colsFit / (float)CON_COLS;
            tv1 = (usedRows * (float)GLYPH_H) / (float)TEX_H;
            draw_panel_uv(s_tex, x0, y0, tx1, y1, tu1, tv1);
        }

        /* Pointer, topmost — the OS cursor is force-hidden, so while the
         * console is open this is the only visible cursor. Same arrow sprite
         * the menus use, drawn at 2x like the glyphs. */
        if (s_console_open && s_console_slide >= 1.0f) {
            int   wx, wy;
            float cx0, cy0;

            SDL_GetMouseState(&wx, &wy);
            cx0 = -1.0f + 2.0f * (float)(wx - vp[0]) / (float)vp[2];
            cy0 =  1.0f - 2.0f * (float)(wy - (g_windowHeight - (vp[1] + vp[3]))) / (float)vp[3];
            draw_panel(s_cursor_tex, cx0, cy0,
                       cx0 + 2.0f * 32.0f / (float)vp[2],
                       cy0 - 2.0f * 32.0f / (float)vp[3]);
        }
    }

    if (drawColl) {
        /* Bottom-right corner (clear of the top-left console and bottom-center
         * subtitles). Rebuilt every frame since the data is live. */
        float cw = 2.0f * (float)(COLL_TEX_W * SCALE) / (float)vp[2];
        float ch = 2.0f * (float)(COLL_TEX_H * SCALE) / (float)vp[3];
        float x1 =  1.0f;
        float x0 =  x1 - cw;
        float y1 = -1.0f;
        float y0 =  y1 + ch;

        coll_build_texture();
        draw_panel(s_coll_tex, x0, y0, x1, y1);
    }

    if (drawAnim) {
        /* Bottom-right; stacked directly above the collision panel when that's
         * also on, otherwise in the bottom-right corner itself. */
        float aw    = 2.0f * (float)(ANIM_TEX_W * SCALE) / (float)vp[2];
        float ah    = 2.0f * (float)(ANIM_TEX_H * SCALE) / (float)vp[3];
        float collH = drawColl ? (2.0f * (float)(COLL_TEX_H * SCALE) / (float)vp[3]) : 0.0f;
        float x1 =  1.0f;
        float x0 =  x1 - aw;
        float y1 = -1.0f + collH;
        float y0 =  y1 + ah;

        anim_build_texture();
        draw_panel(s_anim_tex, x0, y0, x1, y1);
    }

    if (s_score_on) {
        /* Left edge, vertically centred — clear of every debug panel (top-left
         * console/toast, bottom-right collision/anim). */
        float sw = 2.0f * (float)(SCORE_TEX_W * SCALE) / (float)vp[2];
        float sh = 2.0f * (float)(SCORE_TEX_H * SCALE) / (float)vp[3];
        float x0 = -1.0f + 2.0f * 12.0f / (float)vp[2]; /* 12px inset */
        float x1 = x0 + sw;
        float y0 = sh * 0.5f;
        float y1 = y0 - sh;

        score_build_texture();
        draw_panel(s_score_tex, x0, y0, x1, y1);
    }

    if (drawToast) {
        /* Top-left, where the console sits at rest — but only ever drawn while the
         * console is hidden, so they never overlap. */
        float x0 = -1.0f;
        float y0 =  1.0f;
        float x1 = x0 + 2.0f * (float)(TOAST_TEX_W * SCALE) / (float)vp[2];
        float y1 = y0 - 2.0f * (float)(TOAST_TEX_H * SCALE) / (float)vp[3];

        toast_build_texture(toastAlpha);
        draw_panel(s_toast_tex, x0, y0, x1, y1);
    }

    /* Collision wireframe lines (separate program). Consume + clear the
     * per-frame segment buffer captured during this frame's collision pass. */
    if (s_coll_on) {
        collvis_render_lines();
        s_cvHitCount = 0;    /* per-frame; cell segs/cyls cached until the cell changes */
        s_cvHitCylCount = 0; /* per-frame blocker markers */
    }

    /* Restore ALL state. */
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fb);
    glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
    glBindVertexArray(prev_vao);
    glActiveTexture(prev_active_tex);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glUseProgram(prev_prog);
    glBlendFunc(prev_blend_src, prev_blend_dst);
    glBlendEquationSeparate(prev_blend_eq_rgb, prev_blend_eq_a);
    if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_blend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
}

#endif /* SH_PC_PORT */
