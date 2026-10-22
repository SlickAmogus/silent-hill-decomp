/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_hud.c - chat feed, composer, mic indicator and prompt.
 *
 * The GL is sh_net_ui.c's (online branch) and keeps every constraint that
 * file lists, each of which was paid for there:
 *   - its OWN program / VAO / atlas, with full state save and restore, and
 *     the previous PROGRAM put back (PsyCross caches it and skips
 *     glUseProgram when it believes the right one is current);
 *   - legacy GLSL with `precision mediump float` under GL_ES for ANGLE;
 *   - ONE atlas texture created at init and only ever sub-imaged -- a texture
 *     created mid-frame gets a name the game recycles;
 *   - attribute pointers re-asserted on every draw (the "overlay blotches"
 *     class);
 *   - explicit blend equation, since PsyCross can leave a subtract behind.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"
#include "pc_conscious.h"
#include "sh_log.h"

#define HU_ATLAS_W   1024
#define HU_ATLAS_H   1024
#define HU_ATLAS_PAD 2
#define HU_SLOT_MAX  160
#define HU_CACHE_MAX 96
#define HU_TEXT_MAX  128

static GLuint s_prog, s_vao, s_vbo, s_atlas;
static GLint  s_locColor;
static int    s_glReady;

static struct { int x, y, w, h, cw, ch, free; } s_slot[HU_SLOT_MAX];
static int s_slotCount;
static int s_atlasX, s_atlasY, s_atlasRowH;
static int s_atlasExhausted;

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk, s_fontTried;

static struct
{
    char     text[HU_TEXT_MAX];
    int      px;
    GLuint   slot;
    int      w, h;
    unsigned lastUse;
} s_cache[HU_CACHE_MAX];
static unsigned s_useClock;

static float s_vpW = 1920.0f, s_vpH = 1080.0f;

