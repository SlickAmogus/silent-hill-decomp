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

#include "stb_truetype.h"

#include "pc_quick_options.h"
#include "pc_mouse_cursor.h"
#include "pc_config.h"
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
#define QO_PAGES    2

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

enum { ROW_OPT = 0, ROW_EXTRA, ROW_PAGE, ROW_CLOSE };

typedef struct
{
    int         kind;   /* ROW_* */
    const char* key;    /* ROW_OPT: config key looked up in options.c */
    int         extra;  /* ROW_EXTRA: QO_X_* */
    const char* label;  /* ROW_EXTRA / ROW_PAGE / ROW_CLOSE display name */
} QoRowDef;

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
    { ROW_EXTRA, NULL, QO_X_SPEAKERS,       "Speaker Layout" },
    { ROW_EXTRA, NULL, QO_X_BGM,            "Music Volume" },
    { ROW_EXTRA, NULL, QO_X_SFX,            "Effects Volume" },
    { ROW_OPT,   "fmv_volume",           0, NULL },
    { ROW_PAGE,  NULL, 0,                   "Next page  (Graphics)" },
    { ROW_CLOSE, NULL, 0,                   "Close" },
};

static const QoRowDef* qo_page_rows(int page, int* count)
{
    if (page == 1) { *count = (int)(sizeof(s_page1) / sizeof(s_page1[0])); return s_page1; }
    *count = (int)(sizeof(s_page0) / sizeof(s_page0[0]));
    return s_page0;
}

static const char* const s_pageTitles[QO_PAGES] = { "QUICK OPTIONS  -  GRAPHICS", "QUICK OPTIONS  -  HUD & AUDIO" };

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
static int   s_geoRows;

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

static GLuint qo_upload_rgba(const unsigned char* rgba, int w, int h, int nearest)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void qo_retire(GLuint tex)
{
    if (tex && s_garbageCount < QO_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void qo_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Quad in NDC (yTop > yBot). */
static void qo_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    if (!tex)
        return;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = 0; v[0][3] = 0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = 0; v[1][3] = 1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = 1; v[2][3] = 0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = 1; v[3][3] = 0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = 0; v[4][3] = 1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = 1; v[5][3] = 1;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

static unsigned char* qo_read_file(const char* path)
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
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
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
        unsigned char* data = qo_read_file(paths[i]);
        if (!data)
            continue;
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData = data; /* stbtt keeps pointers into this - never free */
            s_fontOk   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[QUICKOPT] no usable font - text disabled");
}

/* Rasterize one line of ASCII to a white-coverage RGBA texture, trimmed to
 * the ink so right-alignment and centring use the real text width. */
static GLuint qo_bake(const char* text, float px, int* outW, int* outH)
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
    tex = qo_upload_rgba(rgba, W, H, 0);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static void qo_build_white(void)
{
    unsigned char one[4] = { 255, 255, 255, 255 };
    if (!s_texWhite)
        s_texWhite = qo_upload_rgba(one, 1, 1, 0);
}

static void qo_free_text(void)
{
    int i;
    qo_retire(s_texTitle); s_texTitle = 0;
    qo_retire(s_texHint);  s_texHint  = 0;
    for (i = 0; i < QO_MAX_ROWS; i++)
    {
        qo_retire(s_texLabel[i]); s_texLabel[i] = 0;
        qo_retire(s_texValue[i]); s_texValue[i] = 0;
        s_valueText[i][0] = 0;
    }
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
    s_page = (page + QO_PAGES) % QO_PAGES;
    qo_page_rows(s_page, &n);
    if (s_sel >= n) s_sel = n - 1;
    if (s_sel < 0)  s_sel = 0;
}

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
        case ROW_PAGE:  qo_set_page(s_page + 1); break;
        case ROW_CLOSE: Pc_QuickOptions_Close(); break;
        default: break;
    }
}

