/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_start_screen.c - the launcher's simple view, inside the game.
 *
 * A UWP package cannot ship the WinForms launcher, and on a console there is
 * no desktop to run one from, so the build opens on the same page the launcher
 * shows in simple mode (Form1.Simple.cs): the quality presets, a few boxes,
 * the minimap, the disc, the game language and Play, over fog and snow. It
 * writes the same config.cfg keys the launcher does.
 *
 * It runs once, between PsyX_Initialise and the rest of boot, in a loop of its
 * own: the disc and the language have to be chosen before the disc is mounted,
 * and everything else it sets is read by the boot code that follows it. The
 * Essential QoL Mods window (pc_qol_mods.c) stands in for the Mod Manager
 * button; it is driven from here the way the game's main loop drives it.
 *
 * GL as in pc_rando_settings.c: own program and textures, legacy GLSL, and the
 * state it found is put back before boot carries on.
 */
#if defined(SH_UWP)

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"

#include "pc_config.h"
#include "pc_qol_mods.h"
#include "pc_start_screen.h"
#include "sh_log.h"

extern SDL_Window* g_window;
extern int         g_presentWidth, g_presentHeight;
extern int         g_PcNativeRender;
extern void        PsyX_PumpEvents(void);
extern void        PsyX_ApplyWindowState(int width, int height, int fullscreen);
extern int         Sh_LogSetEnabled(int on);
extern int         Uwp_FindDiscDir(char* out, int outSize);

/* The page is laid out on a canvas of this size (the launcher's client area,
 * less its Mode row), scaled to fit the screen and centred. */
#define SS_CANVAS_W 760.0f
#define SS_CANVAS_H 690.0f

enum { SS_FONT_TITLE = 0, SS_FONT_BODY, SS_FONT_COUNT };

/* Focus stops. The presets are the left column, FOCUS_DITHER..FOCUS_LANG the
 * right one, then the buttons. */
enum
{
    FOCUS_PRESET0 = 0,
    FOCUS_DITHER  = PC_SIMPLE_COUNT,
    FOCUS_LOGGING,
    FOCUS_CRT,
    FOCUS_MINIMAP,
    FOCUS_DISC,
    FOCUS_LANG,
    FOCUS_PLAY,
    FOCUS_MODS,
    FOCUS_COUNT
};

enum
{
    IN_UP = 1, IN_DOWN = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_OK = 16, IN_BACK = 32, IN_START = 64, IN_CLICK = 128
};

static const char* const kPresetNames[PC_SIMPLE_COUNT + 1] = {
    "Original", "Enhanced: Low Spec", "Enhanced: Normal", "Enhanced: High Quality", "Enhanced: Ultra Quality",
    "Custom",
};
static const char* const kPresetAbout[PC_SIMPLE_COUNT + 1] = {
    "The game as it was on the PlayStation: its own resolution, 30 fps and the original lighting.",
    "Smooth textures and no extra effects; a 1080p screen renders at 0.7 scale. For older graphics and Xbox One.",
    "No FPS limit, sharper textures, per-pixel lighting and flashlight shadows.",
    "Per-pixel lighting with Hero Light, and the modern flashlight with detailed shadows.",
    "The highest shadow detail and the whole town in view through thinner fog. For powerful graphics cards.",
    "Settings changed in the game's own options.",
};
static const char* const kMinimapNames[9] = {
    "Off",
    "Circle + Top Left",    "Circle + Top Right",    "Circle + Bottom Left", "Circle + Bottom Right",
    "Square + Top Left",    "Square + Top Right",    "Square + Bottom Left", "Square + Bottom Right",
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define SS_MAX_CHOICES 48

typedef struct
{
    char value[128]; /* what config.cfg gets */
    char label[128]; /* what the row shows */
} s_SsChoice;

static s_SsChoice s_discs[SS_MAX_CHOICES];
static int        s_discCount, s_discSel, s_discFound;
static s_SsChoice s_langs[SS_MAX_CHOICES];
static int        s_langCount, s_langSel;
static Uint32     s_discScanMs;

static int   s_focus = FOCUS_PLAY;
static int   s_lastColumn = FOCUS_PRESET0 + PC_SIMPLE_NORMAL;
static float s_rect[FOCUS_COUNT][4]; /* canvas x0, y0, x1, y1 of each stop */

static float s_vpW, s_vpH, s_scale, s_orgX, s_orgY;

static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locColor;
static GLuint s_texWhite, s_texDisc, s_texFog[2];

static stbtt_fontinfo s_font[SS_FONT_COUNT];
static int            s_fontOk[SS_FONT_COUNT];

#define SS_TEXT_CACHE 160
typedef struct
{
    char     text[200];
    int      font, px;
    GLuint   tex;
    int      w, h;
    unsigned used;
} s_SsText;
static s_SsText s_text[SS_TEXT_CACHE];
static unsigned s_textClock;

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint ss_shader(GLenum type, const char* src)
{
    GLuint      sh = glCreateShader(type);
    GLint       ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };

    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[START] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int ss_gl_init(void)
{
    static const char* vsSrc =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fsSrc =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";
    GLuint vs = ss_shader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = ss_shader(GL_FRAGMENT_SHADER, fsSrc);
    GLint  ok = 0;

    if (!vs || !fs)
        return 0;
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
        glDeleteProgram(s_prog);
        s_prog = 0;
        return 0;
    }
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    return 1;
}