static GLuint Hu_Shader(GLenum type, const char* src)
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
        SH_DBG("[CONSCIOUS] HUD shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void Hu_AtlasReset(void)
{
    int i;
    s_atlasX    = HU_ATLAS_PAD + 1 + HU_ATLAS_PAD;
    s_atlasY    = HU_ATLAS_PAD;
    s_atlasRowH = 1;
    /* Slot 0 is the permanent white texel the fills draw from. */
    s_slot[0].x = HU_ATLAS_PAD; s_slot[0].y = HU_ATLAS_PAD;
    s_slot[0].w = s_slot[0].h = s_slot[0].cw = s_slot[0].ch = 1;
    s_slot[0].free = 0;
    s_slotCount = 1;
    for (i = 0; i < HU_CACHE_MAX; i++)
        s_cache[i].slot = 0;
}

static void Hu_GlInit(void)
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
    GLint  ok = 0, prevVao = 0, prevBuf = 0, prevProg = 0, prevTex = 0;

    s_glReady = -1;
    vs = Hu_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Hu_Shader(GL_FRAGMENT_SHADER, fs_src);
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

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    glGenTextures(1, &s_atlas);
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* A MIN filter that wants missing mip levels makes the texture incomplete,
     * and an incomplete texture samples as opaque black. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HU_ATLAS_W, HU_ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    {
        const unsigned char one[4] = { 255, 255, 255, 255 };
        glTexSubImage2D(GL_TEXTURE_2D, 0, HU_ATLAS_PAD, HU_ATLAS_PAD, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
    }
    Hu_AtlasReset();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glUseProgram((GLuint)prevProg);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    s_glReady = 1;
}

static GLuint Hu_Upload(const unsigned char* rgba, int w, int h)
{
    int idx, best = -1, bestArea = 0;

    if (s_glReady != 1 || w <= 0 || h <= 0 || w > HU_ATLAS_W - 2 * HU_ATLAS_PAD || h > HU_ATLAS_H - 2 * HU_ATLAS_PAD)
        return 0;
    for (idx = 1; idx < s_slotCount; idx++)
    {
        int area;
        if (!s_slot[idx].free || s_slot[idx].cw < w || s_slot[idx].ch < h) continue;
        area = s_slot[idx].cw * s_slot[idx].ch;
        if (best < 0 || area < bestArea) { best = idx; bestArea = area; }
    }
    if (best >= 0)
    {
        s_slot[best].w = w; s_slot[best].h = h; s_slot[best].free = 0;
        glBindTexture(GL_TEXTURE_2D, s_atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, s_slot[best].x, s_slot[best].y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return (GLuint)(best + 1);
    }
    if (s_slotCount >= HU_SLOT_MAX) { s_atlasExhausted = 1; return 0; }
    if (s_atlasX + w + HU_ATLAS_PAD > HU_ATLAS_W)
    {
        s_atlasX    = HU_ATLAS_PAD;
        s_atlasY   += s_atlasRowH + HU_ATLAS_PAD;
        s_atlasRowH = 0;
    }
    if (s_atlasY + h + HU_ATLAS_PAD > HU_ATLAS_H) { s_atlasExhausted = 1; return 0; }

    idx = s_slotCount++;
    s_slot[idx].x = s_atlasX; s_slot[idx].y = s_atlasY;
    s_slot[idx].w = s_slot[idx].cw = w;
    s_slot[idx].h = s_slot[idx].ch = h;
    s_slot[idx].free = 0;
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexSubImage2D(GL_TEXTURE_2D, 0, s_atlasX, s_atlasY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    s_atlasX += w + HU_ATLAS_PAD;
    if (h > s_atlasRowH) s_atlasRowH = h;
    return (GLuint)(idx + 1);
}

static void Hu_Quad(GLuint slot, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    float u0, v0, u1, v1;
    int   idx = (int)slot - 1;

    if (slot == 0 || idx >= s_slotCount)
        return;
    /* Half-texel inset so LINEAR taps never reach a neighbouring slot. */
    u0 = ((float)s_slot[idx].x + 0.5f) / (float)HU_ATLAS_W;
    v0 = ((float)s_slot[idx].y + 0.5f) / (float)HU_ATLAS_H;
    u1 = ((float)(s_slot[idx].x + s_slot[idx].w) - 0.5f) / (float)HU_ATLAS_W;
    v1 = ((float)(s_slot[idx].y + s_slot[idx].h) - 0.5f) / (float)HU_ATLAS_H;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = u0; v[0][3] = v0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = u0; v[1][3] = v1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = u1; v[2][3] = v0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = u1; v[3][3] = v0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = u0; v[4][3] = v1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = u1; v[5][3] = v1;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void Hu_FillPx(float x, float y, float w, float h, float r, float g, float b, float a)
{
    Hu_Quad(1, (x / s_vpW) * 2.0f - 1.0f, 1.0f - (y / s_vpH) * 2.0f,
            ((x + w) / s_vpW) * 2.0f - 1.0f, 1.0f - ((y + h) / s_vpH) * 2.0f, r, g, b, a);
}

static void Hu_FontInit(void)
{
    static const char* const paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    int i;
    if (s_fontTried)
        return;
    s_fontTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        FILE* f = fopen(paths[i], "rb");
        long  n;
        unsigned char* data;
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = (n > 0) ? (unsigned char*)malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n &&
            stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            fclose(f);
            /* stb_truetype reads this buffer on every glyph: never freed. */
            s_fontData = data;
            s_fontOk   = 1;
            return;
        }
        free(data);
        fclose(f);
    }
    SH_DBG("[CONSCIOUS] no usable font - the chat draws without text");
}

/* White glyphs with a one-pixel dark halo, so the chat reads over fog. */
static GLuint Hu_Bake(const char* text, int px, int* outW, int* outH)
{
    float  scale, penX, baseY;
    int    asc, desc, gap, W, H, x, y, stride, prev = 0;
    const int pad = 2;
    unsigned char *cov, *rgba, *scratch;
    const char* p;
    GLuint slot;

    if (!s_fontOk || !text || !text[0] || px < 6)
        return 0;
    scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    W = (int)((float)px * 0.66f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf((float)px * 1.4f) + 2 * pad;
    if (W > HU_ATLAS_W - 8) W = HU_ATLAS_W - 8;
    if (H > 200) return 0;
    stride  = W;
    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return 0; }

    penX  = (float)pad;
    baseY = (float)pad + (float)asc * scale;
    for (p = text; *p; )
    {
        int   cp = (unsigned char)*p++;
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev) penX += (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0; gh = gy1 - gy0;
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
                    unsigned char c;
                    if (dx < 0 || dx >= W) continue;
                    c = scratch[sy * gw + sx];
                    if (c > cov[dy * W + dx]) cov[dy * W + dx] = c;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += (float)adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }
    { int inkW = (int)ceilf(penX) + pad; if (inkW > 0 && inkW < W) W = inkW; }

    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            int c = cov[y * stride + x], dil = c, dx, dy, o = (y * W + x) * 4;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H && cov[ny * stride + nx] > dil)
                        dil = cov[ny * stride + nx];
                }
            rgba[o + 0] = rgba[o + 1] = rgba[o + 2] = (unsigned char)c;
            rgba[o + 3] = (unsigned char)dil;
        }
    }
    slot = Hu_Upload(rgba, W, H);
    *outW = W;
    *outH = H;
    free(cov); free(rgba); free(scratch);
    return slot;
}