void Pc_QuickOptions_Update(int up, int down, int left, int right,
                            int confirm, int close, int pageNext, int pagePrev)
{
    int nRows;
    const QoRowDef* rows = qo_page_rows(s_page, &nRows);
    int mMoved, mClick, wheel;
    float mx, my;

    qo_phase_tick();

    /* Keyboard extras (arrows arrive through the pad emulation already). */
    if (qo_key_edge(SDL_SCANCODE_ESCAPE))   close    = 1;
    if (qo_key_edge(SDL_SCANCODE_PAGEDOWN) || qo_key_edge(SDL_SCANCODE_E)) pageNext = 1;
    if (qo_key_edge(SDL_SCANCODE_PAGEUP)   || qo_key_edge(SDL_SCANCODE_Q)) pagePrev = 1;

    if (s_phase != QO_SHOWN) /* ignore input while animating in/out */
        return;

    if (close) { Pc_QuickOptions_Close(); return; }

    if (pageNext) qo_set_page(s_page + 1);
    if (pagePrev) qo_set_page(s_page - 1);
    rows = qo_page_rows(s_page, &nRows);

    if (up)   { s_sel = (s_sel + nRows - 1) % nRows; }
    if (down) { s_sel = (s_sel + 1) % nRows; }

    /* Mouse: hover selects, wheel adjusts, click cycles a value / runs an action. */
    mMoved = Pc_MouseCursor_Moved();
    mClick = Pc_MouseCursor_LeftClicked();
    wheel  = Pc_MouseCursor_WheelStep();
    if (Pc_MouseCursor_ViewportPos(&mx, &my) && s_geoRowPitch > 0.0f)
    {
        float py  = (1.0f - my) * s_vpH; /* top-left norm -> bottom-left px */
        float mpx = mx * s_vpW;
        int   row = (int)((s_geoListT - py) / s_geoRowPitch);
        int   inX = mpx >= s_geoPanelL && mpx <= s_geoPanelR;
        if (inX && row >= 0 && row < nRows)
        {
            if (mMoved) s_sel = row;
            if (mClick) { s_sel = row; qo_activate(&rows[row], +1); }
            if (wheel && (rows[row].kind == ROW_OPT || rows[row].kind == ROW_EXTRA))
                qo_activate(&rows[row], wheel > 0 ? +1 : -1);
        }
    }

    if (rows[s_sel].kind == ROW_OPT || rows[s_sel].kind == ROW_EXTRA)
    {
        if (left)  qo_activate(&rows[s_sel], -1);
        if (right) qo_activate(&rows[s_sel], +1);
        if (confirm) qo_activate(&rows[s_sel], +1);
    }
    else if (confirm)
    {
        qo_activate(&rows[s_sel], +1);
    }
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
    panelR = panelL + panelW;
    panelB = (vpH - panelH) * 0.5f;
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
    if (!s_texHint)
    {
        char hint[160];
        snprintf(hint, sizeof(hint),
                 "Up/Down select   Left/Right adjust   PgUp/PgDn page   %s or Esc close   * = restart",
                 g_PcConfig.keyQuickOptions[0] ? g_PcConfig.keyQuickOptions : "F10");
        s_texHint = qo_bake(hint, (float)(int)(hintH * 0.50f), &s_hintW, &s_hintH);
    }

    /* Publish geometry for Update's mouse hit-test. */
    s_vpW = vpW; s_vpH = vpH;
    s_geoListT = listT; s_geoRowPitch = rowPitch; s_geoRowH = rowH;
    s_geoPanelL = panelL; s_geoPanelR = panelR; s_geoRows = nRows;

    qo_build_white();

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
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

        if (r->kind == ROW_OPT || r->kind == ROW_EXTRA)
        {
            qo_row_value(r, txt, (int)sizeof(txt));
            if (s_texValue[i] == 0 || strcmp(txt, s_valueText[i]) != 0)
            {
                qo_retire(s_texValue[i]);
                snprintf(s_valueText[i], sizeof(s_valueText[i]), "%s", txt);
                s_texValue[i] = qo_bake(txt, (float)px, &s_valueW[i], &s_valueH[i]);
            }
            if (s_texLabel[i])
            {
                tH = (float)s_labelH[i]; tY = rowMid + tH * 0.5f;
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
}