static GLuint ss_upload(const unsigned char* rgba, int w, int h, int repeatS)
{
    GLuint t = 0;

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeatS ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

/* A quad in viewport pixels, y down from the top. */
static void ss_quad_px(GLuint tex, float x0, float y0, float x1, float y1, float u0, float u1,
                       float r, float g, float b, float a)
{
    float v[6][4];
    float nx0 = -1.0f + 2.0f * x0 / s_vpW, nx1 = -1.0f + 2.0f * x1 / s_vpW;
    float ny0 = 1.0f - 2.0f * y0 / s_vpH, ny1 = 1.0f - 2.0f * y1 / s_vpH;

    if (!tex)
        return;
    v[0][0] = nx0; v[0][1] = ny0; v[0][2] = u0; v[0][3] = 0;
    v[1][0] = nx0; v[1][1] = ny1; v[1][2] = u0; v[1][3] = 1;
    v[2][0] = nx1; v[2][1] = ny0; v[2][2] = u1; v[2][3] = 0;
    v[3][0] = nx1; v[3][1] = ny0; v[3][2] = u1; v[3][3] = 0;
    v[4][0] = nx0; v[4][1] = ny1; v[4][2] = u0; v[4][3] = 1;
    v[5][0] = nx1; v[5][1] = ny1; v[5][2] = u1; v[5][3] = 1;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static float ss_px(float canvas) { return floorf(canvas * s_scale + 0.5f); }
static float ss_x(float cx) { return floorf(s_orgX + cx * s_scale + 0.5f); }
static float ss_y(float cy) { return floorf(s_orgY + cy * s_scale + 0.5f); }

/* A filled rectangle in canvas units; colour as 0..255. */
static void ss_fill(float x0, float y0, float x1, float y1, int r, int g, int b, int a)
{
    ss_quad_px(s_texWhite, ss_x(x0), ss_y(y0), ss_x(x1), ss_y(y1), 0, 1,
               r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static void ss_frame(float x0, float y0, float x1, float y1, float t, int r, int g, int b, int a)
{
    ss_fill(x0, y0, x1, y0 + t, r, g, b, a);
    ss_fill(x0, y1 - t, x1, y1, r, g, b, a);
    ss_fill(x0, y0 + t, x0 + t, y1 - t, r, g, b, a);
    ss_fill(x1 - t, y0 + t, x1, y1 - t, r, g, b, a);
}

static void ss_disc(float cx, float cy, float radius, int r, int g, int b, int a)
{
    ss_quad_px(s_texDisc, ss_x(cx - radius), ss_y(cy - radius), ss_x(cx + radius), ss_y(cy + radius), 0, 1,
               r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static unsigned char* ss_read_file(const char* path)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (buf = (unsigned char*)malloc((size_t)n)) == NULL)
    {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static void ss_fonts_init(void)
{
    static const char* const paths[SS_FONT_COUNT] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
    };
    int i;

    for (i = 0; i < SS_FONT_COUNT; i++)
    {
        /* stb_truetype keeps pointers into the file, so it is never freed. */
        unsigned char* data = ss_read_file(paths[i]);
        s_fontOk[i] = data && stbtt_InitFont(&s_font[i], data, stbtt_GetFontOffsetForIndex(data, 0));
    }
    for (i = 0; i < SS_FONT_COUNT; i++)
    {
        int other = (i + 1) % SS_FONT_COUNT;
        if (!s_fontOk[i] && s_fontOk[other])
        {
            s_font[i]   = s_font[other];
            s_fontOk[i] = 1;
        }
    }
}

static int ss_utf8(const char** p)
{
    const unsigned char* s = (const unsigned char*)*p;
    int cp = s[0], extra = 0, i;

    if (cp >= 0xF0)      { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    for (i = 1; i <= extra; i++)
    {
        if ((s[i] & 0xC0) != 0x80)
        {
            *p += i;
            return '?';
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += extra + 1;
    return cp;
}

/* 1 when the font has a glyph for every character. */
static int ss_font_covers(int font, const char* text)
{
    while (*text)
    {
        int cp = ss_utf8(&text);
        if (cp != ' ' && stbtt_FindGlyphIndex(&s_font[font], cp) == 0)
            return 0;
    }
    return 1;
}

static float ss_measure_px(int font, float px, const char* text, int len)
{
    const char* end = text + len;
    float       scale, w = 0.0f;
    int         prev = 0;

    if (!s_fontOk[font])
        return 0.0f;
    scale = stbtt_ScaleForPixelHeight(&s_font[font], px);
    while (text < end && *text)
    {
        int cp = ss_utf8(&text), adv, lsb;
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&s_font[font], prev, cp) * scale;
        stbtt_GetCodepointHMetrics(&s_font[font], cp, &adv, &lsb);
        w += adv * scale;
        prev = cp;
    }
    return w;
}

static GLuint ss_bake(int font, float px, const char* text, int* outW, int* outH)
{
    const char*    p = text;
    float          scale, penX, baseY;
    int            asc, desc, gap, W, H, i, pad = 2, prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint         tex;

    if (!s_fontOk[font] || !text[0] || px < 4.0f)
        return 0;
    scale = stbtt_ScaleForPixelHeight(&s_font[font], px);
    stbtt_GetFontVMetrics(&s_font[font], &asc, &desc, &gap);
    W = (int)ceilf(ss_measure_px(font, px, text, (int)strlen(text))) + 2 * pad + 2;
    H = (int)ceilf(px) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)malloc((size_t)W * H * 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    while (*p)
    {
        int   cp = ss_utf8(&p);
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;

        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font[font], prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font[font], cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font[font], scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H)
                    continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx;
                    if (dx >= 0 && dx < W && scratch[sy * gw + sx] > cov[dy * W + dx])
                        cov[dy * W + dx] = scratch[sy * gw + sx];
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font[font], cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
    }

    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    tex = ss_upload(rgba, W, H, 0);
    *outW = W;
    *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

static const s_SsText* ss_text_get(int font, int px, const char* text)
{
    int i, slot = 0;

    s_textClock++;
    for (i = 0; i < SS_TEXT_CACHE; i++)
    {
        if (s_text[i].tex && s_text[i].font == font && s_text[i].px == px && strcmp(s_text[i].text, text) == 0)
        {
            s_text[i].used = s_textClock;
            return &s_text[i];
        }
        if (s_text[i].used < s_text[slot].used)
            slot = i;
    }
    if (strlen(text) >= sizeof(s_text[slot].text))
        return NULL;
    if (s_text[slot].tex)
        glDeleteTextures(1, &s_text[slot].tex);
    s_text[slot].tex = ss_bake(font, (float)px, text, &s_text[slot].w, &s_text[slot].h);
    if (!s_text[slot].tex)
    {
        s_text[slot].used = 0;
        return NULL;
    }
    snprintf(s_text[slot].text, sizeof(s_text[slot].text), "%s", text);
    s_text[slot].font = font;
    s_text[slot].px   = px;
    s_text[slot].used = s_textClock;
    return &s_text[slot];
}

static float ss_text_width(int font, float size, const char* text)
{
    return ss_measure_px(font, ss_px(size), text, (int)strlen(text)) / s_scale;
}

/* One line, its top-left at (cx, cy) in canvas units, outlined in black as the
 * launcher's text is so it reads over the fog. */
static void ss_text_draw(int font, float size, float cx, float cy, int r, int g, int b, const char* text)
{
    const s_SsText* t = ss_text_get(font, (int)ss_px(size), text);
    float           x, y, o;
    int             i;
    static const float dir[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };

    if (!t)
        return;
    x = ss_x(cx) - 2.0f;
    y = ss_y(cy) - 2.0f;
    o = floorf(s_scale + 0.5f);
    if (o < 1.0f)
        o = 1.0f;
    for (i = 0; i < 8; i++)
        ss_quad_px(t->tex, x + dir[i][0] * o, y + dir[i][1] * o, x + dir[i][0] * o + t->w, y + dir[i][1] * o + t->h,
                   0, 1, 0.0f, 0.0f, 0.0f, 0.55f);
    ss_quad_px(t->tex, x, y, x + t->w, y + t->h, 0, 1, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

static void ss_text_centred(int font, float size, float x0, float x1, float cy, int r, int g, int b, const char* text)
{
    ss_text_draw(font, size, (x0 + x1 - ss_text_width(font, size, text)) * 0.5f, cy, r, g, b, text);
}

/* Greedy word wrap into at most maxLines lines; the last one takes what is left. */
static void ss_text_wrapped(int font, float size, float cx, float cy, float width, float pitch, int maxLines,
                            int r, int g, int b, const char* text)
{
    char        line[200];
    const char* p = text;
    float       px = ss_px(size), maxPx = width * s_scale;
    int         n;

    for (n = 0; n < maxLines && *p; n++)
    {
        const char* brk = NULL;
        const char* q   = p;

        if (n < maxLines - 1)
        {
            while (*q)
            {
                const char* wordEnd = q;
                while (*wordEnd && *wordEnd != ' ')
                    wordEnd++;
                if (ss_measure_px(font, px, p, (int)(wordEnd - p)) > maxPx && brk)
                    break;
                brk = wordEnd;
                q   = *wordEnd ? wordEnd + 1 : wordEnd;
            }
        }
        if (!brk || !*q)
            brk = p + strlen(p);
        snprintf(line, sizeof(line), "%.*s", (int)(brk - p), p);
        ss_text_draw(font, size, cx, cy + n * pitch, r, g, b, line);
        p = brk;
        while (*p == ' ')
            p++;
    }
}

/* ------------------------------------------------------------------ */
/* Fog and snow (the launcher's SimpleScene)                           */
/* ------------------------------------------------------------------ */

#define SS_FOG_W 512
#define SS_FOG_H 256

static float ss_hash(int x, int y, int seed)
{
    unsigned h = (unsigned)(x * 374761393 + y * 668265263 + seed * 144665);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}

/* Value noise that repeats every `period` cells across, so a bank can slide
 * sideways for ever. */
static float ss_noise(float x, float y, int period, int seed)
{
    int   xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    int   x0 = ((xi % period) + period) % period, x1 = (x0 + 1) % period;
    float a, b, c, d;

    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    a = ss_hash(x0, yi, seed);     b = ss_hash(x1, yi, seed);
    c = ss_hash(x0, yi + 1, seed); d = ss_hash(x1, yi + 1, seed);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static GLuint ss_make_fog(int seed, float strength)
{
    unsigned char* px = (unsigned char*)malloc(SS_FOG_W * SS_FOG_H * 4);
    GLuint         tex;
    int            x, y, o;

    if (!px)
        return 0;
    for (y = 0; y < SS_FOG_H; y++)
    {
        for (x = 0; x < SS_FOG_W; x++)
        {
            float v = 0.0f, amp = 0.5f, a;
            int   per = 4;
            /* Stretched sideways: long soft layers, not smoke. */
            for (o = 0; o < 4; o++)
            {
                v += amp * ss_noise(x * per / (float)SS_FOG_W, y * per * 1.6f / SS_FOG_H + o * 7.3f, per, seed + o);
                amp *= 0.5f;
                per *= 2;
            }
            a = (v - 0.36f) / 0.36f;
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            a = a * a * (3 - 2 * a) * strength;
            px[(y * SS_FOG_W + x) * 4 + 0] = 150;
            px[(y * SS_FOG_W + x) * 4 + 1] = 156;
            px[(y * SS_FOG_W + x) * 4 + 2] = 158;
            px[(y * SS_FOG_W + x) * 4 + 3] = (unsigned char)(255.0f * a);
        }
    }
    tex = ss_upload(px, SS_FOG_W, SS_FOG_H, 1);
    free(px);
    return tex;
}

static void ss_make_disc(void)
{
    enum { N = 64 };
    static unsigned char px[N * N * 4];
    int x, y;

    for (y = 0; y < N; y++)
    {
        for (x = 0; x < N; x++)
        {
            float dx = x + 0.5f - N * 0.5f, dy = y + 0.5f - N * 0.5f;
            float a = (N * 0.5f - 1.0f) - sqrtf(dx * dx + dy * dy);
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            px[(y * N + x) * 4 + 0] = 255; px[(y * N + x) * 4 + 1] = 255; px[(y * N + x) * 4 + 2] = 255;
            px[(y * N + x) * 4 + 3] = (unsigned char)(255.0f * a);
        }
    }
    s_texDisc = ss_upload(px, N, N, 0);
}

static void ss_draw_scene(float t)
{
    /* One fog period spans the canvas's width, as it spans the launcher's. */
    float span = (s_vpW / s_vpH) / (760.0f / 732.0f);
    float ua = t * 9.0f / 760.0f, ub = -t * 5.0f / 760.0f;
    float unit = s_vpH / 732.0f;
    int   i, count = (int)(110.0f * span);

    ss_quad_px(s_texFog[0], 0, 0, s_vpW, s_vpH, ua, ua + span, 1, 1, 1, 1);
    ss_quad_px(s_texFog[1], 0, 0, s_vpW, s_vpH, ub, ub + span, 1, 1, 1, 1);

    for (i = 0; i < count; i++)
    {
        int   layer = i % 3;
        float speed = 9.0f + layer * 7.0f + ss_hash(i, 1, 1999) * 4.0f;
        float sway  = 4.0f + ss_hash(i, 2, 1999) * 6.0f;
        float phase = ss_hash(i, 3, 1999) * 6.28f;
        float y = fmodf(ss_hash(i, 4, 1999) * 732.0f + t * speed, 732.0f) * unit;
        float x = fmodf(ss_hash(i, 5, 1999) * s_vpW + (sinf(t * 0.6f + phase) * sway + t * speed * 0.2f) * unit, s_vpW);
        float size = floorf((2 + (layer == 2 ? 1 : 0)) * unit + 0.5f);

        x = floorf(x);
        y = floorf(y);
        ss_quad_px(s_texWhite, x, y, x + size, y + size, 0, 1,
                   235 / 255.0f, 230 / 255.0f, 244 / 255.0f, (90 + layer * 40) / 255.0f);
    }
}

/* ------------------------------------------------------------------ */
/* The lists                                                           */
/* ------------------------------------------------------------------ */

static int ss_ends_with_bin(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && SDL_strcasecmp(name + n - 4, ".bin") == 0;
}

static void ss_scan_discs(char* dataDir, int dataDirSize)
{
    DIR*           d;
    struct dirent* e;

    s_discCount = 0;
    s_discSel   = 0;
    snprintf(s_discs[0].value, sizeof(s_discs[0].value), "%s", "");
    snprintf(s_discs[0].label, sizeof(s_discs[0].label), "%s", "Automatic");
    s_discCount = 1;

    /* A drive plugged in since the last look (uwp_main.cpp). */
    Uwp_FindDiscDir(dataDir, dataDirSize);

    d = opendir(dataDir);
    if (d)
    {
        while ((e = readdir(d)) != NULL && s_discCount < SS_MAX_CHOICES)
        {
            if (!ss_ends_with_bin(e->d_name) || strlen(e->d_name) >= sizeof(s_discs[0].value))
                continue;
            snprintf(s_discs[s_discCount].value, sizeof(s_discs[0].value), "%s", e->d_name);
            snprintf(s_discs[s_discCount].label, sizeof(s_discs[0].label), "%.*s",
                     (int)strlen(e->d_name) - 4, e->d_name);
            if (SDL_strcasecmp(e->d_name, g_PcConfig.discImage) == 0)
                s_discSel = s_discCount;
            s_discCount++;
        }
        closedir(d);
    }
    s_discFound  = s_discCount > 1;
    s_discScanMs = SDL_GetTicks();
}

static void ss_add_lang(const char* label, const char* code)
{
    int i;

    for (i = 0; i < s_langCount; i++)
        if (SDL_strcasecmp(s_langs[i].value, code) == 0)
            return;
    if (s_langCount >= SS_MAX_CHOICES || strlen(code) >= sizeof(g_PcConfig.languageCode))
        return;
    snprintf(s_langs[s_langCount].value, sizeof(s_langs[0].value), "%s", code);
    snprintf(s_langs[s_langCount].label, sizeof(s_langs[0].label), "%s",
             ss_font_covers(SS_FONT_BODY, label) ? label : code);
    s_langCount++;
}

/* Automatic, the languages every disc family carries, then the packs in
 * gamedata/lang: the launcher's Game Language list. The game falls back to
 * Automatic for anything the mounted disc cannot show. */
static void ss_scan_langs(void)
{
    DIR*           d;
    struct dirent* e;
    int            i;

    s_langCount = 0;
    ss_add_lang("Automatic", "auto");
    ss_add_lang("US English", "en_us");
    ss_add_lang("PAL English", "en_pal");
    ss_add_lang("German", "de");
    ss_add_lang("French", "fr");
    ss_add_lang("Spanish", "es");
    ss_add_lang("Italian", "it");

    d = opendir("gamedata/lang");
    if (d)
    {
        while ((e = readdir(d)) != NULL)
        {
            char   path[400], line[256], code[64] = "", name[128] = "";
            size_t n = strlen(e->d_name);
            FILE*  f;
            int    row;

            if (n < 6 || SDL_strcasecmp(e->d_name + n - 5, ".lang") != 0)
                continue;
            snprintf(path, sizeof(path), "gamedata/lang/%s", e->d_name);
            f = fopen(path, "rb");
            if (!f)
                continue;
            /* Only the headers, which the format puts first. */
            for (row = 0; row < 16 && fgets(line, sizeof(line), f); row++)
            {
                line[strcspn(line, "\r\n")] = '\0';
                if (strncmp(line, "!code=", 6) == 0)
                    snprintf(code, sizeof(code), "%s", line + 6);
                else if (strncmp(line, "!name=", 6) == 0)
                    snprintf(name, sizeof(name), "%s", line + 6);
            }
            fclose(f);
            if (code[0])
                ss_add_lang(name[0] ? name : code, code);
        }
        closedir(d);
    }

    s_langSel = -1;
    for (i = 0; i < s_langCount; i++)
        if (SDL_strcasecmp(s_langs[i].value, g_PcConfig.languageCode) == 0)
            s_langSel = i;
    if (s_langSel < 0)
    {
        /* A code the list does not have (a pack since removed, or typed by
         * hand) stays the player's until they pick another. */
        if (g_PcConfig.languageCode[0] && s_langCount < SS_MAX_CHOICES)
        {
            snprintf(s_langs[s_langCount].value, sizeof(s_langs[0].value), "%s", g_PcConfig.languageCode);
            snprintf(s_langs[s_langCount].label, sizeof(s_langs[0].label), "%s", g_PcConfig.languageCode);
            s_langSel = s_langCount++;
        }
        else
            s_langSel = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static void ss_save_int(const char* key, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    PcConfig_SaveKeyValue(key, buf);
}

static int ss_minimap_index(void)
{
    int corner = (g_PcConfig.minimapCorner >= 0 && g_PcConfig.minimapCorner <= 3) ? g_PcConfig.minimapCorner : 0;
    return g_PcConfig.minimap == 0 ? 0 : (g_PcConfig.minimap == 2 ? 1 + corner : 5 + corner);
}

static void ss_pick_preset(int p)
{
    SDL_DisplayMode dm;

    PcConfig_SimplePresetApply(p);
    /* As the launcher's click does: the render size goes back to the
     * screen's. A window is its own size and has none to reset. */
    if (g_PcConfig.fullscreen != 0 && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0)
    {
        g_PcConfig.windowWidth  = dm.w;
        g_PcConfig.windowHeight = dm.h;
        ss_save_int("width", dm.w);
        ss_save_int("height", dm.h);
    }
}

/* dir: +1 / -1 for a list, 0 for a press. */
static int ss_activate(int stop, int dir)
{
    if (stop < PC_SIMPLE_COUNT)
    {
        if (dir == 0)
            ss_pick_preset(stop);
        return 0;
    }
    switch (stop)
    {
        case FOCUS_DITHER:
            if (dir != 0)
                break;
            g_PcConfig.dithering = !g_PcConfig.dithering;
            ss_save_int("dithering", g_PcConfig.dithering);
            if (g_PcConfig.dithering)
            {
                g_PcConfig.ditherResolution = 0; /* the PSX pixel grid */
                ss_save_int("dither_resolution", 0);
            }
            break;
        case FOCUS_LOGGING:
            if (dir != 0)
                break;
            g_PcConfig.enableDebugLog = !g_PcConfig.enableDebugLog;
            if (g_PcConfig.enableDebugLog && !Sh_LogSetEnabled(1))
                g_PcConfig.enableDebugLog = 0;
            else if (!g_PcConfig.enableDebugLog)
                Sh_LogSetEnabled(0);
            ss_save_int("enable_debug_log", g_PcConfig.enableDebugLog);
            break;
        case FOCUS_CRT:
            if (dir != 0)
                break;
            g_PcConfig.postProcess = (g_PcConfig.postProcess == 1) ? 0 : 1; /* 1 = CRT */
            ss_save_int("post_process", g_PcConfig.postProcess);
            break;
        case FOCUS_MINIMAP:
        {
            int mi = (ss_minimap_index() + (dir < 0 ? 8 : 1)) % 9;
            g_PcConfig.minimap       = mi == 0 ? 0 : (mi <= 4 ? 2 : 1);
            g_PcConfig.minimapCorner = mi == 0 ? 0 : (mi <= 4 ? mi - 1 : mi - 5);
            ss_save_int("minimap", g_PcConfig.minimap);
            ss_save_int("minimap_corner", g_PcConfig.minimapCorner);
            break;
        }
        case FOCUS_DISC:
            if (s_discCount > 1)
            {
                s_discSel = (s_discSel + (dir < 0 ? s_discCount - 1 : 1)) % s_discCount;
                snprintf(g_PcConfig.discImage, sizeof(g_PcConfig.discImage), "%s", s_discs[s_discSel].value);
                PcConfig_SaveKeyValue("disc_image", g_PcConfig.discImage);
            }
            break;
        case FOCUS_LANG:
            s_langSel = (s_langSel + (dir < 0 ? s_langCount - 1 : 1)) % s_langCount;
            snprintf(g_PcConfig.languageCode, sizeof(g_PcConfig.languageCode), "%s", s_langs[s_langSel].value);
            PcConfig_SaveKeyValue("language", g_PcConfig.languageCode);
            break;
        case FOCUS_PLAY:
            if (dir == 0 && s_discFound)
                return 1;
            break;
        case FOCUS_MODS:
            if (dir == 0)
                Pc_QolMods_OpenList();
            break;
        default:
            break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static int ss_read_held(void)
{
    const Uint8* k = SDL_GetKeyboardState(NULL);
    int          held = 0, i, n = SDL_NumJoysticks();

    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W])    held |= IN_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S])  held |= IN_DOWN;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A])  held |= IN_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) held |= IN_RIGHT;
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_C])
        held |= IN_OK;
    if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_V])
        held |= IN_BACK;

    for (i = 0; i < n; i++)
    {
        /* PsyCross opened these when their device-added events were pumped. */
        SDL_GameController* c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        int lx, ly;

        if (!c)
            continue;
        lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -16000)    held |= IN_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 16000)   held |= IN_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -16000)  held |= IN_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 16000)  held |= IN_RIGHT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A))     held |= IN_OK;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B))     held |= IN_BACK;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) held |= IN_START;
    }
    return held;
}

