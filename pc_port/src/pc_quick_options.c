/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_quick_options.c - in-game quick options overlay (key_quick_options, default F10).
 *
 * A sibling of pc_rando_settings.c and it repeats that file's hard-won GL
 * constraints deliberately rather than sharing code:
 *   - its OWN GL program / VAO / textures, with full state save+restore, so it
 *     never clobbers PsyCross's or the console's programs.
 *   - LEGACY GLSL (attribute / varying / gl_FragColor, no #version).
 *   - all GL work deferred to Draw (post-capture); the game thread only reads
 *     input and toggles state.
 *   - wall-clock timing (the draw hook runs after the frame is captured).
 *
 * Unlike the randomizer panel there is NO full-screen dim and the panel is
 * translucent: the point of this menu is to see a change land in the live
 * scene behind it. Text is stb_truetype, baked to small textures; the toast
 * owns the only stb_truetype implementation in the build.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"

#include "pc_quick_options.h"
#include "pc_mouse_cursor.h"
#include "pc_config.h"
#include "pc_cheats.h"
#include "sh_log.h"

/* options.c: the live-applying rows of the PC Options screen, by config key. */
extern const void* PcOpt_QuickFind(const char* key);
extern const char* PcOpt_QuickName(const void* h);
extern const char* PcOpt_QuickLabel(const void* h, char* buf, int bufsz);
extern void        PcOpt_QuickAdjust(const void* h, int dir);
extern int         PcOpt_QuickRealtime(const void* h);
/* options.c: rows that are not in that table. */
enum { QO_X_SHADOW = 0, QO_X_SPEAKERS, QO_X_BGM, QO_X_SFX };
extern const char* PcOpt_QuickExtraLabel(int which, char* buf, int bufsz);
extern void        PcOpt_QuickExtraAdjust(int which, int dir);

#define QO_GARBAGE  48
#define QO_MAX_ROWS 16
#define QO_PAGES    4
#define QO_DD_MAX     64  /* dropdown entries cached */
#define QO_DD_VISIBLE 8

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

enum { ROW_OPT = 0, ROW_EXTRA, ROW_PAGE, ROW_CLOSE, ROW_CHEAT };

typedef struct
{
    int         kind;   /* ROW_* */
    const char* key;    /* ROW_OPT: config key looked up in options.c */
    int         extra;  /* ROW_EXTRA: QO_X_*; ROW_CHEAT: row index */
    const char* label;  /* ROW_EXTRA / ROW_PAGE / ROW_CLOSE display name */
    int         cpage;  /* ROW_CHEAT: PC_CHEATS_PAGE_* */
} QoRowDef;

#define QO_IS_VALUE_ROW(k) ((k) == ROW_OPT || (k) == ROW_EXTRA || (k) == ROW_CHEAT)

static const QoRowDef s_page0[] = {
    { ROW_OPT,   "psx_dither",           0, NULL },  /* Texture_Filter */
    { ROW_OPT,   "msaa",                 0, NULL },  /* Antialiasing (restart) */
    { ROW_OPT,   "post_process",         0, NULL },
    { ROW_OPT,   "tonemap",              0, NULL },
    { ROW_OPT,   "fog_strength",         0, NULL },
    { ROW_OPT,   "flashlight_mode",      0, NULL },
    { ROW_OPT,   "flashlight_intensity", 0, NULL },
    { ROW_OPT,   "flashlight_size",      0, NULL },
    { ROW_EXTRA, NULL, QO_X_SHADOW,         "Shadow Resolution" },
    { ROW_OPT,   "bullet_decals",        0, NULL },
    { ROW_PAGE,  NULL, 0,                   "Next page  (HUD & Audio)" },
    { ROW_CLOSE, NULL, 0,                   "Close" },
};

static const QoRowDef s_page1[] = {
    { ROW_OPT,   "minimap",              0, NULL },
    { ROW_OPT,   "minimap_scale",        0, NULL },
    { ROW_OPT,   "minimap_corner",       0, NULL },
    { ROW_OPT,   "minimap_opacity",      0, NULL },
    { ROW_OPT,   "minimap_require_map",  0, NULL },
    { ROW_OPT,   "crosshair",            0, NULL },
    { ROW_OPT,   "low_health_glow",      0, NULL },
    { ROW_EXTRA, NULL, QO_X_SPEAKERS,       "Speaker Layout" },
    { ROW_EXTRA, NULL, QO_X_BGM,            "Music Volume" },
    { ROW_EXTRA, NULL, QO_X_SFX,            "Effects Volume" },
    { ROW_OPT,   "fmv_volume",           0, NULL },
    { ROW_PAGE,  NULL, 0,                   "Next page  (Cheats)" },
    { ROW_CLOSE, NULL, 0,                   "Close" },
};

/* Pages 2/3 mirror pc_cheats.c's tables, built on first use. */
static QoRowDef s_cheatRows[2][QO_MAX_ROWS];
static int      s_cheatRowCount[2];

static const QoRowDef* qo_cheat_page(int cpage, const char* nextLabel, int* count)
{
    if (s_cheatRowCount[cpage] == 0)
    {
        int n = Pc_Cheats_Count(cpage), i, k = 0;
        if (n > QO_MAX_ROWS - 2) n = QO_MAX_ROWS - 2;
        for (i = 0; i < n; i++)
        {
            s_cheatRows[cpage][k].kind  = ROW_CHEAT;
            s_cheatRows[cpage][k].key   = NULL;
            s_cheatRows[cpage][k].extra = i;
            s_cheatRows[cpage][k].label = NULL;
            s_cheatRows[cpage][k].cpage = cpage;
            k++;
        }
        s_cheatRows[cpage][k].kind = ROW_PAGE;  s_cheatRows[cpage][k].label = nextLabel; k++;
        s_cheatRows[cpage][k].kind = ROW_CLOSE; s_cheatRows[cpage][k].label = "Close";   k++;
        s_cheatRowCount[cpage] = k;
    }
    *count = s_cheatRowCount[cpage];
    return s_cheatRows[cpage];
}

static const QoRowDef* qo_page_rows(int page, int* count)
{
    if (page == 1) { *count = (int)(sizeof(s_page1) / sizeof(s_page1[0])); return s_page1; }
    if (page == 2) return qo_cheat_page(PC_CHEATS_PAGE_CHEATS, "Next page  (Debug)",    count);
    if (page == 3) return qo_cheat_page(PC_CHEATS_PAGE_DEBUG,  "Next page  (Graphics)", count);
    *count = (int)(sizeof(s_page0) / sizeof(s_page0[0]));
    return s_page0;
}

static const char* const s_pageTitles[QO_PAGES] = {
    "QUICK OPTIONS  -  GRAPHICS", "QUICK OPTIONS  -  HUD & AUDIO",
    "QUICK OPTIONS  -  CHEATS",   "QUICK OPTIONS  -  DEBUG" };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { QO_CLOSED = 0, QO_OPENING, QO_SHOWN, QO_CLOSING };
static int    s_phase;
static Uint32 s_phaseStart;
#define QO_OPEN_MS  160u
#define QO_CLOSE_MS 120u

int g_PcQuickOptionsActive = 0;

static int s_page;
static int s_sel;

/* GL */
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locColor;
static int    s_glReady;
static GLuint s_texWhite;
static GLuint s_texCursor;
static GLuint s_garbage[QO_GARBAGE];
static int    s_garbageCount;

/* Fonts (stb_truetype). */
static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontsTried;

/* Baked text, re-baked when the pixel size or the page changes; values are
 * re-baked whenever their text changes. */
static GLuint s_texTitle, s_texHint;
static int    s_titleW, s_titleH, s_hintW, s_hintH;
static GLuint s_texLabel[QO_MAX_ROWS];
static int    s_labelW[QO_MAX_ROWS], s_labelH[QO_MAX_ROWS];
static GLuint s_texValue[QO_MAX_ROWS];
static int    s_valueW[QO_MAX_ROWS], s_valueH[QO_MAX_ROWS];
static char   s_valueText[QO_MAX_ROWS][48];
static int    s_bakedForPx;
static int    s_bakedForPage = -1;

/* Geometry published by Draw for Update's mouse hit-test (viewport px, y up). */
static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static float s_geoListT, s_geoRowPitch, s_geoRowH, s_geoPanelL, s_geoPanelR;
/* Panel drag: grab the title bar and move it. Offsets are in viewport px and
 * survive close/reopen within a session. The title-bar rect is published for
 * Update's hit-test the same way the row geometry is. */
static float s_panelOfsX, s_panelOfsY;
static float s_geoTitleB, s_geoTitleT;
static int   s_dragging;
static float s_dragLastX, s_dragLastY;
static int   s_geoRows;

/* Dropdown over a list row (the Spawn row): open on clicking its value,
 * navigated with Up/Down / wheel / hover, picked with confirm / click. */
static int    s_ddRow = -1;   /* row index on the current page, -1 = closed */
static int    s_ddSel;
static int    s_ddScroll;
static GLuint s_ddTex[QO_DD_MAX];
static int    s_ddW[QO_DD_MAX], s_ddH[QO_DD_MAX];
static float  s_ddL, s_ddR, s_ddTop, s_ddRowH; /* published for the hit-test */
static int    s_ddShown;

static int qo_row_is_list(const QoRowDef* r)
{
    return r->kind == ROW_CHEAT && Pc_Cheats_ListCount(r->cpage, r->extra) > 0;
}

/* ------------------------------------------------------------------ */
/* GL primitives (mirrors pc_rando_settings.c)                         */
/* ------------------------------------------------------------------ */

static GLuint qo_make_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[QUICKOPT] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int qo_atlas_ensure(void);

static void qo_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_glReady = -1;
    vs = qo_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = qo_make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[QUICKOPT] link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    qo_atlas_ensure(); /* one texture, created here, never recreated */
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    s_glReady = 1;
}

