/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_ttf_text.c -- TrueType lines in native-menu coordinates (pc_ttf_text.h).
 *
 * Same self-contained GL arrangement as pc_ra_toast.c: own program, VAO and
 * textures, every touched state saved and restored. Each distinct line is
 * baked once at the current device-pixel size and cached.
 */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>
#include <PsyX/PsyX_public.h>
extern GLuint GR_ScreenFBO(void);

#include "stb_truetype.h"

#include "lang_ar.h"
#include "lang_pack.h"
#include "lang_quick.h"
#include "lang_ru.h"
#include "pc_ttf_text.h"
#include "sh_log.h"

/* Native-menu space: Nm_TextDraw's x 0..320 and y 0..224 (centre 160,112). */
#define TT_NATIVE_W 320.0f
#define TT_NATIVE_H 224.0f

/* Glyph heights the game font occupies there: a tall line is a 16-row cell
 * drawn at full height, a small one the interlaced half-height cell. */
#define TT_TALL_PX  13.0f
#define TT_SMALL_PX 7.0f
#define TT_MIN_FIT  0.70f

#define TT_QUEUE_MAX 96
#define TT_CACHE_MAX 64
#define TT_TEXT_MAX  512

/* A frame that queued nothing for this long has left the screen. */
#define TT_STALE_MS 150

typedef struct
{
    int  x, y, tall, colorId, maxW;
    char text[TT_TEXT_MAX];
} s_TtLine;

typedef struct
{
    char   text[TT_TEXT_MAX];
    int    pxH;
    GLuint tex;
    int    w, h;
    float  inkW;
    Uint32 used;
} s_TtBake;

static s_TtLine s_build[TT_QUEUE_MAX];
static int      s_buildCount;
static int      s_building;
static s_TtLine s_show[TT_QUEUE_MAX];
static int      s_showCount;
static Uint32   s_lastQueueMs;

static s_TtBake s_cache[TT_CACHE_MAX];

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontState; /* 0 untried, 1 ok, -1 none */
static int            s_fontScript = -1; /* script*2 + Cyrillic the face was picked for */

static int    s_glState; /* 0 untried, 1 ok, -1 failed */
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locTex, s_locColor;

int Pc_TtfText_Needed(void)
{
    /* Arabic discs repaint every Latin cell; the Russian discs and the
     * Cyrillic pack repaint the Latin capitals, which every title uses. */
    return Pc_ArActive() || Pc_RuActive() ||
           (Pc_LangPackActive() && Pc_LangPackFont() == LANG_PACK_FONT_CYRILLIC);
}

static int tt_cyrillic(void)
{
    const unsigned char* t = (const unsigned char*)Pc_LangQuick("Yes");

    for (; *t; t++)
        if (*t == 0xD0 || *t == 0xD1)
            return 1;
    return 0;
}

static int tt_needs_script_face(void)
{
    return Pc_LangQuickScript() != LANG_QUICK_SCRIPT_LATIN || tt_cyrillic();
}

static void tt_fonts_init(void)
{
    /* Barlow, the native menus' Latin face, unless the language needs a
     * script it lacks: then the quick menu's faces for that script first (a
     * CJK or Arabic face, or Oswald for Cyrillic; each carries Latin too). */
    static const char* fixed[] = {
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "gamedata/font/arabic.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    };
    const char* paths[24];
    int         n, i;

    n = tt_needs_script_face() ? Pc_LangQuickFontPaths(paths, 16) : 0;
    for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])) && n < 24; i++)
        paths[n++] = fixed[i];
    s_fontScript = Pc_LangQuickScript() * 2 + tt_cyrillic();
    s_fontState  = -1;
    for (i = 0; i < n; i++)
    {
        FILE* f = fopen(paths[i], "rb");
        long  n;

        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0)
        {
            unsigned char* buf = (unsigned char*)malloc((size_t)n);

            if (buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n &&
                stbtt_InitFont(&s_font, buf, stbtt_GetFontOffsetForIndex(buf, 0)))
            {
                /* stbtt reads this buffer on every call: it lives as long as the font. */
                s_fontData  = buf;
                s_fontState = 1;
                fclose(f);
                SH_DBG("[TTFTEXT] font: %s", paths[i]);
                return;
            }
            free(buf);
        }
        fclose(f);
    }
    SH_DBG("[TTFTEXT] no usable font - run-time English stays in the game font");
}