/* The pointer in canvas units; 0 when it is outside the window. */
static int ss_pointer(float* cx, float* cy, int* down)
{
    int mx, my, ww = 0, wh = 0;
    Uint32 buttons = SDL_GetMouseState(&mx, &my);

    SDL_GetWindowSize(g_window, &ww, &wh);
    *down = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (ww <= 0 || wh <= 0 || mx < 0 || my < 0 || mx >= ww || my >= wh)
        return 0;
    *cx = (mx * s_vpW / ww - s_orgX) / s_scale;
    *cy = (my * s_vpH / wh - s_orgY) / s_scale;
    return 1;
}

static int ss_stop_at(float cx, float cy)
{
    int i;
    for (i = 0; i < FOCUS_COUNT; i++)
        if (cx >= s_rect[i][0] && cx < s_rect[i][2] && cy >= s_rect[i][1] && cy < s_rect[i][3])
            return i;
    return -1;
}

static void ss_move(int pressed)
{
    int f = s_focus;

    if (pressed & IN_UP)
    {
        if (f == FOCUS_MODS)      f = FOCUS_PLAY;
        else if (f == FOCUS_PLAY) f = s_lastColumn < FOCUS_DITHER ? PC_SIMPLE_COUNT - 1 : FOCUS_LANG;
        else if (f != FOCUS_PRESET0 && f != FOCUS_DITHER) f--;
    }
    if (pressed & IN_DOWN)
    {
        if (f == PC_SIMPLE_COUNT - 1 || f == FOCUS_LANG) f = FOCUS_PLAY;
        else if (f == FOCUS_PLAY)                        f = FOCUS_MODS;
        else if (f != FOCUS_MODS)                        f++;
    }
    if ((pressed & IN_RIGHT) && f < PC_SIMPLE_COUNT)
        f = FOCUS_DITHER + (f > FOCUS_LANG - FOCUS_DITHER ? FOCUS_LANG - FOCUS_DITHER : f);
    else if ((pressed & IN_LEFT) && f >= FOCUS_DITHER && f <= FOCUS_CRT)
        f = f - FOCUS_DITHER;
    s_focus = f;
    if (f < FOCUS_PLAY)
        s_lastColumn = f;
}

