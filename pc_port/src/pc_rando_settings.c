/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_rando_settings.c - in-game randomizer settings panel.
 *
 * A sibling of pc_ra_browser.c and it repeats that file's hard-won GL
 * constraints deliberately rather than sharing code:
 *   - its OWN GL program / VAO / textures, with full state save+restore, so it
 *     never clobbers PsyCross's or the console's programs.
 *   - LEGACY GLSL (attribute / varying / gl_FragColor, no #version).
 *   - all GL work deferred to Draw (post-capture); the game thread only reads
 *     input and toggles state.
 *   - wall-clock timing (the draw hook runs after the frame is captured).
 *
 * Text is stb_truetype, baked to small textures. The toast owns the only
 * stb_truetype implementation in the build; this file is a consumer.
 *
 * Rows: one per tunable (pc_rando_config.c descriptor table), then two action
 * rows. Up/Down move the selection; Left/Right adjust the selected tunable;
 * Confirm activates an action; the Map button (or Cancel) closes.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>

#include "stb_truetype.h"

#include "pc_rando_settings.h"
#include "pc_rando_config.h"
#include "pc_mouse_cursor.h"
#include "sh_log.h"

#define RS_GARBAGE 32

/* Action rows, after the tunables. */
enum { RS_ACT_RESET = 0, RS_ACT_CLOSE, RS_ACT_COUNT };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { RS_CLOSED = 0, RS_OPENING, RS_SHOWN, RS_CLOSING };
static int    s_phase;
static Uint32 s_phaseStart;
#define RS_OPEN_MS  200u
#define RS_CLOSE_MS 160u

static int s_sel;       /* selected row (0..nRows-1) */
static int s_changed;   /* a value was edited this session -> save on close */

/* GL */
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locColor;
static int    s_glReady;
static GLuint s_texWhite;
static GLuint s_garbage[RS_GARBAGE];
static int    s_garbageCount;

/* Fonts (stb_truetype). */
static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontsTried;

/* Baked text, keyed to the pixel size it was baked at (re-baked on resize). */
#define RS_MAX_ROWS 32
static GLuint s_texTitle,  s_texHint;
static int    s_titleW, s_titleH, s_hintW, s_hintH;
static GLuint s_texLabel[RS_MAX_ROWS];
static int    s_labelW[RS_MAX_ROWS], s_labelH[RS_MAX_ROWS];
static GLuint s_texValue[RS_MAX_ROWS];
static int    s_valueW[RS_MAX_ROWS], s_valueH[RS_MAX_ROWS], s_valueFor[RS_MAX_ROWS];
static int    s_bakedForPx;      /* font px the static labels were baked at */

/* Geometry published by Draw for Update's mouse hit-test (viewport px, y up). */
static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static float s_geoListT, s_geoRowPitch, s_geoRowH, s_geoPanelL, s_geoPanelR;
static int   s_geoRows;

static int rs_row_count(void)
{
    return Pc_RandoConfig_Count() + RS_ACT_COUNT;
}

/* ------------------------------------------------------------------ */
/* GL primitives (mirrors pc_ra_browser.c)                             */
/* ------------------------------------------------------------------ */

static GLuint rs_make_shader(GLenum type, const char* src)
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
        SH_DBG("[RANDOUI] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void rs_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_glReady = -1;
    vs = rs_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = rs_make_shader(GL_FRAGMENT_SHADER, fs_src);
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
        SH_DBG("[RANDOUI] link failed: %s", log);
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

static GLuint rs_upload_rgba(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void rs_retire(GLuint tex)
{
    if (tex && s_garbageCount < RS_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void rs_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Quad in NDC (yTop > yBot). */
static void rs_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
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

static unsigned char* rs_read_file(const char* path)
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

static void rs_fonts_init(void)
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
        unsigned char* data = rs_read_file(paths[i]);
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
    SH_DBG("[RANDOUI] no usable font - text disabled");
}

/* Rasterize one line of ASCII to a white-coverage RGBA texture. */
static GLuint rs_bake(const char* text, float px, int* outW, int* outH)
{
    const char* p;
    float scale, penX, baseY;
    int   asc, desc, gap, W, H, i, pad = 1, prev = 0;
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

    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    tex = rs_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static void rs_build_white(void)
{
    unsigned char one[4] = { 255, 255, 255, 255 };
    if (!s_texWhite)
        s_texWhite = rs_upload_rgba(one, 1, 1);
}

/* Drop every baked text texture (viewport resize -> re-bake at the new size). */
static void rs_free_text(void)
{
    int i, n = Pc_RandoConfig_Count();
    rs_retire(s_texTitle); s_texTitle = 0;
    rs_retire(s_texHint);  s_texHint  = 0;
    for (i = 0; i < n && i < RS_MAX_ROWS; i++)
    {
        rs_retire(s_texLabel[i]); s_texLabel[i] = 0;
        rs_retire(s_texValue[i]); s_texValue[i] = 0;
        s_valueFor[i] = -0x7fffffff;
    }
    for (i = 0; i < RS_ACT_COUNT; i++)
    {
        int r = n + i;
        if (r < RS_MAX_ROWS) { rs_retire(s_texLabel[r]); s_texLabel[r] = 0; }
    }
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

int Pc_RandoSettings_IsOpen(void)
{
    return s_phase != RS_CLOSED;
}

void Pc_RandoSettings_Open(void)
{
    if (s_phase == RS_OPENING || s_phase == RS_SHOWN)
        return;
    s_phase      = RS_OPENING;
    s_phaseStart = SDL_GetTicks();
    s_sel        = 0;
    s_changed    = 0;
}

void Pc_RandoSettings_Close(void)
{
    if (s_phase == RS_CLOSED || s_phase == RS_CLOSING)
        return;
    if (s_changed)
        Pc_RandoConfig_Save();
    s_phase      = RS_CLOSING;
    s_phaseStart = SDL_GetTicks();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void rs_activate_action(int act)
{
    switch (act)
    {
        case RS_ACT_RESET:
            Pc_RandoConfig_ResetDefaults();
            s_changed = 1;
            break;
        case RS_ACT_CLOSE:
            Pc_RandoSettings_Close();
            break;
        default:
            break;
    }
}

void Pc_RandoSettings_Update(int up, int down, int left, int right, int confirm, int close)
{
    int nRows = rs_row_count();
    int nSet  = Pc_RandoConfig_Count();
    int mMoved, mClick, wheel;
    float mx, my;

    if (s_phase != RS_SHOWN) /* ignore input while animating in/out */
        return;

    if (close) { Pc_RandoSettings_Close(); return; }

    if (up)   { s_sel = (s_sel + nRows - 1) % nRows; }
    if (down) { s_sel = (s_sel + 1) % nRows; }

    /* Mouse: hover selects, wheel adjusts, left-click activates an action. */
    mMoved = Pc_MouseCursor_Moved();
    mClick = Pc_MouseCursor_LeftClicked();
    wheel  = Pc_MouseCursor_WheelStep();
    if (Pc_MouseCursor_ViewportPos(&mx, &my) && s_geoRowPitch > 0.0f)
    {
        float py  = (1.0f - my) * s_vpH;            /* top-left norm -> bottom-left px */
        float mpx = mx * s_vpW;
        int   row = (int)((s_geoListT - py) / s_geoRowPitch);
        int   inX = mpx >= s_geoPanelL && mpx <= s_geoPanelR;
        if (inX && row >= 0 && row < nRows)
        {
            if (mMoved) s_sel = row;
            if (mClick && row >= nSet) rs_activate_action(row - nSet);
            if (wheel && row < nSet) { Pc_RandoConfig_Adjust(row, wheel); s_changed = 1; }
        }
    }

    if (s_sel < nSet)
    {
        if (left)  { Pc_RandoConfig_Adjust(s_sel, -1); s_changed = 1; }
        if (right) { Pc_RandoConfig_Adjust(s_sel, +1); s_changed = 1; }
    }
    else if (confirm)
    {
        rs_activate_action(s_sel - nSet);
    }
}

/* Tap opens the panel; a hold past the threshold wants the real map. */
#define RS_HOLD_MS 260u
int Pc_RandoSettings_MapButtonArbiter(int mapClicked, int mapHeld)
{
    static int    s_pending, s_consumed;
    static Uint32 s_pressMs;
    Uint32 now = SDL_GetTicks();

    if (Pc_RandoSettings_IsOpen())
        return RANDO_MAP_NONE;

    if (mapClicked && !s_pending)
    {
        s_pending  = 1;
        s_consumed = 0;
        s_pressMs  = now;
    }
    if (s_pending)
    {
        if (mapHeld)
        {
            if (!s_consumed && (now - s_pressMs) >= RS_HOLD_MS)
            {
                s_consumed = 1;
                return RANDO_MAP_WANT_MAP;
            }
        }
        else /* released */
        {
            int wasTap = !s_consumed;
            s_pending = 0;
            if (wasTap)
            {
                Pc_RandoSettings_Open();
                return RANDO_MAP_OPENED_SETTINGS;
            }
        }
    }
    return RANDO_MAP_NONE;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

void Pc_RandoSettings_Draw(void)
{
    GLint vp[4];
    float vpW, vpH, panelW, panelH, panelL, panelR, panelT, panelB;
    float titleH, hintH, listT, listB, listH, rowPitch, rowH, pad, dim = 1.0f;
    int   nRows = rs_row_count();
    int   nSet  = Pc_RandoConfig_Count();
    int   px, i;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_phase == RS_CLOSED)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    if (!s_glReady) rs_gl_init();
    if (s_glReady != 1) return;
    if (!s_fontsTried) rs_fonts_init();

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

    rs_gl_pump();

    /* Phase advance + fade. Wall clock: the hook runs post-capture. */
    {
        Uint32 age = SDL_GetTicks() - s_phaseStart;
        if (s_phase == RS_OPENING)
        {
            dim = (float)age / (float)RS_OPEN_MS;
            if (dim >= 1.0f) { dim = 1.0f; s_phase = RS_SHOWN; }
        }
        else if (s_phase == RS_CLOSING)
        {
            dim = 1.0f - (float)age / (float)RS_CLOSE_MS;
            if (dim <= 0.0f) { s_phase = RS_CLOSED; return; }
        }
    }

    /* Layout (viewport px, origin bottom-left). */
    panelH = 0.78f * vpH;
    panelW = 0.92f * panelH;
    if (panelW > 0.90f * vpW) panelW = 0.90f * vpW;
    panelL = (vpW - panelW) * 0.5f;
    panelR = panelL + panelW;
    panelB = (vpH - panelH) * 0.5f;
    panelT = panelB + panelH;

    pad      = panelW * 0.05f;
    titleH   = panelH * 0.10f;
    hintH    = panelH * 0.06f;
    listT    = panelT - titleH;
    listB    = panelB + hintH;
    listH    = listT - listB;
    rowPitch = listH / (float)nRows;
    rowH     = rowPitch * 0.84f;
    px       = (int)(rowH * 0.52f);
    if (px < 8) px = 8;

    /* Re-bake all static text when the pixel size changes (resize / first run). */
    if (s_bakedForPx != px)
    {
        rs_free_text();
        s_bakedForPx = px;
    }
    if (!s_texTitle)
        s_texTitle = rs_bake("RANDOMIZER SETTINGS", (float)(int)(titleH * 0.46f), &s_titleW, &s_titleH);
    if (!s_texHint)
        s_texHint = rs_bake("Up/Down select   Left/Right adjust   [OK] apply   [Map] close",
                            (float)(int)(hintH * 0.62f), &s_hintW, &s_hintH);

    /* Publish geometry for Update's mouse hit-test. */
    s_vpW = vpW; s_vpH = vpH;
    s_geoListT = listT; s_geoRowPitch = rowPitch; s_geoRowH = rowH;
    s_geoPanelL = panelL; s_geoPanelR = panelR; s_geoRows = nRows;

    rs_build_white();

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

    /* Dim the world behind the panel. */
    rs_quad(s_texWhite, NX(0.0f), NY(vpH), NX(vpW), NY(0.0f), 0.0f, 0.0f, 0.0f, 0.62f * dim);
    /* Panel body. */
    rs_quad(s_texWhite, NX(panelL), NY(panelT), NX(panelR), NY(panelB), 0.055f, 0.05f, 0.065f, 0.95f * dim);
    /* Title band + rule. */
    rs_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - 2.0f), NX(panelR - 2.0f), NY(listT),
            0.10f, 0.05f, 0.05f, 0.95f * dim);
    rs_quad(s_texWhite, NX(panelL + 2.0f), NY(listT), NX(panelR - 2.0f), NY(listT - 2.0f),
            0.47f, 0.11f, 0.08f, dim);
    if (s_texTitle)
    {
        float tx = panelL + (panelW - (float)s_titleW) * 0.5f;
        float ty = panelT - titleH * 0.30f;
        rs_quad(s_texTitle, NX(tx), NY(ty), NX(tx + s_titleW), NY(ty - s_titleH), 1.0f, 0.93f, 0.86f, dim);
    }

    for (i = 0; i < nRows; i++)
    {
        float rowTop = listT - (float)i * rowPitch;
        float rowMid = rowTop - rowH * 0.5f;
        float tH, tY;

        if (i == s_sel)
            rs_quad(s_texWhite, NX(panelL + 4.0f), NY(rowTop), NX(panelR - 4.0f), NY(rowTop - rowH),
                    0.42f, 0.16f, 0.12f, 0.55f * dim);

        if (i >= RS_MAX_ROWS)
            continue;

        if (i < nSet)
        {
            const s_RandoSetting* st = Pc_RandoConfig_At(i);
            if (!st) continue;
            if (!s_texLabel[i])
                s_texLabel[i] = rs_bake(st->label, (float)px, &s_labelW[i], &s_labelH[i]);
            if (s_texValue[i] == 0 || s_valueFor[i] != *st->value)
            {
                char buf[32];
                rs_retire(s_texValue[i]);
                snprintf(buf, sizeof(buf), "%d%s", *st->value, st->suffix);
                s_texValue[i] = rs_bake(buf, (float)px, &s_valueW[i], &s_valueH[i]);
                s_valueFor[i] = *st->value;
            }
            /* Label left. */
            if (s_texLabel[i])
            {
                tH = (float)s_labelH[i]; tY = rowMid + tH * 0.5f;
                rs_quad(s_texLabel[i], NX(panelL + pad), NY(tY), NX(panelL + pad + s_labelW[i]), NY(tY - tH),
                        0.92f, 0.92f, 0.95f, dim);
            }
            /* Value right, brighter when selected. */
            if (s_texValue[i])
            {
                float vg = (i == s_sel) ? 1.0f : 0.85f;
                float vr = panelR - pad - (float)s_valueW[i];
                tH = (float)s_valueH[i]; tY = rowMid + tH * 0.5f;
                rs_quad(s_texValue[i], NX(vr), NY(tY), NX(vr + s_valueW[i]), NY(tY - tH),
                        vg, vg * 0.85f, vg * 0.45f, dim);
            }
        }
        else
        {
            /* Action row, centred. */
            static const char* const ACT_LABELS[RS_ACT_COUNT] = { "Reset to defaults", "Close" };
            int a = i - nSet;
            if (!s_texLabel[i])
                s_texLabel[i] = rs_bake(ACT_LABELS[a], (float)px, &s_labelW[i], &s_labelH[i]);
            if (s_texLabel[i])
            {
                float lx = panelL + (panelW - (float)s_labelW[i]) * 0.5f;
                tH = (float)s_labelH[i]; tY = rowMid + tH * 0.5f;
                rs_quad(s_texLabel[i], NX(lx), NY(tY), NX(lx + s_labelW[i]), NY(tY - tH),
                        0.80f, 0.85f, 0.95f, dim);
            }
        }
    }

    if (s_texHint)
    {
        float hx = panelL + (panelW - (float)s_hintW) * 0.5f;
        float hy = panelB + hintH * 0.78f;
        rs_quad(s_texHint, NX(hx), NY(hy), NX(hx + s_hintW), NY(hy - s_hintH), 0.7f, 0.7f, 0.75f, dim);
    }

#undef NX
#undef NY

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