static void qo_reg_set(GLuint id, int w, int h, unsigned hash, int nearest);
static unsigned qo_hash(const unsigned char* p, size_t n);

/* ---------------------------------------------------------------- *
 * Glyph atlas.
 *
 * Everything the panel draws -- every label, value, title, footer and the
 * solid-white quad behind the panel itself -- lives in ONE texture created
 * once, at init, and never recreated.
 *
 * This is a rewrite of a design that repeatedly drew its text as solid
 * coloured blocks. What forced it: the panel's own numbers make it
 * impossible for those blocks to come from our own pixels. A baked string
 * is white RGB with alpha = coverage, and real text measures 12-21% ink, so
 * texture2D(...) * u_color can never yield a fully opaque rectangle. The
 * blocks therefore meant we were not sampling our texture at all -- and
 * every check written to prove otherwise (name alive, dimensions intact,
 * pixels byte-identical, sampler state correct) passed, because the pixels
 * were never the problem.
 *
 * The one object that never once failed across every report is s_texWhite,
 * and what set it apart is that it was created at INIT. Everything that
 * failed was created lazily, mid-frame, inside the game's own GL stream --
 * where PsyCross keeps a redundant-bind cache (g_lastBoundTexture) that
 * early-outs and skips glBindTexture, and where texture names are being
 * created and destroyed constantly. So the fix is to stop making textures
 * during a frame at all: one atlas, allocated up front, sub-image updates
 * only.
 *
 * Slots are handed out by a shelf allocator and handed back as the same
 * GLuint the old code passed around, so every caller and all the layout
 * maths is untouched -- the value is now a slot index + 1 rather than a
 * texture name. Padding keeps LINEAR filtering from bleeding between
 * neighbours. */
#define QO_ATLAS_W    2048
#define QO_ATLAS_H    1024
#define QO_ATLAS_PAD  2
#define QO_SLOT_MAX   96

static GLuint s_atlasTex;
static int    s_atlasReady;
static int    s_atlasX, s_atlasY, s_atlasRowH;
static struct { int x, y, w, h; } s_slot[QO_SLOT_MAX];
static int    s_slotCount;

/* Slot 0 is the solid white texel and is PERMANENT: the panel background,
 * the scrollbar and the row highlights all draw from it, and a reset that
 * handed slot 0 to the next label would repaint those with a word. */
static void qo_atlas_reset(void)
{
    s_atlasX    = QO_ATLAS_PAD;
    s_atlasY    = QO_ATLAS_PAD;
    s_atlasRowH = 0;
    s_slotCount = 0;
    if (s_atlasTex)
    {
        /* re-claim slot 0 in place, exactly as qo_atlas_ensure laid it out */
        s_slot[0].x = QO_ATLAS_PAD; s_slot[0].y = QO_ATLAS_PAD;
        s_slot[0].w = 1;            s_slot[0].h = 1;
        s_slotCount = 1;
        s_atlasX    = QO_ATLAS_PAD + 1 + QO_ATLAS_PAD;
        s_atlasRowH = 1;
    }
}

static int qo_atlas_ensure(void)
{
    if (s_atlasReady)
        return s_atlasTex != 0;

    s_atlasReady = 1;
    glGenTextures(1, &s_atlasTex);
    if (!s_atlasTex)
        return 0;

    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* A MIN filter wanting mip levels that do not exist makes a texture
     * INCOMPLETE, and an incomplete texture samples as (0,0,0,1): opaque
     * black. Pinning the chain at level 0 puts that state out of reach no
     * matter what filter anything else sets. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, QO_ATLAS_W, QO_ATLAS_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    {
        /* Slot 0: one opaque white texel, written once and never reissued. */
        const unsigned char one[4] = { 255, 255, 255, 255 };
        glTexSubImage2D(GL_TEXTURE_2D, 0, QO_ATLAS_PAD, QO_ATLAS_PAD, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, one);
    }
    qo_atlas_reset();
    SH_DBG("[QOTEX] glyph atlas %dx%d id=%u", QO_ATLAS_W, QO_ATLAS_H, (unsigned)s_atlasTex);
    return 1;
}

/* Returns a slot handle (index + 1), or 0 if it will not fit. */
static GLuint qo_upload_rgba(const unsigned char* rgba, int w, int h, int nearest)
{
    int idx;

    (void)nearest; /* one atlas, one filter; padding covers the bleed */

    if (!qo_atlas_ensure() || w <= 0 || h <= 0 || s_slotCount >= QO_SLOT_MAX)
        return 0;
    if (w > QO_ATLAS_W - 2 * QO_ATLAS_PAD || h > QO_ATLAS_H - 2 * QO_ATLAS_PAD)
        return 0;

    if (s_atlasX + w + QO_ATLAS_PAD > QO_ATLAS_W)
    {
        s_atlasX     = QO_ATLAS_PAD;
        s_atlasY    += s_atlasRowH + QO_ATLAS_PAD;
        s_atlasRowH  = 0;
    }
    if (s_atlasY + h + QO_ATLAS_PAD > QO_ATLAS_H)
        return 0; /* full: caller simply draws nothing this pass */

    idx = s_slotCount++;
    s_slot[idx].x = s_atlasX;
    s_slot[idx].y = s_atlasY;
    s_slot[idx].w = w;
    s_slot[idx].h = h;

    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, s_atlasX, s_atlasY, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    s_atlasX    += w + QO_ATLAS_PAD;
    if (h > s_atlasRowH)
        s_atlasRowH = h;

    return (GLuint)(idx + 1);
}