/* ------------------------------------------------------------------ */
/* The page                                                            */
/* ------------------------------------------------------------------ */

static void ss_set_rect(int stop, float x0, float y0, float x1, float y1)
{
    s_rect[stop][0] = x0; s_rect[stop][1] = y0; s_rect[stop][2] = x1; s_rect[stop][3] = y1;
}

static void ss_mark(float x, float y, int round, int on)
{
    if (round)
    {
        ss_disc(x + 9, y + 9, 10.5f, 0, 0, 0, 235);
        ss_disc(x + 9, y + 9, 9.0f, 214, 212, 204, 255);
        ss_disc(x + 9, y + 9, 7.0f, 12, 13, 12, 255);
        if (on)
            ss_disc(x + 9, y + 9, 5.0f, 214, 70, 56, 255);
    }
    else
    {
        ss_fill(x - 1.5f, y - 1.5f, x + 19.5f, y + 19.5f, 0, 0, 0, 235);
        ss_fill(x, y, x + 18, y + 18, 214, 212, 204, 255);
        ss_fill(x + 2, y + 2, x + 16, y + 16, 12, 13, 12, 255);
        if (on)
            ss_fill(x + 4, y + 4, x + 14, y + 14, 214, 70, 56, 255);
    }
}

static void ss_focus_frame(int stop)
{
    if (s_focus == stop)
        ss_frame(s_rect[stop][0], s_rect[stop][1], s_rect[stop][2], s_rect[stop][3], 2, 232, 230, 222, 255);
}

