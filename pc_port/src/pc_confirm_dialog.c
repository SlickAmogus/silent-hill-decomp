/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_confirm_dialog.c - modal Yes/No message box.
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
 * Text is stb_truetype, baked to small textures. The toast owns the only
 * stb_truetype implementation in the build; this file is a consumer.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>

#include "stb_truetype.h"

#include "pc_confirm_dialog.h"
#include "pc_mouse_cursor.h"
#include "sh_log.h"

#define CD_GARBAGE 16
#define CD_TEXT_MAX 128

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { CD_CLOSED = 0, CD_OPENING, CD_SHOWN, CD_CLOSING };
static int    s_phase;
static Uint32 s_phaseStart;
#define CD_OPEN_MS  180u
#define CD_CLOSE_MS 140u

static int  s_sel;          /* 0 = Yes, 1 = No */
static char s_title[CD_TEXT_MAX];
static char s_message[CD_TEXT_MAX];
static int  s_textDirty;    /* strings changed -> re-bake */

/* GL */
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locColor;
static int    s_glReady;
static GLuint s_texWhite;
static GLuint s_garbage[CD_GARBAGE];
static int    s_garbageCount;

/* Fonts (stb_truetype). */
static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontsTried;

/* Baked text, keyed to the pixel size it was baked at (re-baked on resize). */
static GLuint s_texTitle, s_texMsg, s_texYes, s_texNo, s_texHint;
static int    s_titleW, s_titleH, s_msgW, s_msgH, s_yesW, s_yesH, s_noW, s_noH, s_hintW, s_hintH;
static int    s_bakedForPx;

/* Geometry published by Draw for Update's mouse hit-test (viewport px, y up). */
static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static float s_geoBtnL[2], s_geoBtnR[2], s_geoBtnT, s_geoBtnB;
static int   s_geoValid;

/* ------------------------------------------------------------------ */
/* GL primitives (mirrors pc_rando_settings.c)                         */
/* ------------------------------------------------------------------ */