static GLuint Hu_Text(const char* text, int px, int* outW, int* outH)
{
    int      i, victim = -1;
    unsigned oldest = 0xFFFFFFFFu;

    if (!text || !text[0])
        return 0;
    s_useClock++;
    for (i = 0; i < HU_CACHE_MAX; i++)
    {
        if (s_cache[i].slot && s_cache[i].px == px && strncmp(s_cache[i].text, text, HU_TEXT_MAX - 1) == 0)
        {
            s_cache[i].lastUse = s_useClock;
            *outW = s_cache[i].w;
            *outH = s_cache[i].h;
            return s_cache[i].slot;
        }
        if (!s_cache[i].slot)               { victim = i; oldest = 0; }
        else if (s_cache[i].lastUse < oldest) { victim = i; oldest = s_cache[i].lastUse; }
    }
    if (victim < 0)
        return 0;
    if (s_cache[victim].slot)
    {
        int idx = (int)s_cache[victim].slot - 1;
        if (idx > 0 && idx < s_slotCount) s_slot[idx].free = 1;
    }
    memset(&s_cache[victim], 0, sizeof(s_cache[victim]));
    {
        int    w = 0, h = 0;
        GLuint slot = Hu_Bake(text, px, &w, &h);
        if (!slot) return 0;
        SDL_strlcpy(s_cache[victim].text, text, HU_TEXT_MAX);
        s_cache[victim].px = px; s_cache[victim].slot = slot;
        s_cache[victim].w = w;   s_cache[victim].h = h;
        s_cache[victim].lastUse = s_useClock;
        *outW = w;
        *outH = h;
        return slot;
    }
}

static float Hu_DrawText(const char* text, float x, float y, int px, float r, float g, float b, float a)
{
    int    w = 0, h = 0;
    GLuint slot = Hu_Text(text, px, &w, &h);
    if (!slot)
        return 0.0f;
    Hu_Quad(slot, (x / s_vpW) * 2.0f - 1.0f, 1.0f - (y / s_vpH) * 2.0f,
            ((x + (float)w) / s_vpW) * 2.0f - 1.0f, 1.0f - ((y + (float)h) / s_vpH) * 2.0f, r, g, b, a);
    return (float)w;
}

static float Hu_TextWidth(const char* text, int px)
{
    int w = 0, h = 0;
    return Hu_Text(text, px, &w, &h) ? (float)w : 0.0f;
}

/* ------------------------------------------------------------------ */

static void Hu_DrawChat(int px)
{
    const int   n    = PcConChat_LineCount();
    const int   comp = PcConChat_IsOpen();
    const float lh   = (float)px * 1.3f;
    const float x    = s_vpH * 0.03f;
    const float bot  = s_vpH * 0.90f;
    float       y;
    int         i;

    if (comp)
    {
        float top = bot - lh * (float)(n + 1) - (float)px * 0.5f;
        Hu_FillPx(x - (float)px * 0.5f, top, s_vpW * 0.66f, bot - top + lh * 0.4f, 0.03f, 0.03f, 0.04f, 0.78f);
    }
    y = bot - lh * (float)(n + (comp ? 1 : 0));
    for (i = 0; i < n; i++)
    {
        float a = PcConChat_LineAlpha(i);
        int   k = PcConChat_LineKind(i);
        if (a > 0.0f)
        {
            if (k == PCCHAT_NPC)
                Hu_DrawText(PcConChat_LineText(i), x, y, px, 0.92f, 0.46f, 0.40f, a);
            else if (k == PCCHAT_PLAYER)
                Hu_DrawText(PcConChat_LineText(i), x, y, px, 0.86f, 0.84f, 0.70f, a);
            else if (k == PCCHAT_DIM)
                Hu_DrawText(PcConChat_LineText(i), x, y, px, 0.50f, 0.50f, 0.50f, a * 0.85f);
            else
                Hu_DrawText(PcConChat_LineText(i), x, y, px, 0.62f, 0.72f, 0.86f, a);
        }
        y += lh;
    }
    if (comp)
    {
        char line[240];
        snprintf(line, sizeof(line), "> %s_", PcConChat_ComposeText());
        Hu_DrawText(line, x, y, px, 0.98f, 0.94f, 0.72f, 1.0f);
    }
}