static int tt_utf8_next(const char** pp)
{
    const unsigned char* p = (const unsigned char*)*pp;
    int                  cp;

    if (p[0] < 0x80)
    {
        cp = p[0];
        *pp += 1;
    }
    else if ((p[0] & 0xE0) == 0xC0 && p[1])
    {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *pp += 2;
    }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2])
    {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp += 3;
    }
    else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
    {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *pp += 4;
    }
    else
    {
        cp = '?';
        *pp += 1;
    }
    return cp;
}

static void tt_script_check(void)
{
    int i;

    if (s_fontState == 0 || Pc_LangQuickScript() * 2 + tt_cyrillic() == s_fontScript)
        return;
    for (i = 0; i < TT_CACHE_MAX; i++)
        if (s_cache[i].tex)
        {
            if (s_glState == 1)
                glDeleteTextures(1, &s_cache[i].tex);
            s_cache[i].tex = 0;
        }
    free(s_fontData);
    s_fontData  = NULL;
    s_fontState = 0;
}

static float tt_measure(const char* text, float px)
{
    float       scale = stbtt_ScaleForPixelHeight(&s_font, px);
    float       x     = 0.0f;
    const char* p     = text;
    int         prev  = 0;

    while (*p)
    {
        int cp = tt_utf8_next(&p);
        int adv, lsb;

        if (prev)
            x += scale * (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp);
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        x   += scale * (float)adv;
        prev = cp;
    }
    return x;
}

/* One line into an RGBA texture: white ink, alpha = coverage; colour comes
 * from the draw's uniform. */
static int tt_bake(s_TtBake* b, const char* text, int pxH)
{
    float          scale = stbtt_ScaleForPixelHeight(&s_font, (float)pxH);
    int            ascent, descent, gap;
    int            w, h, base;
    unsigned char* alpha;
    unsigned char* rgba;
    float          x = 0.0f;
    const char*    p = text;
    int            prev = 0;
    int            i;

    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &gap);
    base = (int)(scale * (float)ascent + 0.5f);
    h    = (int)(scale * (float)(ascent - descent) + 1.5f);
    w    = (int)tt_measure(text, (float)pxH) + 4;
    if (w < 1 || h < 1 || w > 8192)
        return 0;

    alpha = (unsigned char*)calloc((size_t)w * (size_t)h, 1);
    rgba  = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    if (alpha == NULL || rgba == NULL)
    {
        free(alpha);
        free(rgba);
        return 0;
    }

    while (*p)
    {
        int cp = tt_utf8_next(&p);
        int adv, lsb, x0, y0, x1, y1;

        if (prev)
            x += scale * (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp);
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&s_font, cp, scale, scale, &x0, &y0, &x1, &y1);
        {
            int gx = (int)x + x0;
            int gy = base + y0;
            int gw = x1 - x0;
            int gh = y1 - y0;

            if (gw > 0 && gh > 0 && gx >= 0 && gy >= 0 && gx + gw <= w && gy + gh <= h)
                stbtt_MakeCodepointBitmap(&s_font, alpha + gy * w + gx, gw, gh, w, scale, scale, cp);
        }
        x   += scale * (float)adv;
        prev = cp;
    }

    for (i = 0; i < w * h; i++)
    {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = alpha[i];
    }
    free(alpha);

    glGenTextures(1, &b->tex);
    glBindTexture(GL_TEXTURE_2D, b->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    snprintf(b->text, sizeof(b->text), "%s", text);
    b->pxH  = pxH;
    b->w    = w;
    b->h    = h;
    b->inkW = x;
    return 1;
}