/* Retiring a texture used to queue it for glDeleteTextures. It must not.
 *
 * A deleted GL name goes straight back to the driver's pool and the game takes
 * it on its next texture create -- after which these quads sample whatever the
 * game put there. That is the solid red/black block corruption: the [QOTEX]
 * probe showed every upload is clean (right size, first texel 255,255,255,0,
 * glErr 0), so the content is only wrong LATER, once the name is no longer
 * ours. It also explains why the colour varied by area (whatever texture
 * inherited the name) and why glIsTexture-based validation could not catch it
 * -- the name is still a perfectly valid texture, just not ours.
 *
 * Retired names are therefore kept alive and simply abandoned. The caller has
 * already zeroed its slot, so the next draw bakes a fresh texture. That leaks
 * one small texture per re-bake (page switches and value edits only), which is
 * a few MB across a long session -- the correct fix is to re-upload into the
 * same name instead of allocating a new one, but that is a wider change than
 * belongs in a release build. */
static void qo_retire(GLuint tex)
{
    (void)tex;
}

/* Nothing is queued for deletion any more (see qo_retire); kept so the draw
 * path is unchanged and a future in-place-reuse rewrite has a hook. */
static void qo_gl_pump(void)
{
    s_garbageCount = 0;
}

/* Quad in NDC (yTop > yBot). */
/* `tex` is an atlas SLOT handle (index + 1), not a texture name. */
static void qo_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    float u0, v0, u1, v1;
    int   idx = (int)tex - 1;

    if (tex == 0 || idx < 0 || idx >= s_slotCount || !s_atlasTex)
        return;

    /* Half-texel inset: with LINEAR filtering an edge tap would otherwise
     * reach into the padding between neighbouring slots. */
    u0 = ((float)s_slot[idx].x + 0.5f) / (float)QO_ATLAS_W;
    v0 = ((float)s_slot[idx].y + 0.5f) / (float)QO_ATLAS_H;
    u1 = ((float)(s_slot[idx].x + s_slot[idx].w) - 0.5f) / (float)QO_ATLAS_W;
    v1 = ((float)(s_slot[idx].y + s_slot[idx].h) - 0.5f) / (float)QO_ATLAS_H;

    v[0][0] = x0; v[0][1] = yTop; v[0][2] = u0; v[0][3] = v0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = u0; v[1][3] = v1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = u1; v[2][3] = v0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = u1; v[3][3] = v0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = u0; v[4][3] = v1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = u1; v[5][3] = v1;

    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, s_atlasTex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

static unsigned char* qo_read_file(const char* path, long* outSize)
{
    FILE* f = fopen(path, "rb");
    long  n;
    unsigned char* buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    if (outSize) *outSize = n;
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

/* stb_truetype does not copy the font file -- it reads this buffer on every
 * glyph. If it is corrupted, glyphs rasterize as filled boxes, which is exactly
 * what the panel shows, and the texture checks all pass because the block is
 * uploaded faithfully. A strided signature is cheap enough to verify per frame
 * and catches a stray write into the buffer. */
static const char* s_fontPath;
static long        s_fontSize;
static unsigned    s_fontSig;

static unsigned qo_font_sig(const unsigned char* p, long n)
{
    unsigned h = 2166136261u;
    long     i;

    for (i = 0; i < n; i += 61)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ^ (unsigned)n;
}

/* Re-reads the font into a fresh buffer. The old one is abandoned, never freed:
 * stbtt may still hold pointers into it, and whatever scribbled on it will keep
 * doing so. */
static int qo_font_reload(void)
{
    long           sz    = 0;
    unsigned char* fresh = s_fontPath ? qo_read_file(s_fontPath, &sz) : NULL;

    if (fresh && stbtt_InitFont(&s_font, fresh, stbtt_GetFontOffsetForIndex(fresh, 0)))
    {
        s_fontData = fresh;
        s_fontSize = sz;
        s_fontSig  = qo_font_sig(fresh, sz);
        return 1;
    }
    if (fresh)
        free(fresh);
    return 0;
}

/* Returns non-zero when the font memory changed under us (and reloads it). */
static int qo_font_check(void)
{
    static int s_fontLogs = 0;
    unsigned   now;

    if (!s_fontOk || !s_fontData || s_fontSize <= 0)
        return 0;

    now = qo_font_sig(s_fontData, s_fontSize);
    if (now == s_fontSig)
        return 0;

    if (s_fontLogs < 8)
    {
        s_fontLogs++;
        SH_DBG("[QOFONT] font memory CORRUPTED: sig %08x -> %08x (%ld bytes at %p, %s) "
               "-- glyphs would rasterize as blocks; reloading",
               s_fontSig, now, s_fontSize, (void*)s_fontData,
               s_fontPath ? s_fontPath : "?");
    }

    if (qo_font_reload())
        return 1;
    s_fontSig = now; /* cannot reload; stop re-reporting every frame */
    return 1;
}

static void qo_fonts_init(void)
{
    static const char* paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    int i;
    if (s_fontsTried)
        return;
    s_fontsTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        long           sz   = 0;
        unsigned char* data = qo_read_file(paths[i], &sz);
        if (!data)
            continue;
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData = data; /* stbtt keeps pointers into this - never free */
            s_fontOk   = 1;
            s_fontPath = paths[i];
            s_fontSize = sz;
            s_fontSig  = qo_font_sig(data, sz);
            return;
        }
        free(data);
    }
    SH_DBG("[QUICKOPT] no usable font - text disabled");
}

/* Rasterize one line of ASCII to a white-coverage RGBA texture, trimmed to
 * the ink so right-alignment and centring use the real text width. */