static void ss_check_row(int stop, float y, const char* label, int on)
{
    ss_set_rect(stop, 472, y - 4, 720, y + 26);
    ss_mark(480, y + 2, 0, on);
    ss_text_draw(SS_FONT_BODY, 21, 508, y - 1, 232, 230, 222, label);
    ss_focus_frame(stop);
}

static void ss_list_row(int stop, float y, const char* label, const char* value, int r, int g, int b)
{
    char shown[160];
    int  focused = s_focus == stop;

    ss_text_draw(SS_FONT_BODY, 20, 480, y, 232, 230, 222, label);
    ss_set_rect(stop, 480, y + 25, 720, y + 55);
    ss_fill(480, y + 25, 720, y + 55, 34, 36, 34, 235);
    ss_frame(480, y + 25, 720, y + 55, 1, 84, 86, 82, 255);
    if (focused)
    {
        ss_text_draw(SS_FONT_BODY, 20, 486, y + 28, 214, 70, 56, "<");
        ss_text_draw(SS_FONT_BODY, 20, 706, y + 28, 214, 70, 56, ">");
    }
    /* Long disc names are cut to the box. */
    snprintf(shown, sizeof(shown), "%s", value);
    while (strlen(shown) > 4 && ss_text_width(SS_FONT_BODY, 19, shown) > 196.0f)
    {
        size_t n = strlen(shown);
        do { n--; } while (n > 0 && ((unsigned char)shown[n] & 0xC0) == 0x80);
        snprintf(shown + (n > 3 ? n - 3 : 0), 4, "...");
    }
    ss_text_centred(SS_FONT_BODY, 19, 480, 720, y + 29, r, g, b, shown);
    ss_focus_frame(stop);
}