/* A microphone built from fills: capsule, cradle, stem and foot. Red with a
 * live level bar while listening, grey and struck through while off. */
static void Hu_DrawMic(const PcConHudState* st, int px)
{
    const float u  = (float)px * 0.09f;
    const float cx = s_vpW - s_vpH * 0.05f - u * 6.0f;
    const float cy = s_vpH * 0.05f;
    float r, g, b, a;
    char  label[64];
    int   i;

    if (st->micFailed)          { r = 0.45f; g = 0.45f; b = 0.45f; a = 0.55f; }
    else if (!st->micOn)        { r = 0.62f; g = 0.62f; b = 0.62f; a = 0.70f; }
    else if (st->micSpeaking)   { r = 1.00f; g = 0.30f; b = 0.25f; a = 1.00f; }
    else                        { r = 0.92f; g = 0.22f; b = 0.20f; a = 0.92f; }

    Hu_FillPx(cx - u * 4.0f, cy - u * 2.0f, u * 8.0f, u * 16.0f, 0.0f, 0.0f, 0.0f, 0.35f);
    Hu_FillPx(cx - u * 1.6f, cy - u * 0.4f, u * 3.2f, u * 1.0f, r, g, b, a);
    Hu_FillPx(cx - u * 2.0f, cy + u * 0.4f, u * 4.0f, u * 6.6f, r, g, b, a);
    Hu_FillPx(cx - u * 1.6f, cy + u * 6.8f, u * 3.2f, u * 1.0f, r, g, b, a);
    Hu_FillPx(cx - u * 3.4f, cy + u * 4.0f, u * 0.8f, u * 4.4f, r, g, b, a);
    Hu_FillPx(cx + u * 2.6f, cy + u * 4.0f, u * 0.8f, u * 4.4f, r, g, b, a);
    Hu_FillPx(cx - u * 3.4f, cy + u * 8.2f, u * 6.8f, u * 0.8f, r, g, b, a);
    Hu_FillPx(cx - u * 0.4f, cy + u * 9.0f, u * 0.8f, u * 2.4f, r, g, b, a);
    Hu_FillPx(cx - u * 2.4f, cy + u * 11.2f, u * 4.8f, u * 0.8f, r, g, b, a);

    if (!st->micOn)
    {
        for (i = 0; i < 16; i++)
            Hu_FillPx(cx - u * 4.0f + u * 0.5f * (float)i, cy - u * 1.0f + u * 0.8f * (float)i,
                      u * 1.0f, u * 1.0f, 0.95f, 0.95f, 0.95f, 0.85f);
    }
    else
    {
        float lv = st->micLevel * 8.0f;
        if (lv > 1.0f) lv = 1.0f;
        Hu_FillPx(cx + u * 5.0f, cy, u * 1.2f, u * 12.0f, 0.15f, 0.15f, 0.15f, 0.6f);
        Hu_FillPx(cx + u * 5.0f, cy + u * 12.0f * (1.0f - lv), u * 1.2f, u * 12.0f * lv, 0.35f, 0.95f, 0.40f, 0.95f);
    }

    snprintf(label, sizeof(label), st->micFailed ? "NO MIC" : st->micOn ? "MIC ON [%s]" : "MIC OFF [%s]",
             st->micKeyName);
    {
        const int lp = (int)((float)px * 0.62f);
        const float w = Hu_TextWidth(label, lp);
        Hu_DrawText(label, cx - w * 0.5f, cy + u * 13.0f, lp, r, g, b, 0.95f);
    }
}

#define BUBBLE_CHUNK 28
#define BUBBLE_MAXCHUNKS 24

/* The line over the speaker's head. Baked as word chunks laid end to end so a
 * long line never needs one texture wider than the atlas; when it is wider
 * than the bubble it scrolls right-to-left (read left to right) in step with
 * the voice, clipped to the bubble by a scissor. */