static GLuint cd_make_shader(GLenum type, const char* src)
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
        SH_DBG("[CONFIRM] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void cd_gl_init(void)
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
    vs = cd_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = cd_make_shader(GL_FRAGMENT_SHADER, fs_src);
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
        SH_DBG("[CONFIRM] link failed: %s", log);
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

static GLuint cd_upload_rgba(const unsigned char* rgba, int w, int h)
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

static void cd_retire(GLuint tex)
{
    if (tex && s_garbageCount < CD_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void cd_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Quad in NDC (yTop > yBot). */
static void cd_quad(GLuint tex, float x0, float yTop, float x1, float yBot,
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

static unsigned char* cd_read_file(const char* path)
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

static void cd_fonts_init(void)
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
        unsigned char* data = cd_read_file(paths[i]);
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
    SH_DBG("[CONFIRM] no usable font - text disabled");
}

/* Rasterize one line of ASCII to a white-coverage RGBA texture. */
static GLuint cd_bake(const char* text, float px, int* outW, int* outH)
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

    /* Trim the texture to the ink so centring uses the real text width; cov
     * keeps its allocation stride. */
    {
        int stride = W;
        int inkW   = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
        for (i = 0; i < W * H; i++)
        {
            rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
        }
    }
    tex = cd_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static void cd_build_white(void)
{
    unsigned char one[4] = { 255, 255, 255, 255 };
    if (!s_texWhite)
        s_texWhite = cd_upload_rgba(one, 1, 1);
}

static void cd_free_text(void)
{
    cd_retire(s_texTitle); s_texTitle = 0;
    cd_retire(s_texMsg);   s_texMsg   = 0;
    cd_retire(s_texYes);   s_texYes   = 0;
    cd_retire(s_texNo);    s_texNo    = 0;
    cd_retire(s_texHint);  s_texHint  = 0;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

int Pc_ConfirmDialog_IsOpen(void)
{
    return s_phase != CD_CLOSED;
}

void Pc_ConfirmDialog_Open(const char* title, const char* message)
{
    if (s_phase == CD_OPENING || s_phase == CD_SHOWN)
        return;
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    snprintf(s_message, sizeof(s_message), "%s", message ? message : "");
    s_textDirty  = 1;
    s_phase      = CD_OPENING;
    s_phaseStart = SDL_GetTicks();
    s_sel        = 1; /* "No" is the safe default */
    s_geoValid   = 0;
}

static void cd_close(void)
{
    s_phase      = CD_CLOSING;
    s_phaseStart = SDL_GetTicks();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

int Pc_ConfirmDialog_Update(int left, int right, int confirm, int cancel)
{
    int   mMoved, mClick;
    float mx, my;

    if (s_phase != CD_SHOWN) /* ignore input while animating in/out */
        return PC_CONFIRM_NONE;

    if (cancel) { cd_close(); return PC_CONFIRM_NO; }

    if (left)  s_sel = 0;
    if (right) s_sel = 1;

    /* Mouse: hover selects a button, left-click activates it. */
    mMoved = Pc_MouseCursor_Moved();
    mClick = Pc_MouseCursor_LeftClicked();
    if (s_geoValid && (mMoved || mClick) && Pc_MouseCursor_ViewportPos(&mx, &my))
    {
        float py  = (1.0f - my) * s_vpH; /* top-left norm -> bottom-left px */
        float mpx = mx * s_vpW;
        int   b;
        for (b = 0; b < 2; b++)
        {
            if (mpx >= s_geoBtnL[b] && mpx <= s_geoBtnR[b] && py <= s_geoBtnT && py >= s_geoBtnB)
            {
                if (mMoved) s_sel = b;
                if (mClick) { s_sel = b; confirm = 1; }
            }
        }
    }

    if (confirm)
    {
        cd_close();
        return s_sel == 0 ? PC_CONFIRM_YES : PC_CONFIRM_NO;
    }
    return PC_CONFIRM_NONE;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

void Pc_ConfirmDialog_Draw(void)
{
    GLint vp[4];
    float vpW, vpH, panelW, panelH, panelL, panelR, panelT, panelB;
    float titleH, hintH, pad, btnW, btnH, btnGap, btnT, btnB, btnL[2], dim = 1.0f;
    int   px, b;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_phase == CD_CLOSED)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    if (!s_glReady) cd_gl_init();
    if (s_glReady != 1) return;
    if (!s_fontsTried) cd_fonts_init();

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

    cd_gl_pump();

    /* Phase advance + fade. Wall clock: the hook runs post-capture. */
    {
        Uint32 age = SDL_GetTicks() - s_phaseStart;
        if (s_phase == CD_OPENING)
        {
            dim = (float)age / (float)CD_OPEN_MS;
            if (dim >= 1.0f) { dim = 1.0f; s_phase = CD_SHOWN; }
        }
        else if (s_phase == CD_CLOSING)
        {
            dim = 1.0f - (float)age / (float)CD_CLOSE_MS;
            if (dim <= 0.0f) { s_phase = CD_CLOSED; s_geoValid = 0; goto restore; }
        }
    }

    /* Layout (viewport px, origin bottom-left). Sized off the viewport height
     * so it scales with resolution; width stretches to fit the message. */
    panelH = 0.26f * vpH;
    titleH = panelH * 0.22f;
    hintH  = panelH * 0.16f;
    pad    = panelH * 0.16f;
    px     = (int)(panelH * 0.15f);
    if (px < 8) px = 8;

    if (s_bakedForPx != px || s_textDirty)
    {
        cd_free_text();
        s_bakedForPx = px;
        s_textDirty  = 0;
    }
    if (!s_texTitle) s_texTitle = cd_bake(s_title,   (float)(int)(titleH * 0.50f), &s_titleW, &s_titleH);
    if (!s_texMsg)   s_texMsg   = cd_bake(s_message, (float)px,                    &s_msgW,   &s_msgH);
    if (!s_texYes)   s_texYes   = cd_bake("Yes",     (float)px,                    &s_yesW,   &s_yesH);
    if (!s_texNo)    s_texNo    = cd_bake("No",      (float)px,                    &s_noW,    &s_noH);
    if (!s_texHint)  s_texHint  = cd_bake("Left/Right select   [OK] confirm   [Cancel] back",
                                          (float)(int)(hintH * 0.60f), &s_hintW, &s_hintH);

    panelW = 0.36f * vpW;
    if (panelW < (float)s_msgW + 2.0f * pad) panelW = (float)s_msgW + 2.0f * pad;
    if (panelW < (float)s_hintW + 2.0f * pad) panelW = (float)s_hintW + 2.0f * pad;
    if (panelW > 0.90f * vpW) panelW = 0.90f * vpW;
    panelL = (vpW - panelW) * 0.5f;
    panelR = panelL + panelW;
    panelB = (vpH - panelH) * 0.5f;
    panelT = panelB + panelH;

    /* Two equal buttons centred in the lower half, above the hint line. */
    btnH   = panelH * 0.20f;
    btnW   = panelW * 0.22f;
    btnGap = panelW * 0.06f;
    btnB   = panelB + hintH + panelH * 0.08f;
    btnT   = btnB + btnH;
    btnL[0] = (vpW - (2.0f * btnW + btnGap)) * 0.5f;
    btnL[1] = btnL[0] + btnW + btnGap;

    /* Publish geometry for Update's mouse hit-test. */
    s_vpW = vpW; s_vpH = vpH;
    for (b = 0; b < 2; b++) { s_geoBtnL[b] = btnL[b]; s_geoBtnR[b] = btnL[b] + btnW; }
    s_geoBtnT = btnT; s_geoBtnB = btnB;
    s_geoValid = 1;

    cd_build_white();

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

    /* Dim the screen behind the dialog. */
    cd_quad(s_texWhite, NX(0.0f), NY(vpH), NX(vpW), NY(0.0f), 0.0f, 0.0f, 0.0f, 0.62f * dim);
    /* Panel body. */
    cd_quad(s_texWhite, NX(panelL), NY(panelT), NX(panelR), NY(panelB), 0.055f, 0.05f, 0.065f, 0.95f * dim);
    /* Title band + rule. */
    cd_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - 2.0f), NX(panelR - 2.0f), NY(panelT - titleH),
            0.10f, 0.05f, 0.05f, 0.95f * dim);
    cd_quad(s_texWhite, NX(panelL + 2.0f), NY(panelT - titleH), NX(panelR - 2.0f), NY(panelT - titleH - 2.0f),
            0.47f, 0.11f, 0.08f, dim);
    if (s_texTitle)
    {
        float tx = panelL + (panelW - (float)s_titleW) * 0.5f;
        float ty = panelT - (titleH - (float)s_titleH) * 0.5f;
        cd_quad(s_texTitle, NX(tx), NY(ty), NX(tx + s_titleW), NY(ty - s_titleH), 1.0f, 0.93f, 0.86f, dim);
    }

    /* Message, centred between the rule and the buttons. */
    if (s_texMsg)
    {
        float top = panelT - titleH;
        float mid = (top + btnT) * 0.5f;
        float tx  = panelL + (panelW - (float)s_msgW) * 0.5f;
        float ty  = mid + (float)s_msgH * 0.5f;
        cd_quad(s_texMsg, NX(tx), NY(ty), NX(tx + s_msgW), NY(ty - s_msgH), 0.92f, 0.92f, 0.95f, dim);
    }

    /* Buttons: the selected one gets the rust fill, the other a faint outline. */
    for (b = 0; b < 2; b++)
    {
        GLuint tex = b == 0 ? s_texYes : s_texNo;
        int    tw  = b == 0 ? s_yesW : s_noW;
        int    th  = b == 0 ? s_yesH : s_noH;
        float  l = btnL[b], r = btnL[b] + btnW;
        if (b == s_sel)
            cd_quad(s_texWhite, NX(l), NY(btnT), NX(r), NY(btnB), 0.42f, 0.16f, 0.12f, 0.85f * dim);
        else
        {
            cd_quad(s_texWhite, NX(l), NY(btnT), NX(r), NY(btnB), 1.0f, 1.0f, 1.0f, 0.06f * dim);
            cd_quad(s_texWhite, NX(l), NY(btnT), NX(r), NY(btnT - 1.0f), 1.0f, 1.0f, 1.0f, 0.18f * dim);
            cd_quad(s_texWhite, NX(l), NY(btnB + 1.0f), NX(r), NY(btnB), 1.0f, 1.0f, 1.0f, 0.18f * dim);
            cd_quad(s_texWhite, NX(l), NY(btnT), NX(l + 1.0f), NY(btnB), 1.0f, 1.0f, 1.0f, 0.18f * dim);
            cd_quad(s_texWhite, NX(r - 1.0f), NY(btnT), NX(r), NY(btnB), 1.0f, 1.0f, 1.0f, 0.18f * dim);
        }
        if (tex)
        {
            float tx = l + (btnW - (float)tw) * 0.5f;
            float ty = btnB + (btnH + (float)th) * 0.5f;
            float c  = b == s_sel ? 1.0f : 0.80f;
            cd_quad(tex, NX(tx), NY(ty), NX(tx + tw), NY(ty - th), c, c * 0.96f, c * 0.92f, dim);
        }
    }

    if (s_texHint)
    {
        float hx = panelL + (panelW - (float)s_hintW) * 0.5f;
        float hy = panelB + hintH * 0.78f;
        cd_quad(s_texHint, NX(hx), NY(hy), NX(hx + s_hintW), NY(hy - s_hintH), 0.7f, 0.7f, 0.75f, dim);
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