static void ss_draw_page(void)
{
    int preset = PcConfig_SimplePresetDetect();
    int shownPreset = preset == PC_SIMPLE_CUSTOM ? PC_SIMPLE_COUNT : preset;
    int i;

    ss_text_draw(SS_FONT_TITLE, 46, 38, 14, 214, 70, 56, "SILENT HILL PC");
    ss_text_draw(SS_FONT_BODY, 20, 40, 76, 176, 176, 168, "Quality");
    ss_text_draw(SS_FONT_BODY, 20, 480, 76, 176, 176, 168, "Options");

    for (i = 0; i <= PC_SIMPLE_COUNT; i++)
    {
        float top = 104.0f + i * 66.0f;
        int   on = i == shownPreset;
        /* Custom cannot be picked: it only says the settings are the player's own. */
        int   grey = i == PC_SIMPLE_COUNT && !on;

        if (i < PC_SIMPLE_COUNT)
            ss_set_rect(i, 32, top, 452, top + 62);
        if (on)
            ss_fill(32, top, 452, top + 62, 120, 40, 32, 70);
        ss_mark(40, top + 6, 1, on);
        if (on)
            ss_text_draw(SS_FONT_BODY, 23, 68, top + 1, 255, 255, 255, kPresetNames[i]);
        else if (grey)
            ss_text_draw(SS_FONT_BODY, 23, 68, top + 1, 112, 112, 106, kPresetNames[i]);
        else
            ss_text_draw(SS_FONT_BODY, 23, 68, top + 1, 232, 230, 222, kPresetNames[i]);
        if (grey)
            ss_text_wrapped(SS_FONT_BODY, 16.5f, 68, top + 25, 376, 17, 2, 96, 96, 92, kPresetAbout[i]);
        else
            ss_text_wrapped(SS_FONT_BODY, 16.5f, 68, top + 25, 376, 17, 2, 176, 176, 168, kPresetAbout[i]);
        if (i < PC_SIMPLE_COUNT)
            ss_focus_frame(i);
    }

    ss_check_row(FOCUS_DITHER, 108, "Dithering", g_PcConfig.dithering != 0);
    ss_check_row(FOCUS_LOGGING, 142, "Logging", g_PcConfig.enableDebugLog != 0);
    ss_check_row(FOCUS_CRT, 176, "CRT Filter", g_PcConfig.postProcess == 1);

    ss_list_row(FOCUS_MINIMAP, 222, "Minimap:", kMinimapNames[ss_minimap_index()], 232, 230, 222);
    if (s_discFound)
        ss_list_row(FOCUS_DISC, 290, "Disc:", s_discs[s_discSel].label, 232, 230, 222);
    else
        ss_list_row(FOCUS_DISC, 290, "Disc:", "No disc image found", 226, 120, 104);
    ss_list_row(FOCUS_LANG, 358, "Game Language:", s_langs[s_langSel].label, 232, 230, 222);

    if (!s_discFound)
        ss_text_wrapped(SS_FONT_BODY, 16.5f, 480, 426, 240, 17, 5, 226, 120, 104,
                        "Put your Silent Hill disc image (.bin) in the app's LocalState\\gamedata folder, "
                        "or in SilentHill\\gamedata on a USB drive.");

    ss_set_rect(FOCUS_PLAY, 40, 522, 720, 578);
    if (s_discFound)
    {
        ss_fill(40, 522, 720, 578, 104, 26, 22, 255);
        ss_frame(40, 522, 720, 578, 1, 170, 70, 58, 255);
        ss_text_centred(SS_FONT_BODY, 34, 40, 720, 532, 255, 255, 255, "Play!");
    }
    else
    {
        ss_fill(40, 522, 720, 578, 44, 30, 28, 255);
        ss_frame(40, 522, 720, 578, 1, 84, 60, 56, 255);
        ss_text_centred(SS_FONT_BODY, 34, 40, 720, 532, 128, 120, 116, "Play!");
    }
    ss_focus_frame(FOCUS_PLAY);

    ss_set_rect(FOCUS_MODS, 40, 594, 260, 628);
    ss_fill(40, 594, 260, 628, 34, 36, 34, 255);
    ss_frame(40, 594, 260, 628, 1, 84, 86, 82, 255);
    ss_text_centred(SS_FONT_BODY, 20, 40, 260, 599, 232, 230, 222, "Essential QoL Mods");
    ss_focus_frame(FOCUS_MODS);

    ss_text_draw(SS_FONT_BODY, 16.5f, 40, 650, 176, 176, 168,
                 "D-pad: move      A: select      Left / Right: change a list      Menu: play");
}