static GLuint qo_bake_once(const char* text, float px, int* outW, int* outH, int* outInk)
{
    const char* p;
    float scale, penX, baseY;
    int   asc, desc, gap, W, H, i, pad = 1, prev = 0, stride;
    unsigned char *cov, *rgba, *scratch;
    GLuint tex;

    if (!s_fontOk || !text || !text[0] || px < 4.0f)
        return 0;

    scale = stbtt_ScaleForPixelHeight(&s_font, px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)(px * 0.62f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf(px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;
    stride = W;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return 0; }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    p     = text;
    while (*p)
    {
        int cp = (unsigned char)*p++;
        int gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx;
                    unsigned char v;
                    if (dx < 0 || dx >= W) continue;
                    v = scratch[sy * gw + sx];
                    if (v > cov[dy * W + dx]) cov[dy * W + dx] = v;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }

    {
        int inkW = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
    }
    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
    }
    {
        /* Ink coverage of what was actually rasterized. This separates a bake
         * that produced filled boxes (~100%) from a bake that is fine and a
         * draw that samples something opaque -- the texture checks cannot tell
         * those apart, because a faithfully-uploaded block passes every one. */
        int    n   = W * H;
        int    ink = 0;
        int    k;

        for (k = 0; k < n; k++)
        {
            if (rgba[k * 4 + 3] >= 32)
                ink++;
        }
        if (outInk)
            *outInk = n ? (ink * 100 / n) : -1;
    }
    tex = qo_upload_rgba(rgba, W, H, 0);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

/* Glyphs rasterized from bad font memory come out as filled boxes -- the solid
 * blotches users hit. That is measurable: real text inks 12-21% of its box, a
 * block ~100%. So a bake that comes back saturated is not shipped. The font is
 * re-read and the string baked again; if it is still saturated nothing is
 * returned, because a blank row is at least honest and the next frame retries.
 * Single characters are exempt -- a glyph like a full block legitimately fills
 * its box. */
#define QO_INK_MAX 55


static GLuint qo_bake(const char* text, float px, int* outW, int* outH)
{
    static int s_bakeLogs = 0;
    int    ink = -1;
    GLuint tex = qo_bake_once(text, px, outW, outH, &ink);

    if (tex == 0 || ink < QO_INK_MAX || !text || !text[1])
        return tex;

    if (s_bakeLogs < 8)
    {
        s_bakeLogs++;
        SH_DBG("[QOBAKE] \"%.24s\" baked SATURATED (ink=%d%%, normal is 12-21) -- "
               "glyph data is bad; reloading the font and re-baking", text, ink);
    }

    qo_retire(tex);
    if (!qo_font_reload())
        return 0;

    ink = -1;
    tex = qo_bake_once(text, px, outW, outH, &ink);
    if (tex && ink >= QO_INK_MAX)
    {
        qo_retire(tex);
        return 0;
    }
    return tex;
}

static void qo_build_white(void)
{
    if (qo_atlas_ensure())
        s_texWhite = 1; /* slot 0 handle */

}

static void qo_free_text(void)
{
    int i;

    /* Slots are recycled wholesale: the atlas itself is never freed, so
     * there is no texture lifetime left to get wrong.
     *
     * EVERY handle held outside this function has to be dropped with them,
     * or it keeps pointing at a slot index that now belongs to a different
     * image. The cursor is the one that is not in the loops below: after a
     * page change it drew a label's pixels, then vanished once the index
     * fell past the live slot count. Only slot 0, the white texel, survives
     * a reset, because qo_atlas_reset re-claims it in place. */
    qo_atlas_reset();
    s_texCursor = 0; /* re-uploaded on the next draw */
    qo_retire(s_texTitle); s_texTitle = 0;
    qo_retire(s_texHint);  s_texHint  = 0;
    for (i = 0; i < QO_MAX_ROWS; i++)
    {
        qo_retire(s_texLabel[i]); s_texLabel[i] = 0;
        qo_retire(s_texValue[i]); s_texValue[i] = 0;
        s_valueText[i][0] = 0;
    }
    for (i = 0; i < QO_DD_MAX; i++)
    {
        qo_retire(s_ddTex[i]); s_ddTex[i] = 0;
    }
}

/* The overlay's baked textures have twice been seen drawing as solid blocks
 * -- red once, black once. The COLOUR VARYING is the tell: the quads sample
 * whatever texture currently owns that GL name, i.e. the name was freed out
 * from under us and handed to the game (the 1x1 white panel texture, created
 * once at init, never shows it). Deletion is detectable, so verify every
 * cached name before use and rebuild any that went stale, and say so once. */
/* What we uploaded, per texture name. glIsTexture cannot tell that a name is
 * still OURS -- only that it is a live texture -- so the size we put there is
 * kept and checked against what the driver reports. Round-robin; a name that
 * ages out simply stops being checked. */
#define QO_TEXREG 256
static GLuint   s_regId[QO_TEXREG];
static GLint    s_regW[QO_TEXREG], s_regH[QO_TEXREG];
static unsigned s_regHash[QO_TEXREG];
static unsigned char s_regNearest[QO_TEXREG];
static int      s_regNext;
/* One texture is content-checked per frame, rotating, so the readback stall
 * stays off the per-frame path. */
static int s_deepIdx, s_passSlot;

static unsigned qo_hash(const unsigned char* p, size_t n)
{
    unsigned h = 2166136261u;
    size_t   i;

    for (i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void qo_reg_set(GLuint id, int w, int h, unsigned hash, int nearest)
{
    int i;

    if (id == 0)
        return;
    for (i = 0; i < QO_TEXREG; i++)
    {
        if (s_regId[i] == id)
        {
            s_regW[i] = w; s_regH[i] = h; s_regHash[i] = hash;
            s_regNearest[i] = (unsigned char)(nearest ? 1 : 0);
            return;
        }
    }
    s_regId[s_regNext]   = id;
    s_regW[s_regNext]    = w;
    s_regH[s_regNext]    = h;
    s_regHash[s_regNext] = hash;
    s_regNearest[s_regNext] = (unsigned char)(nearest ? 1 : 0);
    s_regNext = (s_regNext + 1) % QO_TEXREG;
}

/* A texture whose min filter wants mip levels it does not have is INCOMPLETE,
 * and an incomplete texture samples as (0,0,0,1) -- a solid opaque black quad,
 * exactly what the panel shows. This is why the 1x1 white background is immune
 * while every baked label is not: at 1x1, level 0 IS the whole mip chain, so no
 * filter can ever make it incomplete. Content, size and name all check out
 * clean, so the sampler state is what is left. Re-assert it every frame and say
 * so once if it had drifted. */


static void qo_validate_ex(GLuint* t, const char* what, int deep);

static void qo_validate(GLuint* t, const char* what)
{
    qo_validate_ex(t, what, s_passSlot++ == s_deepIdx);
}

/* Nothing to validate any more. These are atlas slot indices, not texture
 * names, and the single atlas is created once and never deleted -- the
 * lifetime problem this used to police no longer exists. */
static void qo_validate_ex(GLuint* t, const char* what, int deep)
{
    (void)t; (void)what; (void)deep;

}

static void qo_validate_cache(void)
{
    int i;

    s_passSlot = 0;

    if (qo_font_check())
    {
        qo_free_text();   /* every cached label was baked from bad glyph data */
        return;
    }

    qo_validate(&s_texTitle,  "title");
    qo_validate(&s_texHint,   "hint");
    qo_validate(&s_texWhite,  "white");
    qo_validate(&s_texCursor, "cursor");
    for (i = 0; i < QO_MAX_ROWS; i++)
    {
        qo_validate(&s_texLabel[i], "label");
        qo_validate(&s_texValue[i], "value");
        if (s_texValue[i] == 0)
            s_valueText[i][0] = 0; /* force the value string to re-bake too */
    }
    for (i = 0; i < QO_DD_MAX; i++)
        qo_validate(&s_ddTex[i], "dropdown");

    if (s_passSlot > 0)
        s_deepIdx = (s_deepIdx + 1) % s_passSlot;
}

/* Options-table names use underscores for spaces. */
static void qo_row_name(const QoRowDef* r, char* out, int n)
{
    const char* src = NULL;
    int i;

    if (r->kind == ROW_OPT)
    {
        const void* h = PcOpt_QuickFind(r->key);
        src = h ? PcOpt_QuickName(h) : r->key;
    }
    else if (r->kind == ROW_CHEAT)
        src = Pc_Cheats_Name(r->cpage, r->extra);
    else
        src = r->label;

    for (i = 0; i < n - 1 && src[i]; i++)
        out[i] = (src[i] == '_') ? ' ' : src[i];
    out[i] = 0;
}

static void qo_row_value(const QoRowDef* r, char* out, int n)
{
    char buf[48];
    out[0] = 0;
    if (r->kind == ROW_OPT)
    {
        const void* h = PcOpt_QuickFind(r->key);
        const char* v = h ? PcOpt_QuickLabel(h, buf, (int)sizeof(buf)) : "?";
        int i;
        for (i = 0; i < n - 1 && v[i]; i++)
            out[i] = (v[i] == '_') ? ' ' : v[i];
        out[i] = 0;
        if (h && !PcOpt_QuickRealtime(h) && i < n - 3)
        {
            out[i++] = ' '; out[i++] = '*'; out[i] = 0;
        }
    }
    else if (r->kind == ROW_EXTRA)
    {
        snprintf(out, (size_t)n, "%s", PcOpt_QuickExtraLabel(r->extra, buf, (int)sizeof(buf)));
    }
    else if (r->kind == ROW_CHEAT)
    {
        snprintf(out, (size_t)n, "%s", Pc_Cheats_Label(r->cpage, r->extra, buf, (int)sizeof(buf)));
    }
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

int Pc_QuickOptions_IsOpen(void)
{
    return s_phase != QO_CLOSED;
}

static void qo_open(void)
{
    if (s_phase == QO_OPENING || s_phase == QO_SHOWN)
        return;
    s_phase      = QO_OPENING;
    s_phaseStart = SDL_GetTicks();
    s_sel        = 0;
    g_PcQuickOptionsActive = 1;
}

void Pc_QuickOptions_Close(void)
{
    s_ddRow = -1;
    if (s_phase == QO_CLOSED || s_phase == QO_CLOSING)
        return;
    s_phase      = QO_CLOSING;
    s_phaseStart = SDL_GetTicks();
}

void Pc_QuickOptions_Toggle(void)
{
    if (s_phase == QO_CLOSED || s_phase == QO_CLOSING)
        qo_open();
    else
        Pc_QuickOptions_Close();
}

/* Phase machine on the GAME thread, wall clock. It used to live in Draw, and a
 * Draw that bailed (shader failed on ANGLE/ES) left the panel stuck in OPENING
 * with the freeze flag set: game frozen, input swallowed, cursor gone, no way
 * out. Draw only renders now; if GL is unusable the panel closes itself. */
static float qo_phase_tick(void)
{
    Uint32 age = SDL_GetTicks() - s_phaseStart;
    float  dim = 1.0f;

    if (s_glReady < 0)
    {
        static int s_warned;
        if (!s_warned) { s_warned = 1; SH_DBG("[QUICKOPT] GL unavailable -- quick options disabled for this run"); }
        s_phase = QO_CLOSED;
        g_PcQuickOptionsActive = 0;
        return 0.0f;
    }
    if (s_phase == QO_OPENING)
    {
        dim = (float)age / (float)QO_OPEN_MS;
        if (dim >= 1.0f) { dim = 1.0f; s_phase = QO_SHOWN; }
    }
    else if (s_phase == QO_CLOSING)
    {
        dim = 1.0f - (float)age / (float)QO_CLOSE_MS;
        if (dim <= 0.0f) { dim = 0.0f; s_phase = QO_CLOSED; g_PcQuickOptionsActive = 0; }
    }
    return dim;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Rising edge for a scancode, own history so it never fights the pad path. */
static int qo_key_edge(int sc)
{
    static unsigned char s_prev[SDL_NUM_SCANCODES];
    const unsigned char* ks = SDL_GetKeyboardState(NULL);
    int down = ks ? ks[sc] : 0;
    int edge = down && !s_prev[sc];
    s_prev[sc] = (unsigned char)down;
    return edge;
}

static void qo_set_page(int page)
{
    int n;
    s_ddRow = -1;
    s_page = (page + QO_PAGES) % QO_PAGES;
    qo_page_rows(s_page, &n);
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0)  s_sel = 0;
}

/* Confirm / click: cheat rows have their own confirm (the Spawn row fires
 * its browsed entry); everything else steps up. */
static void qo_confirm(const QoRowDef* r);

static void qo_activate(const QoRowDef* r, int dir)
{
    switch (r->kind)
    {
        case ROW_OPT:
        {
            const void* h = PcOpt_QuickFind(r->key);
            if (h) PcOpt_QuickAdjust(h, dir);
            break;
        }
        case ROW_EXTRA: PcOpt_QuickExtraAdjust(r->extra, dir); break;
        case ROW_CHEAT: Pc_Cheats_Adjust(r->cpage, r->extra, dir); break;
        case ROW_PAGE:  qo_set_page(s_page + (dir < 0 ? -1 : +1)); break;
        case ROW_CLOSE: Pc_QuickOptions_Close(); break;
        default: break;
    }
}

/* Directions arrive as HELD state and repeat on the wall clock here. The
 * pad's own pulse repeat counts vblanks per rendered frame, so with an
 * uncapped frame rate a held arrow stepped every few milliseconds. */
#define QO_REPEAT_FIRST_MS 380u
#define QO_REPEAT_MS       85u
static int qo_repeat(int idx, int held)
{
    static int    s_wasHeld[4];
    static Uint32 s_nextAt[4];
    Uint32 now = SDL_GetTicks();
    int fire = 0;

    if (held && !s_wasHeld[idx])
    {
        fire = 1;
        s_nextAt[idx] = now + QO_REPEAT_FIRST_MS;
    }
    else if (held && (Sint32)(now - s_nextAt[idx]) >= 0)
    {
        fire = 1;
        s_nextAt[idx] = now + QO_REPEAT_MS;
    }
    s_wasHeld[idx] = held;
    return fire;
}

void Pc_QuickOptions_Update(int up, int down, int left, int right,
                            int confirm, int close, int pageNext, int pagePrev)
{
    int nRows;
    const QoRowDef* rows = qo_page_rows(s_page, &nRows);
    int mMoved, mClick, mRClick, wheel;
    float mx, my;

    up    = qo_repeat(0, up);
    down  = qo_repeat(1, down);
    left  = qo_repeat(2, left);
    right = qo_repeat(3, right);

    qo_phase_tick();

    /* Keyboard extras (arrows arrive through the pad emulation already). */
    if (qo_key_edge(SDL_SCANCODE_ESCAPE))   close    = 1;
    if (qo_key_edge(SDL_SCANCODE_PAGEDOWN) || qo_key_edge(SDL_SCANCODE_E)) pageNext = 1;
    if (qo_key_edge(SDL_SCANCODE_PAGEUP)   || qo_key_edge(SDL_SCANCODE_Q)) pagePrev = 1;

    if (s_phase != QO_SHOWN) /* ignore input while animating in/out */
        return;

    /* Dropdown open: it owns every input until it closes. */
    if (s_ddRow >= 0 && s_ddRow < nRows && qo_row_is_list(&rows[s_ddRow]))
    {
        const QoRowDef* r = &rows[s_ddRow];
        int n = Pc_Cheats_ListCount(r->cpage, r->extra);
        int pick = -1, mMoved2, mClick2, wheel2;
        float mx2, my2;

        if (close) { s_ddRow = -1; return; }
        if (up)   s_ddSel = (s_ddSel + n - 1) % n;
        if (down) s_ddSel = (s_ddSel + 1) % n;
        /* Keyboard steps the selection; the window follows it. */
        if (s_ddSel < s_ddScroll) s_ddScroll = s_ddSel;
        if (s_ddSel >= s_ddScroll + QO_DD_VISIBLE) s_ddScroll = s_ddSel - QO_DD_VISIBLE + 1;

        /* The wheel scrolls the WINDOW (standard dropdown feel). It used to step
         * the selection instead, which hover re-snapped to the line under the
         * cursor on the next jitter -- so with the pointer resting over the
         * list the wheel felt dead. */
        wheel2 = Pc_MouseCursor_WheelStep();
        if (wheel2) s_ddScroll += (wheel2 > 0) ? -1 : 1;
        {
            int vis = (n < QO_DD_VISIBLE) ? n : QO_DD_VISIBLE;
            if (s_ddScroll > n - vis) s_ddScroll = n - vis;
            if (s_ddScroll < 0) s_ddScroll = 0;
            if (s_ddSel < s_ddScroll) s_ddSel = s_ddScroll;
            if (s_ddSel > s_ddScroll + vis - 1) s_ddSel = s_ddScroll + vis - 1;
        }

        mMoved2 = Pc_MouseCursor_Moved();
        mClick2 = Pc_MouseCursor_LeftClicked();
        if (s_ddShown && (mMoved2 || mClick2 || wheel2) && Pc_MouseCursor_ViewportPos(&mx2, &my2))
        {
            float py  = (1.0f - my2) * s_vpH;
            float mpx = mx2 * s_vpW;
            int   vis = (n < QO_DD_VISIBLE) ? n : QO_DD_VISIBLE;
            int   line = (int)((s_ddTop - py) / s_ddRowH);
            int   inside = mpx >= s_ddL && mpx <= s_ddR && py <= s_ddTop && py >= s_ddTop - vis * s_ddRowH;
            if (inside && line >= 0 && line < vis)
            {
                /* highlight follows the pointer as the list moves under it */
                if (mMoved2 || wheel2) s_ddSel = s_ddScroll + line;
                if (mClick2) pick = s_ddScroll + line;
            }
            else if (mClick2)
            {
                s_ddRow = -1; /* clicked away */
                return;
            }
        }
        if (confirm) pick = s_ddSel;

        if (pick >= 0)
        {
            Pc_Cheats_ListSet(r->cpage, r->extra, pick);
            s_ddRow = -1;
        }
        return;
    }
    s_ddRow = -1;

    if (close) { Pc_QuickOptions_Close(); return; }

    /* Title-bar drag. Held (not clicked) so it tracks continuously, and it is
     * resolved before the row hit-test below so dragging never also activates
     * whatever the cursor passes over. */
    {
        float dmx, dmy;

        if (Pc_MouseCursor_LeftHeld() && Pc_MouseCursor_ViewportPos(&dmx, &dmy))
        {
            float px_ = dmx * s_vpW;
            float py_ = (1.0f - dmy) * s_vpH;

            if (!s_dragging &&
                px_ >= s_geoPanelL && px_ <= s_geoPanelR &&
                py_ >= s_geoTitleB && py_ <= s_geoTitleT)
            {
                s_dragging  = 1;
                s_dragLastX = px_;
                s_dragLastY = py_;
            }
            if (s_dragging)
            {
                s_panelOfsX += px_ - s_dragLastX;
                s_panelOfsY += py_ - s_dragLastY;
                s_dragLastX  = px_;
                s_dragLastY  = py_;
                return; /* the drag owns the mouse this frame */
            }
        }
        else
        {
            s_dragging = 0;
        }
    }

    if (pageNext) qo_set_page(s_page + 1);
    if (pagePrev) qo_set_page(s_page - 1);
    rows = qo_page_rows(s_page, &nRows);

    if (up)   { s_sel = (s_sel + nRows - 1) % nRows; }
    if (down) { s_sel = (s_sel + 1) % nRows; }

    /* Mouse: hover selects, wheel adjusts, left click steps a value up (or
     * runs an action), right click steps it down. */
    mMoved  = Pc_MouseCursor_Moved();
    mClick  = Pc_MouseCursor_LeftClicked();
    mRClick = Pc_MouseCursor_RightClicked();
    wheel   = Pc_MouseCursor_WheelStep();
    if (Pc_MouseCursor_ViewportPos(&mx, &my) && s_geoRowPitch > 0.0f)
    {
        float py  = (1.0f - my) * s_vpH; /* top-left norm -> bottom-left px */
        float mpx = mx * s_vpW;
        int   row = (int)((s_geoListT - py) / s_geoRowPitch);
        int   inX = mpx >= s_geoPanelL && mpx <= s_geoPanelR;
        if (inX && row >= 0 && row < nRows)
        {
            if (mMoved) s_sel = row;
            if (mClick)
            {
                s_sel = row;
                if (qo_row_is_list(&rows[row]) && mpx > (s_geoPanelL + s_geoPanelR) * 0.5f)
                {
                    /* Right half of a list row: open the dropdown on the value. */
                    s_ddRow    = row;
                    s_ddSel    = Pc_Cheats_ListGet(rows[row].cpage, rows[row].extra);
                    s_ddScroll = s_ddSel - QO_DD_VISIBLE / 2;
                    if (s_ddScroll < 0) s_ddScroll = 0;
                    s_ddShown  = 0;
                }
                else
                    qo_confirm(&rows[row]);
            }
            if (mRClick && (QO_IS_VALUE_ROW(rows[row].kind) || rows[row].kind == ROW_PAGE))
            { s_sel = row; qo_activate(&rows[row], -1); }
            if (wheel && QO_IS_VALUE_ROW(rows[row].kind))
                qo_activate(&rows[row], wheel > 0 ? +1 : -1);
        }
    }

    /* The page row adjusts like a value: Left/right-click go back a page,
     * Right/confirm go forward. */
    if (QO_IS_VALUE_ROW(rows[s_sel].kind) || rows[s_sel].kind == ROW_PAGE)
    {
        if (left)  qo_activate(&rows[s_sel], -1);
        if (right) qo_activate(&rows[s_sel], +1);
        if (confirm) qo_confirm(&rows[s_sel]);
    }
    else if (confirm)
    {
        qo_confirm(&rows[s_sel]);
    }
}

static void qo_confirm(const QoRowDef* r)
{
    if (r->kind == ROW_CHEAT)
        Pc_Cheats_Confirm(r->cpage, r->extra);
    else
        qo_activate(r, +1);
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

void Pc_QuickOptions_Draw(void)
{
    GLint vp[4];
    float vpW, vpH, panelW, panelH, panelL, panelR, panelT, panelB;
    float titleH, hintH, listT, listB, listH, rowPitch, rowH, pad, dim = 1.0f;
    int   nRows;
    const QoRowDef* rows = qo_page_rows(s_page, &nRows);
    int   px, i;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_phase == QO_CLOSED)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    if (!s_glReady) qo_gl_init();
    if (s_glReady != 1) return;

    /* The atlas is created once and never deleted BY US -- but something else
     * frees its name. Binding a freed name creates a brand new EMPTY texture
     * object, and an empty texture samples as (0,0,0,1): opaque black over
     * every quad. That is the block corruption, and it survived the rewrite
     * because one texture can be lost exactly as easily as twenty-five.
     *
     * Checked BEFORE anything binds the atlas -- once a freed name is bound the
     * object springs back as a valid but EMPTY texture and glIsTexture can no
     * longer tell. One call per frame, and it does not stall.
     *
     * The panel background cannot show this by eye: it is drawn with a
     * near-black low-alpha colour, so white and opaque-black give the same dark
     * translucent panel. Only the text ever revealed it. */
    if (s_atlasTex != 0 && !glIsTexture(s_atlasTex))
    {
        static int s_lostLogs = 0;
        if (s_lostLogs < 8)
        {
            s_lostLogs++;
            SH_DBG("[QOTEX] atlas texture %u was DELETED by something else -- recreating",
                   (unsigned)s_atlasTex);
        }
        s_atlasTex   = 0;
        s_atlasReady = 0;
        qo_free_text();   /* every slot handle refers to the dead atlas */
        s_texWhite   = 0;
        s_texCursor  = 0;
        qo_atlas_ensure();
        qo_build_white();
    }
    if (!s_fontsTried) qo_fonts_init();

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    qo_gl_pump();
    qo_validate_cache();

    /* Fade only; the phase itself advances in Update (game thread). */
    {
        Uint32 age = SDL_GetTicks() - s_phaseStart;
        if (s_phase == QO_OPENING)
        {
            dim = (float)age / (float)QO_OPEN_MS;
            if (dim > 1.0f) dim = 1.0f;
        }
        else if (s_phase == QO_CLOSING)
        {
            dim = 1.0f - (float)age / (float)QO_CLOSE_MS;
            if (dim <= 0.0f) goto restore;
        }
    }

    /* Layout (viewport px, origin bottom-left). Left-anchored rather than
     * centred so the scene stays visible beside it while you tune. */
    panelH = 0.74f * vpH;
    panelW = 0.80f * panelH;
    if (panelW > 0.90f * vpW) panelW = 0.90f * vpW;
    panelL = vpW * 0.04f;
    panelB = (vpH - panelH) * 0.5f;

    /* User drag offset, clamped so a good part of the panel always stays on
     * screen (never let it be dragged fully out of reach). */
    {
        const float minL = -panelW * 0.75f;
        const float maxL = vpW - panelW * 0.25f;
        const float minB = -panelH * 0.75f;
        const float maxB = vpH - panelH * 0.25f;

        panelL += s_panelOfsX;
        panelB += s_panelOfsY;
        if (panelL < minL) { s_panelOfsX += minL - panelL; panelL = minL; }
        if (panelL > maxL) { s_panelOfsX += maxL - panelL; panelL = maxL; }
        if (panelB < minB) { s_panelOfsY += minB - panelB; panelB = minB; }
        if (panelB > maxB) { s_panelOfsY += maxB - panelB; panelB = maxB; }
    }

    panelR = panelL + panelW;
    panelT = panelB + panelH;

    pad      = panelW * 0.05f;
    titleH   = panelH * 0.09f;
    hintH    = panelH * 0.07f;
    listT    = panelT - titleH;
    listB    = panelB + hintH;
    listH    = listT - listB;
    rowPitch = listH / (float)nRows;
    rowH     = rowPitch * 0.84f;
    px       = (int)(rowH * 0.52f);
    if (px < 8) px = 8;

    if (s_bakedForPx != px || s_bakedForPage != s_page)
    {
        qo_free_text();
        s_bakedForPx   = px;
        s_bakedForPage = s_page;
    }
    if (!s_texTitle)
        s_texTitle = qo_bake(s_pageTitles[s_page], (float)(int)(titleH * 0.46f), &s_titleW, &s_titleH);
    /* Controls footer. It used to run off-screen at some panel widths, so bake
     * it once at the natural size and, if it overflows, re-bake once scaled to
     * fit -- the text always ends up inside the panel whatever its width. */
    if (!s_texHint)
    {
        char  hint[192];
        float avail = panelW - 2.0f * pad;
        int   hpx   = (int)(hintH * 0.42f);

        if (hpx < 7) hpx = 7;
        snprintf(hint, sizeof(hint),
                 "Up/Down select   Left/Right adjust   PgUp/PgDn page   drag title to move   %s or Esc close   * req restart",
                 g_PcConfig.keyQuickOptions[0] ? g_PcConfig.keyQuickOptions : "F10");
        s_texHint = qo_bake(hint, (float)hpx, &s_hintW, &s_hintH);
        if (s_texHint && s_hintW > avail && s_hintW > 0 && avail > 0.0f)
        {
            int fit = (int)((float)hpx * avail / (float)s_hintW);
            if (fit < 6)   fit = 6;
            if (fit > hpx) fit = hpx;
            s_texHint = qo_bake(hint, (float)fit, &s_hintW, &s_hintH);
        }
    }

    /* Publish geometry for Update's mouse hit-test. */
    s_vpW = vpW; s_vpH = vpH;
    s_geoListT = listT; s_geoRowPitch = rowPitch; s_geoRowH = rowH;
    s_geoPanelL = panelL; s_geoPanelR = panelR; s_geoRows = nRows;
    s_geoTitleT = panelT; s_geoTitleB = panelT - titleH;

    qo_build_white();

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* Re-assert the vertex layout every draw instead of trusting the VAO to
     * have preserved it.
     *
     * The shader is texture2D(u_tex, v_uv) * u_color, so everything depends on
     * a_uv (attribute 1). If that array is left disabled or repointed by other
     * GL code, v_uv collapses to a single value and every vertex samples ONE
     * texel -- and when that texel sits inside a glyph, the quad paints a solid
     * rectangle in u_color: the grey and red blotches, exactly the size of each
     * label, with the geometry still perfect because a_pos (attribute 0) is
     * unaffected.
     *
     * This is why every texture-side check came back clean: the name, size,
     * pixels and sampler state were all genuinely correct. It is also why the
     * panel background never showed it -- that quad samples a 1x1 texture,
     * where ANY uv resolves to the same texel, so broken uvs are invisible on
     * it and visible on everything else. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

#define NX(px_) (-1.0f + 2.0f * (px_) / vpW)
#define NY(py_) (-1.0f + 2.0f * (py_) / vpH)

    /* Translucent panel -- no screen dim, so a change shows in the live scene. */
    qo_quad(s_texWhite, NX(panelL), NY(panelT), NX(panelR), NY(panelB), 0.04f, 0.04f, 0.05f, 0.58f * dim);
    /* Title band + rule. */
    qo_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - 2.0f), NX(panelR - 2.0f), NY(listT),
            0.10f, 0.05f, 0.05f, 0.70f * dim);
    qo_quad(s_texWhite, NX(panelL + 2.0f), NY(listT), NX(panelR - 2.0f), NY(listT - 2.0f),
            0.47f, 0.11f, 0.08f, dim);
    if (s_texTitle)
    {
        float tx = panelL + (panelW - (float)s_titleW) * 0.5f;
        float ty = panelT - titleH * 0.30f;
        qo_quad(s_texTitle, NX(tx), NY(ty), NX(tx + s_titleW), NY(ty - s_titleH), 1.0f, 0.93f, 0.86f, dim);
    }

    for (i = 0; i < nRows && i < QO_MAX_ROWS; i++)
    {
        const QoRowDef* r = &rows[i];
        float rowTop = listT - (float)i * rowPitch;
        float rowMid = rowTop - rowH * 0.5f;
        float tH, tY;
        char  txt[64];

        if (i == s_sel)
            qo_quad(s_texWhite, NX(panelL + 4.0f), NY(rowTop), NX(panelR - 4.0f), NY(rowTop - rowH),
                    0.42f, 0.16f, 0.12f, 0.55f * dim);

        if (!s_texLabel[i])
        {
            qo_row_name(r, txt, (int)sizeof(txt));
            s_texLabel[i] = qo_bake(txt, (float)px, &s_labelW[i], &s_labelH[i]);
        }

        if (QO_IS_VALUE_ROW(r->kind))
        {
            qo_row_value(r, txt, (int)sizeof(txt));
            if (s_texValue[i] == 0 || strcmp(txt, s_valueText[i]) != 0)
            {
                qo_retire(s_texValue[i]);
                snprintf(s_valueText[i], sizeof(s_valueText[i]), "%s", txt);
                s_texValue[i] = qo_bake(txt, (float)px, &s_valueW[i], &s_valueH[i]);
            }
            /* Label left. A list row's label is a BUTTON (the action lives on
             * it; the value on the right only browses), drawn as a boxed pill. */
            if (s_texLabel[i])
            {
                tH = (float)s_labelH[i]; tY = rowMid + tH * 0.5f;
                if (qo_row_is_list(r))
                {
                    float bl = panelL + pad - 6.0f, br = panelL + pad + (float)s_labelW[i] + 6.0f;
                    float bt = rowTop - rowH * 0.08f, bb = rowTop - rowH * 0.92f;
                    float sel = (i == s_sel) ? 1.0f : 0.6f;
                    qo_quad(s_texWhite, NX(bl), NY(bt), NX(br), NY(bb), 0.47f, 0.11f, 0.08f, 0.55f * sel * dim);
                    qo_quad(s_texWhite, NX(bl), NY(bt), NX(br), NY(bt - 1.0f), 1.0f, 0.93f, 0.86f, 0.35f * dim);
                    qo_quad(s_texWhite, NX(bl), NY(bb + 1.0f), NX(br), NY(bb), 1.0f, 0.93f, 0.86f, 0.35f * dim);
                    qo_quad(s_texWhite, NX(bl), NY(bt), NX(bl + 1.0f), NY(bb), 1.0f, 0.93f, 0.86f, 0.35f * dim);
                    qo_quad(s_texWhite, NX(br - 1.0f), NY(bt), NX(br), NY(bb), 1.0f, 0.93f, 0.86f, 0.35f * dim);
                }
                qo_quad(s_texLabel[i], NX(panelL + pad), NY(tY), NX(panelL + pad + s_labelW[i]), NY(tY - tH),
                        0.92f, 0.92f, 0.95f, dim);
            }
            if (s_texValue[i])
            {
                float vg = (i == s_sel) ? 1.0f : 0.85f;
                float vr = panelR - pad - (float)s_valueW[i];
                tH = (float)s_valueH[i]; tY = rowMid + tH * 0.5f;
                qo_quad(s_texValue[i], NX(vr), NY(tY), NX(vr + s_valueW[i]), NY(tY - tH),
                        vg, vg * 0.85f, vg * 0.45f, dim);
            }
        }
        else if (s_texLabel[i])
        {
            /* Action row, centred. */
            float lx = panelL + (panelW - (float)s_labelW[i]) * 0.5f;
            tH = (float)s_labelH[i]; tY = rowMid + tH * 0.5f;
            qo_quad(s_texLabel[i], NX(lx), NY(tY), NX(lx + s_labelW[i]), NY(tY - tH),
                    0.80f, 0.85f, 0.95f, dim);
        }
    }

    if (s_texHint)
    {
        float hx = panelL + (panelW - (float)s_hintW) * 0.5f;
        float hy = panelB + hintH * 0.72f;
        qo_quad(s_texHint, NX(hx), NY(hy), NX(hx + s_hintW), NY(hy - s_hintH), 0.7f, 0.7f, 0.75f, dim);
    }

    /* Dropdown list over the value column of its row, on top of the rows. */
    s_ddShown = 0;
    if (s_ddRow >= 0 && s_ddRow < nRows && qo_row_is_list(&rows[s_ddRow]))
    {
        const QoRowDef* r  = &rows[s_ddRow];
        int   n   = Pc_Cheats_ListCount(r->cpage, r->extra);
        int   vis = (n < QO_DD_VISIBLE) ? n : QO_DD_VISIBLE;
        float ddL = panelL + panelW * 0.5f;
        float ddR = panelR - 4.0f;
        float ddTop = listT - (float)(s_ddRow + 1) * rowPitch + rowPitch * 0.08f;
        float ddH = rowH;
        int   k;

        if (n > QO_DD_MAX) n = QO_DD_MAX;
        if (s_ddScroll > n - vis) s_ddScroll = n - vis;
        if (s_ddScroll < 0) s_ddScroll = 0;
        /* Don't run off the panel bottom: open upward instead. */
        if (ddTop - vis * ddH < panelB) ddTop = listT - (float)s_ddRow * rowPitch + vis * ddH;

        qo_quad(s_texWhite, NX(ddL - 2.0f), NY(ddTop + 2.0f), NX(ddR + 2.0f), NY(ddTop - vis * ddH - 2.0f), 0.47f, 0.11f, 0.08f, dim);
        qo_quad(s_texWhite, NX(ddL), NY(ddTop), NX(ddR), NY(ddTop - vis * ddH), 0.05f, 0.045f, 0.06f, 0.97f * dim);
        for (k = 0; k < vis; k++)
        {
            int   e  = s_ddScroll + k;
            float et = ddTop - (float)k * ddH;
            if (e >= n) break;
            if (e == s_ddSel)
                qo_quad(s_texWhite, NX(ddL + 2.0f), NY(et), NX(ddR - 2.0f), NY(et - ddH), 0.42f, 0.16f, 0.12f, 0.75f * dim);
            if (!s_ddTex[e])
                s_ddTex[e] = qo_bake(Pc_Cheats_ListName(r->cpage, r->extra, e), (float)px, &s_ddW[e], &s_ddH[e]);
            if (s_ddTex[e])
            {
                float ty = et - (ddH - (float)s_ddH[e]) * 0.5f;
                qo_quad(s_ddTex[e], NX(ddL + 10.0f), NY(ty), NX(ddL + 10.0f + s_ddW[e]), NY(ty - s_ddH[e]),
                        e == s_ddSel ? 1.0f : 0.85f, 0.9f, 0.9f, dim);
            }
        }
        s_ddL = ddL; s_ddR = ddR; s_ddTop = ddTop; s_ddRowH = ddH;
        s_ddShown = 1;
    }

    /* Mouse cursor last, so it rides above the panel (the PSX-drawn one is
     * composited under every overlay). Nearest keeps the sprite's pixel look. */
    if (!s_texCursor)
    {
        unsigned char rgba[32 * 32 * 4];
        if (Pc_MouseCursor_SpriteRgba(rgba))
            s_texCursor = qo_upload_rgba(rgba, 32, 32, 1);
    }
    if (s_texCursor)
    {
        float cx, cy, cw, ch;
        if (Pc_MouseCursor_GlRect(vpW, vpH, &cx, &cy, &cw, &ch))
            qo_quad(s_texCursor, NX(cx), NY(cy), NX(cx + cw), NY(cy - ch), 1.0f, 1.0f, 1.0f, 1.0f);
    }

#undef NX
#undef NY

restore:
    /* Restore GL state so PsyCross / the console are untouched. */
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    /* PsyCross keeps a redundant-bind cache and RETURNS EARLY from
     * GR_SetTexture when it believes the texture is already bound. We just
     * bound our own atlas behind that cache, so the binding it restores
     * above is not necessarily what the cache thinks. Invalidate it and the
     * next GR_SetTexture is forced to issue a real glBindTexture. */
    {
        extern unsigned int g_lastBoundTexture; /* PsyX TextureID */
        g_lastBoundTexture = (unsigned int)-1;
    }
}