static void Hu_DrawBubble(const PcConHudState* st, int px, const GLint* vp)
{
    char        chunk[BUBBLE_MAXCHUNKS][BUBBLE_CHUNK + 8];
    float       cw[BUBBLE_MAXCHUNKS];
    int         n = 0, i;
    const char* p = st->bubbleText;
    const int   bp   = (int)((float)px * 0.95f);
    const float gap  = (float)bp * 0.28f;
    const float maxW = s_vpW * 0.40f;
    const float padX = (float)bp * 0.6f, padY = (float)bp * 0.3f;
    const float lh   = (float)bp * 1.4f;
    float       total = 0.0f, winW, x0, y0, off = 0.0f, x;
    float       a = st->bubbleAlpha;

    while (*p && n < BUBBLE_MAXCHUNKS)
    {
        int len = 0, lastSp = -1;
        while (p[len] && len < BUBBLE_CHUNK)
        {
            if (p[len] == ' ') lastSp = len;
            len++;
        }
        if (p[len] && lastSp > 0)
            len = lastSp;
        memcpy(chunk[n], p, (size_t)len);
        chunk[n][len] = '\0';
        cw[n] = Hu_TextWidth(chunk[n], bp);
        total += cw[n] + (n ? gap : 0.0f);
        n++;
        p += len;
        while (*p == ' ') p++;
    }
    if (n == 0)
        return;

    winW = total < maxW ? total : maxW;
    x0   = st->bubbleX * s_vpW - winW * 0.5f;
    if (x0 < padX) x0 = padX;
    if (x0 + winW + padX > s_vpW) x0 = s_vpW - winW - padX;
    y0 = st->bubbleY * s_vpH - lh - padY * 2.0f;
    if (y0 < padY) y0 = padY;

    if (total > maxW)
    {
        /* A beat at each end so the first and last words can be read. */
        float t = (st->bubbleProgress - 0.08f) / 0.80f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        off = t * (total - maxW);
    }

    Hu_FillPx(x0 - padX, y0 - padY, winW + padX * 2.0f, lh + padY * 2.0f, 0.02f, 0.02f, 0.03f, 0.72f * a);
    Hu_FillPx(x0 - padX, y0 - padY, winW + padX * 2.0f, 2.0f, 0.70f, 0.30f, 0.26f, 0.80f * a);
    {
        const float cx = st->bubbleX * s_vpW;
        const float ty = y0 + lh + padY;
        Hu_FillPx(cx - (float)bp * 0.30f, ty,                     (float)bp * 0.60f, (float)bp * 0.18f, 0.02f, 0.02f, 0.03f, 0.72f * a);
        Hu_FillPx(cx - (float)bp * 0.15f, ty + (float)bp * 0.18f, (float)bp * 0.30f, (float)bp * 0.18f, 0.02f, 0.02f, 0.03f, 0.72f * a);
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(vp[0] + (GLint)x0, vp[1] + (GLint)(s_vpH - (y0 + lh + padY)),
              (GLsizei)(winW + 1.0f), (GLsizei)(lh + padY * 2.0f));
    x = x0 - off;
    for (i = 0; i < n; i++)
    {
        if (x + cw[i] >= x0 - 2.0f && x <= x0 + winW + 2.0f)
            Hu_DrawText(chunk[i], x, y0, bp, 1.0f, 0.62f, 0.52f, a);
        x += cw[i] + gap;
    }
    glDisable(GL_SCISSOR_TEST);
}

void PcConHud_Draw(const PcConHudState* st)
{
    GLint     vp[4];
    GLint     prevProg = 0, prevTex = 0, prevVao = 0, prevBuf = 0, prevActive = 0;
    GLint     prevSrcRgb = 0, prevDstRgb = 0, prevSrcA = 0, prevDstA = 0, prevEqRgb = 0, prevEqA = 0;
    GLboolean prevDepth, prevBlend, prevCull, prevScissor;
    int       px;

    if (!st->micIcon && !st->status[0] && !st->bubble && !PcConChat_DisplayActive())
        return;
    if (s_glReady == 0)
        Hu_GlInit();
    if (s_glReady != 1)
        return;
    Hu_FontInit();

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0)
    {
        s_vpW = (float)vp[2];
        s_vpH = (float)vp[3];
    }
    px = (int)(s_vpH * 0.028f);
    if (px < 13) px = 13;
    if (px > 64) px = 64;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    if (s_atlasExhausted)
    {
        s_atlasExhausted = 0;
        Hu_AtlasReset();
    }

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (st->bubble && st->bubbleText && st->bubbleText[0])
        Hu_DrawBubble(st, px, vp);
    if (PcConChat_DisplayActive())
        Hu_DrawChat(px);
    if (st->micIcon)
        Hu_DrawMic(st, px);
    if (st->status[0])
    {
        const int   sp = (int)((float)px * 0.85f);
        const float w  = Hu_TextWidth(st->status, sp);
        Hu_DrawText(st->status, (s_vpW - w) * 0.5f, s_vpH * 0.08f, sp, 0.86f, 0.84f, 0.74f, 0.95f);
    }

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glActiveTexture((GLenum)prevActive);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend)  glDisable(GL_BLEND);
    if (prevDepth)   glEnable(GL_DEPTH_TEST);
    if (prevCull)    glEnable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
}