/* ------------------------------------------------------------------ */
/* Run                                                                 */
/* ------------------------------------------------------------------ */

void Pc_StartScreen_Run(char* dataDir, int dataDirSize)
{
    GLint  prevFbo = 0, prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevVp[4] = { 0, 0, 0, 0 };
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLboolean prevBlend, prevDepth, prevCull, prevScissor;
    const int nativeAtBoot = g_PcConfig.nativeResolution;
    const int widthAtBoot = g_PcConfig.windowWidth, heightAtBoot = g_PcConfig.windowHeight;
    const Uint32 startMs = SDL_GetTicks();
    Uint32 repeatMs = 0;
    int    prevHeld, repeatBits = 0, prevDown = 0, play = 0, i;
    float  prevPx = -1.0f, prevPy = -1.0f;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    if (ss_gl_init())
    {
        static const unsigned char one[4] = { 255, 255, 255, 255 };

        s_texWhite  = ss_upload(one, 1, 1, 0);
        s_texFog[0] = ss_make_fog(11, 0.34f);
        s_texFog[1] = ss_make_fog(47, 0.24f);
        ss_make_disc();
        ss_fonts_init();
        ss_scan_langs();
        ss_scan_discs(dataDir, dataDirSize);

        /* Whatever is held as the app opens is not a press on this page. */
        PsyX_PumpEvents();
        prevHeld = ss_read_held() | IN_OK | IN_START;

        while (!play)
        {
            const Uint32 frameMs = SDL_GetTicks();
            int   held, pressed, down = 0, modsOpen;
            float px = 0.0f, py = 0.0f;

            PsyX_PumpEvents();

            s_vpW = (float)g_presentWidth;
            s_vpH = (float)g_presentHeight;
            if (s_vpW <= 0.0f || s_vpH <= 0.0f)
            {
                int ww = 0, wh = 0;
                SDL_GetWindowSize(g_window, &ww, &wh);
                s_vpW = (float)(ww > 0 ? ww : 1280);
                s_vpH = (float)(wh > 0 ? wh : 720);
            }
            s_scale = s_vpH / SS_CANVAS_H;
            if (s_scale > s_vpW / SS_CANVAS_W)
                s_scale = s_vpW / SS_CANVAS_W;
            s_orgX = floorf((s_vpW - SS_CANVAS_W * s_scale) * 0.5f);
            s_orgY = floorf((s_vpH - SS_CANVAS_H * s_scale) * 0.5f);

            /* Press edges, with a held direction repeating. */
            held    = ss_read_held();
            pressed = held & ~prevHeld;
            if ((held & 15) && (held & 15) == repeatBits)
            {
                if (frameMs >= repeatMs)
                {
                    pressed |= held & 15;
                    repeatMs = frameMs + 110;
                }
            }
            else
            {
                repeatBits = held & 15;
                repeatMs   = frameMs + 380;
            }
            prevHeld = held;

            modsOpen = Pc_QolMods_IsOpen();
            Pc_QolMods_Frame(0, (pressed & IN_UP) != 0, (pressed & IN_DOWN) != 0, (pressed & IN_LEFT) != 0,
                             (pressed & IN_RIGHT) != 0, (pressed & IN_OK) != 0, (pressed & IN_BACK) != 0);

            if (!modsOpen)
            {
                if (!s_discFound && frameMs - s_discScanMs > 2000)
                    ss_scan_discs(dataDir, dataDirSize);

                if (ss_pointer(&px, &py, &down))
                {
                    int over = ss_stop_at(px, py);
                    if (over >= 0 && (px != prevPx || py != prevPy))
                        s_focus = over;
                    if (over >= 0 && down && !prevDown)
                    {
                        /* The left third of a list steps it back. */
                        int back = over >= FOCUS_MINIMAP && over <= FOCUS_LANG &&
                                   px < s_rect[over][0] + (s_rect[over][2] - s_rect[over][0]) / 3.0f;
                        s_focus = over;
                        play    = ss_activate(over, over >= FOCUS_MINIMAP && over <= FOCUS_LANG ? (back ? -1 : 1) : 0);
                    }
                    prevPx = px;
                    prevPy = py;
                }
                prevDown = down;

                if (s_focus >= FOCUS_MINIMAP && s_focus <= FOCUS_LANG && (pressed & (IN_LEFT | IN_RIGHT)))
                    ss_activate(s_focus, (pressed & IN_LEFT) ? -1 : 1);
                else
                    ss_move(pressed);
                if (pressed & IN_OK)
                    play |= ss_activate(s_focus, s_focus >= FOCUS_MINIMAP && s_focus <= FOCUS_LANG ? 1 : 0);
                if (pressed & IN_START)
                    play |= ss_activate(FOCUS_PLAY, 0);
            }
            else
                prevDown = 1;

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, (GLsizei)s_vpW, (GLsizei)s_vpH);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glClearColor(10 / 255.0f, 11 / 255.0f, 10 / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(s_prog);
            glBindVertexArray(s_vao);
            glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            ss_draw_scene((float)(frameMs - startMs) / 1000.0f + 12.0f);
            ss_draw_page();
            Pc_QolMods_Draw();

            if (PsyX_Angle_Active())
                PsyX_Angle_Swap();
            else
                SDL_GL_SwapWindow(g_window);

            /* The swap interval is the game's to set later; this page only
             * needs not to spin. */
            {
                Uint32 spent = SDL_GetTicks() - frameMs;
                if (spent < 15)
                    SDL_Delay(15 - spent);
            }
        }
    }

    for (i = 0; i < SS_TEXT_CACHE; i++)
        if (s_text[i].tex)
            glDeleteTextures(1, &s_text[i].tex);
    memset(s_text, 0, sizeof(s_text));
    glDeleteTextures(1, &s_texWhite);
    glDeleteTextures(1, &s_texDisc);
    glDeleteTextures(2, s_texFog);
    if (s_vbo)  glDeleteBuffers(1, &s_vbo);
    if (s_vao)  glDeleteVertexArrays(1, &s_vao);
    if (s_prog) glDeleteProgram(s_prog);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (prevBlend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (prevDepth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (prevCull)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    /* The window was made for the render size the config had at boot. */
    if (g_PcConfig.nativeResolution != nativeAtBoot || g_PcConfig.windowWidth != widthAtBoot ||
        g_PcConfig.windowHeight != heightAtBoot)
    {
        g_PcNativeRender = g_PcConfig.nativeResolution;
        PsyX_ApplyWindowState(g_PcConfig.windowWidth, g_PcConfig.windowHeight, g_PcConfig.fullscreen);
    }
}

#endif /* SH_UWP */