static s_TtBake* tt_cached(const char* text, int pxH)
{
    s_TtBake* slot = &s_cache[0];
    int       i;

    for (i = 0; i < TT_CACHE_MAX; i++)
    {
        s_TtBake* b = &s_cache[i];

        if (b->tex && b->pxH == pxH && strcmp(b->text, text) == 0)
        {
            b->used = SDL_GetTicks();
            return b;
        }
        if (!b->tex || b->used < slot->used)
            slot = b;
        if (!slot->tex)
            break;
    }

    if (slot->tex)
    {
        glDeleteTextures(1, &slot->tex);
        slot->tex = 0;
    }
    if (!tt_bake(slot, text, pxH))
        return NULL;
    slot->used = SDL_GetTicks();
    return slot;
}

static GLuint tt_shader(GLenum type, const char* src)
{
    GLuint      sh = glCreateShader(type);
    GLint       ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };

    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void tt_gl_init(void)
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

    s_glState = -1;
    vs = tt_shader(GL_VERTEX_SHADER, vs_src);
    fs = tt_shader(GL_FRAGMENT_SHADER, fs_src);
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
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    s_locTex   = glGetUniformLocation(s_prog, "u_tex");
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    /* PsyCross caches its bindings and skips rebinding: leave exactly what was bound. */
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

    s_glState = 1;
}

void Pc_TtfText_Queue(int x, int y, int tall, int colorId, const char* utf8, int maxW)
{
    s_TtLine* l;

    if (utf8 == NULL || utf8[0] == '\0')
        return;
    if (!s_building)
    {
        s_buildCount = 0;
        s_building   = 1;
    }
    if (s_buildCount >= TT_QUEUE_MAX)
        return;

    l          = &s_build[s_buildCount++];
    l->x       = x;
    l->y       = y;
    l->tall    = tall;
    l->colorId = colorId;
    l->maxW    = maxW;
    snprintf(l->text, sizeof(l->text), "%s", utf8);
}

int Pc_TtfText_Width(const char* utf8, int tall)
{
    if (s_fontState == 0)
        tt_fonts_init();
    if (s_fontState != 1 || utf8 == NULL)
        return 0;
    /* Native units: the measure is taken at the native pixel height itself. */
    return (int)(tt_measure(utf8, tall ? TT_TALL_PX : TT_SMALL_PX) + 0.5f);
}

static void tt_color(int colorId, float* rgb)
{
    /* STRING_COLORS (text_draw.c), where 128 is full intensity. */
    static const unsigned char c[8][3] = {
        { 160, 128, 64 }, { 32, 32, 32 }, { 24, 128, 40 }, { 8, 184, 96 },
        { 128, 0, 0 },    { 24, 128, 40 }, { 100, 100, 100 }, { 128, 128, 128 },
    };
    int i;

    if (colorId < 0 || colorId > 7)
        colorId = 7;
    for (i = 0; i < 3; i++)
    {
        float v = (float)c[colorId][i] / 128.0f;
        rgb[i]  = v > 1.0f ? 1.0f : v;
    }
}

void Pc_TtfText_Draw(void)
{
    int       pic[4];
    GLint     vp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0;
    GLint     prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint     prevScissorBox[4];
    GLint     prevDrawFb = 0, prevReadFb = 0;
    GLint     prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull, prevScissor;
    float     sx, sy, wideM;
    int       i;

    if (s_building)
    {
        memcpy(s_show, s_build, sizeof(s_TtLine) * (size_t)s_buildCount);
        s_showCount   = s_buildCount;
        s_building    = 0;
        s_lastQueueMs = SDL_GetTicks();
    }
    else if (SDL_GetTicks() - s_lastQueueMs > TT_STALE_MS)
    {
        s_showCount = 0;
    }
    if (s_showCount == 0)
        return;

    tt_script_check();
    if (s_fontState == 0)
        tt_fonts_init();
    if (s_fontState != 1)
        return;
    if (s_glState == 0)
        tt_gl_init();
    if (s_glState != 1)
        return;

    PsyX_GetDisplayViewport(&pic[0], &pic[1], &pic[2], &pic[3]);
    if (pic[2] <= 0 || pic[3] <= 0)
        return;
    /* A widened native menu (g_PsxWideMenu) spans -m..320+m across the
     * picture, not 0..320. */
    {
        extern int   g_PsxWideMenu;
        extern float PsyX_WideMenuMargin(void);

        wideM = g_PsxWideMenu ? PsyX_WideMenuMargin() : 0.0f;
    }
    sx = (float)pic[2] / (TT_NATIVE_W + 2.0f * wideM);
    sy = (float)pic[3] / TT_NATIVE_H;

    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    /* Same target as the mouse cursor: in borderless, framebuffer 0 is painted
     * over by the scene target's blit at present, so text drawn there never
     * shows. The viewport is the game picture, which is what x/y map onto. */
    glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
    glViewport(pic[0], pic[1], pic[2], pic[3]);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glUniform1i(s_locTex, 0);
    glEnable(GL_BLEND);
    /* Menus leave GL_FUNC_REVERSE_SUBTRACT behind (subtractive shading): white
     * ink would then subtract from black and draw nothing at all. */
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);

    for (i = 0; i < s_showCount; i++)
    {
        const s_TtLine* l      = &s_show[i];
        float           natPx  = l->tall ? TT_TALL_PX : TT_SMALL_PX;
        float           natW   = tt_measure(l->text, natPx);
        float           fit    = 1.0f;
        int             pxH;
        s_TtBake*       b;
        float           left, top, wPx, hPx, rgb[3];
        float           v[6][4];
        float           x0, x1, y0, y1;
        int             clipR;

        if (l->maxW > 0 && natW > (float)l->maxW)
        {
            fit = (float)l->maxW / natW;
            if (fit < TT_MIN_FIT)
                fit = TT_MIN_FIT;
        }
        pxH = (int)(natPx * fit * sy + 0.5f);
        if (pxH < 4)
            pxH = 4;
        b = tt_cached(l->text, pxH);
        if (b == NULL)
            continue;

        /* Window pixels, GL bottom-left origin; the line's top sits at y. */
        left = (float)pic[0] + ((float)l->x + wideM) * sx;
        top  = (float)pic[1] + (float)pic[3] - (float)l->y * sy - (natPx * (1.0f - fit) * 0.5f * sy);
        wPx  = (float)b->w;
        hPx  = (float)b->h;

        clipR = (l->maxW > 0) ? (int)(left + (float)l->maxW * sx) : pic[0] + pic[2];
        glScissor((GLint)left - 1, pic[1], clipR - (GLint)left + 2, pic[3]);

        x0 = ((left - (float)pic[0]) / (float)pic[2]) * 2.0f - 1.0f;
        x1 = ((left + wPx - (float)pic[0]) / (float)pic[2]) * 2.0f - 1.0f;
        y0 = ((top - (float)pic[1]) / (float)pic[3]) * 2.0f - 1.0f;
        y1 = ((top - hPx - (float)pic[1]) / (float)pic[3]) * 2.0f - 1.0f;

        v[0][0] = x0; v[0][1] = y0; v[0][2] = 0.0f; v[0][3] = 0.0f;
        v[1][0] = x0; v[1][1] = y1; v[1][2] = 0.0f; v[1][3] = 1.0f;
        v[2][0] = x1; v[2][1] = y0; v[2][2] = 1.0f; v[2][3] = 0.0f;
        v[3][0] = x1; v[3][1] = y0; v[3][2] = 1.0f; v[3][3] = 0.0f;
        v[4][0] = x0; v[4][1] = y1; v[4][2] = 0.0f; v[4][3] = 1.0f;
        v[5][0] = x1; v[5][1] = y1; v[5][2] = 1.0f; v[5][3] = 1.0f;

        tt_color(l->colorId, rgb);
        glUniform4f(s_locColor, rgb[0], rgb[1], rgb[2], 1.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    if (!prevScissor) glDisable(GL_SCISSOR_TEST);
    if (prevCull)     glEnable(GL_CULL_FACE);
    if (prevDepth)    glEnable(GL_DEPTH_TEST);
    if (!prevBlend)   glDisable(GL_BLEND);
    glBlendFuncSeparate(prevSrcRgb, prevDstRgb, prevSrcA, prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
}
