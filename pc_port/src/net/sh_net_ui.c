/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_ui.c - every screen-space thing the online client draws.
 *
 * A fourth sibling of pc_quick_options.c / pc_rando_settings.c /
 * pc_confirm_dialog.c, and like them it repeats their hard-won GL constraints
 * rather than sharing code with them. Those constraints, each of which was
 * paid for:
 *
 *   - its OWN program / VAO / textures, with full state save and restore, and
 *     in particular the previously bound PROGRAM must be put back: PsyCross
 *     caches it and skips glUseProgram when it believes the right one is
 *     current, so leaving ours bound blacks out any later pass that never asks
 *     for a different shader.
 *   - LEGACY GLSL (attribute / varying / gl_FragColor, no #version), with
 *     `precision mediump float` under GL_ES so the ANGLE backends accept it.
 *   - ONE atlas texture, created at init and only ever sub-imaged. A texture
 *     created mid-frame inside the game's GL stream gets handed a name the
 *     game then recycles, and the panel starts sampling the game's art.
 *   - the vertex attribute pointers are re-asserted on every draw, not just at
 *     init: a stale a_uv is the "overlay blotches" class, and a 1x1 background
 *     texture hides it by making every uv sample the same texel.
 *   - all GL in Draw (post-capture). The game thread only reads input.
 *   - wall-clock timing, because the draw hook runs after the frame is
 *     captured and keeps running while the game is paused.
 *
 * What it draws:
 *   - the player list (key_online_players, default F11)
 *   - the memo composer (key_online_memo, default M)
 *   - the marker the player is standing on, at the bottom of the screen
 *   - the server's join / leave / death feed, top right
 *   - a connection line while the client is not yet connected
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"

#include "sh_net.h"
#include "sh_net_memo.h"
#include "sh_net_chat.h"
#include "sh_net_session.h"
#include "pc_config.h"
#include "pc_discord.h" /* Pc_MapAreaName */
#include "sh_log.h"

#define NU_ATLAS_W   1024
#define NU_ATLAS_H   1024
#define NU_ATLAS_PAD 2
#define NU_SLOT_MAX  192
#define NU_CACHE_MAX 128
#define NU_TEXT_MAX  72

/* Rows the player list shows at once before it starts saying "and N more". */
#define NU_LIST_ROWS 14
/* Rows each composer column shows around the selection. */
#define NU_COMPOSER_ROWS 9

#define NU_EVENT_SHOW_MS 6000
#define NU_EVENT_FADE_MS 1200
#define NU_EVENT_SLOTS   5

/* How long the master server has to stay unreachable before the player is
 * told. Long enough that a reconnect nobody noticed stays unnoticed. */
#define NU_LOST_QUIET_MS 15000

int g_ShNetPlayerListOpen;

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo, s_atlas;
static GLint  s_locColor;
static int    s_glReady;   /* 0 = untried, -1 = failed, 1 = ready */

static struct { int x, y, w, h, cw, ch, free; } s_slot[NU_SLOT_MAX];
static int s_slotCount;
static int s_atlasX, s_atlasY, s_atlasRowH;

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontTried;

static struct
{
    char   text[NU_TEXT_MAX];
    int    px;
    GLuint slot;
    int    w, h;
    unsigned lastUse;
} s_cache[NU_CACHE_MAX];
static unsigned s_useClock;

static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static unsigned int s_lostSinceMs;

static GLuint Nu_Shader(GLenum type, const char* src)
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
        SH_DBG("[NETUI] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void Nu_AtlasReset(void)
{
    s_atlasX    = NU_ATLAS_PAD + 1 + NU_ATLAS_PAD;
    s_atlasY    = NU_ATLAS_PAD;
    s_atlasRowH = 1;
    /* Slot 0 is the permanent white texel every panel background and divider
     * draws from; handing it to a string would repaint the panel with a word. */
    s_slot[0].x  = NU_ATLAS_PAD;
    s_slot[0].y  = NU_ATLAS_PAD;
    s_slot[0].w  = 1;
    s_slot[0].h  = 1;
    s_slot[0].cw = 1;
    s_slot[0].ch = 1;
    s_slot[0].free = 0;
    s_slotCount = 1;
}

static void Nu_GlInit(void)
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
    vs = Nu_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Nu_Shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
    {
        return;
    }

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
        SH_DBG("[NETUI] link failed: %s", log);
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
    if (!s_atlas)
    {
        glUseProgram((GLuint)prevProg);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* A MIN filter that wants mip levels which do not exist makes the texture
     * INCOMPLETE, and an incomplete texture samples as opaque black whatever
     * else is correct. Pinning the chain at level 0 puts that out of reach. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NU_ATLAS_W, NU_ATLAS_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    {
        const unsigned char one[4] = { 255, 255, 255, 255 };
        glTexSubImage2D(GL_TEXTURE_2D, 0, NU_ATLAS_PAD, NU_ATLAS_PAD, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, one);
    }
    Nu_AtlasReset();
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
    SH_DBG("[NETUI] GL ready (atlas %dx%d id=%u)", NU_ATLAS_W, NU_ATLAS_H, (unsigned)s_atlas);
}

/* Returns a slot handle (index + 1), or 0. */
static GLuint Nu_Upload(const unsigned char* rgba, int w, int h)
{
    int idx;

    if (s_glReady != 1 || !s_atlas || w <= 0 || h <= 0)
    {
        return 0;
    }
    if (w > NU_ATLAS_W - 2 * NU_ATLAS_PAD || h > NU_ATLAS_H - 2 * NU_ATLAS_PAD)
    {
        return 0;
    }

    /* Recycle first: a value that re-bakes every frame (a ping in
     * milliseconds) would otherwise burn one slot per frame and the atlas
     * would run dry within a minute of the panel being open. Best fit, so a
     * short string does not claim a long string's rectangle. */
    {
        int best = -1, bestArea = 0;
        for (idx = 1; idx < s_slotCount; idx++)
        {
            int area;
            if (!s_slot[idx].free || s_slot[idx].cw < w || s_slot[idx].ch < h)
            {
                continue;
            }
            area = s_slot[idx].cw * s_slot[idx].ch;
            if (best < 0 || area < bestArea) { best = idx; bestArea = area; }
        }
        if (best >= 0)
        {
            s_slot[best].w    = w;
            s_slot[best].h    = h;
            s_slot[best].free = 0;
            glBindTexture(GL_TEXTURE_2D, s_atlas);
            glTexSubImage2D(GL_TEXTURE_2D, 0, s_slot[best].x, s_slot[best].y, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            return (GLuint)(best + 1);
        }
    }

    if (s_slotCount >= NU_SLOT_MAX)
    {
        return 0;
    }
    if (s_atlasX + w + NU_ATLAS_PAD > NU_ATLAS_W)
    {
        s_atlasX     = NU_ATLAS_PAD;
        s_atlasY    += s_atlasRowH + NU_ATLAS_PAD;
        s_atlasRowH  = 0;
    }
    if (s_atlasY + h + NU_ATLAS_PAD > NU_ATLAS_H)
    {
        return 0;
    }

    idx = s_slotCount++;
    s_slot[idx].x  = s_atlasX;
    s_slot[idx].y  = s_atlasY;
    s_slot[idx].w  = w;
    s_slot[idx].h  = h;
    s_slot[idx].cw = w;
    s_slot[idx].ch = h;
    s_slot[idx].free = 0;

    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexSubImage2D(GL_TEXTURE_2D, 0, s_atlasX, s_atlasY, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    s_atlasX += w + NU_ATLAS_PAD;
    if (h > s_atlasRowH)
    {
        s_atlasRowH = h;
    }
    return (GLuint)(idx + 1);
}

/* Quad in NDC (yTop > yBot). `slot` is an atlas handle, not a texture name. */
static void Nu_Quad(GLuint slot, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    float u0, v0, u1, v1;
    int   idx = (int)slot - 1;

    if (slot == 0 || idx < 0 || idx >= s_slotCount || !s_atlas)
    {
        return;
    }

    /* Half-texel inset: with LINEAR filtering an edge tap would otherwise
     * reach into a neighbouring slot's padding. */
    u0 = ((float)s_slot[idx].x + 0.5f) / (float)NU_ATLAS_W;
    v0 = ((float)s_slot[idx].y + 0.5f) / (float)NU_ATLAS_H;
    u1 = ((float)(s_slot[idx].x + s_slot[idx].w) - 0.5f) / (float)NU_ATLAS_W;
    v1 = ((float)(s_slot[idx].y + s_slot[idx].h) - 0.5f) / (float)NU_ATLAS_H;

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

/* Solid fill, straight from the white texel. */
static void Nu_Fill(float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    Nu_Quad(1, x0, yTop, x1, yBot, r, g, b, a);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static unsigned char* Nu_ReadFile(const char* path, long* outSize)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (outSize) *outSize = n;
    return buf;
}

static void Nu_FontInit(void)
{
    static const char* const paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/Library/Fonts/Arial.ttf"
    };
    int i;

    if (s_fontTried)
    {
        return;
    }
    s_fontTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        long           sz   = 0;
        unsigned char* data = Nu_ReadFile(paths[i], &sz);
        if (!data)
        {
            continue;
        }
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            /* stb_truetype does not copy the file: it reads this buffer on
             * every glyph, so it must outlive the process. Never freed. */
            s_fontData = data;
            s_fontOk   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[NETUI] no usable font - the online panels will draw without text");
}

/* Rasterize one line of ASCII to white-with-coverage-alpha, trimmed to the ink
 * so centring and right-alignment use the real width. */
unsigned char* ShNetUi_RasterizeText(const char* text, int px, int* outW, int* outH)
{
    const char*    p;
    float          scale, penX, baseY;
    int            asc, desc, gap, W, H, i, stride, x, y;
    const int      pad = 2;
    int            prev = 0;
    unsigned char *cov, *rgba, *scratch;

    Nu_FontInit();
    if (!s_fontOk || !text || !text[0] || px < 6)
    {
        return NULL;
    }
    scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)((float)px * 0.66f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf((float)px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 1024 || H > 128)
    {
        return NULL;
    }
    stride = W;
    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return NULL; }

    penX  = (float)pad;
    baseY = (float)pad + (float)asc * scale;
    p     = text;
    while (*p)
    {
        int   cp = (unsigned char)*p++;
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev) penX += (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f,
                                            &gx0, &gy0, &gx1, &gy1);
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
                    int dx = (int)penX + gx0 + sx; unsigned char c;
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

    /* White glyph with a dark halo: alpha is coverage dilated by one pixel,
     * colour is the glyph where it is solid and near-black in the halo ring. */
    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            int   c = cov[y * stride + x];
            int   dil = c, dx, dy;
            int   o = (y * W + x) * 4;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                    {
                        int nc = cov[ny * stride + nx];
                        if (nc > dil) dil = nc;
                    }
                }
            rgba[o + 0] = (unsigned char)c;
            rgba[o + 1] = (unsigned char)c;
            rgba[o + 2] = (unsigned char)c;
            rgba[o + 3] = (unsigned char)dil;
        }
    }

    free(cov); free(scratch);
    if (outW) *outW = W;
    if (outH) *outH = H;
    return rgba;
}

static GLuint Nu_Bake(const char* text, int px, int* outW, int* outH)
{
    const char*    p;
    float          scale, penX, baseY;
    int            asc, desc, gap, W, H, i, stride;
    const int      pad = 1;
    int            prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint         slot;

    if (!s_fontOk || !text || !text[0] || px < 6)
    {
        return 0;
    }

    scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)((float)px * 0.66f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf((float)px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 2048 || H > 256)
    {
        return 0;
    }
    stride = W;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    penX  = (float)pad;
    baseY = (float)pad + (float)asc * scale;
    p     = text;
    while (*p)
    {
        int   cp = (unsigned char)*p++;
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;

        if (prev)
        {
            penX += (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        }
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f,
                                            &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw,
                                              scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int           dx = (int)penX + gx0 + sx;
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

    {
        int inkW = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
    }
    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
    }

    slot = Nu_Upload(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return slot;
}

/* Baked strings, keyed on (text, size). A ping value re-bakes constantly and a
 * label never does; one cache with LRU eviction handles both without the
 * caller having to know which it is holding. */
static GLuint Nu_Text(const char* text, int px, int* outW, int* outH)
{
    int i, victim = -1;
    unsigned oldest = 0xFFFFFFFFu;

    if (!text || !text[0])
    {
        return 0;
    }
    s_useClock++;

    for (i = 0; i < NU_CACHE_MAX; i++)
    {
        if (s_cache[i].slot && s_cache[i].px == px &&
            strncmp(s_cache[i].text, text, NU_TEXT_MAX - 1) == 0)
        {
            s_cache[i].lastUse = s_useClock;
            if (outW) *outW = s_cache[i].w;
            if (outH) *outH = s_cache[i].h;
            return s_cache[i].slot;
        }
        if (!s_cache[i].slot)
        {
            victim = i;
            oldest = 0;
        }
        else if (s_cache[i].lastUse < oldest)
        {
            victim = i;
            oldest = s_cache[i].lastUse;
        }
    }

    if (victim < 0)
    {
        return 0;
    }
    if (s_cache[victim].slot)
    {
        int idx = (int)s_cache[victim].slot - 1;
        if (idx > 0 && idx < s_slotCount)
        {
            s_slot[idx].free = 1;
        }
    }
    memset(&s_cache[victim], 0, sizeof(s_cache[victim]));

    {
        int w = 0, h = 0;
        GLuint slot = Nu_Bake(text, px, &w, &h);
        if (!slot)
        {
            return 0;
        }
        SDL_strlcpy(s_cache[victim].text, text, NU_TEXT_MAX);
        s_cache[victim].px      = px;
        s_cache[victim].slot    = slot;
        s_cache[victim].w       = w;
        s_cache[victim].h       = h;
        s_cache[victim].lastUse = s_useClock;
        if (outW) *outW = w;
        if (outH) *outH = h;
        return slot;
    }
}

/* Draws `text` with its LEFT edge at viewport pixel x and its TOP at y (y
 * measured down from the top). Returns the drawn width in pixels. */
static float Nu_DrawText(const char* text, float x, float y, int px,
                         float r, float g, float b, float a)
{
    int    w = 0, h = 0;
    GLuint slot = Nu_Text(text, px, &w, &h);
    float  x0, x1, yT, yB;

    if (!slot)
    {
        return 0.0f;
    }
    x0 = (x / s_vpW) * 2.0f - 1.0f;
    x1 = ((x + (float)w) / s_vpW) * 2.0f - 1.0f;
    yT = 1.0f - (y / s_vpH) * 2.0f;
    yB = 1.0f - ((y + (float)h) / s_vpH) * 2.0f;
    Nu_Quad(slot, x0, yT, x1, yB, r, g, b, a);
    return (float)w;
}

static float Nu_TextWidth(const char* text, int px)
{
    int w = 0, h = 0;
    return Nu_Text(text, px, &w, &h) ? (float)w : 0.0f;
}

static void Nu_DrawTextRight(const char* text, float xRight, float y, int px,
                             float r, float g, float b, float a)
{
    Nu_DrawText(text, xRight - Nu_TextWidth(text, px), y, px, r, g, b, a);
}

static void Nu_DrawTextCentered(const char* text, float cx, float y, int px,
                                float r, float g, float b, float a)
{
    Nu_DrawText(text, cx - Nu_TextWidth(text, px) * 0.5f, y, px, r, g, b, a);
}

/* Panel in viewport pixels: a translucent slab with a one-pixel edge. */
static void Nu_Panel(float x, float y, float w, float h, float alpha)
{
    const float x0 = (x / s_vpW) * 2.0f - 1.0f;
    const float x1 = ((x + w) / s_vpW) * 2.0f - 1.0f;
    const float yT = 1.0f - (y / s_vpH) * 2.0f;
    const float yB = 1.0f - ((y + h) / s_vpH) * 2.0f;
    const float px = 2.0f / s_vpW;
    const float py = 2.0f / s_vpH;

    Nu_Fill(x0, yT, x1, yB, 0.04f, 0.045f, 0.05f, 0.86f * alpha);
    Nu_Fill(x0, yT, x1, yT - py, 0.45f, 0.44f, 0.40f, 0.55f * alpha);
    Nu_Fill(x0, yB + py, x1, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
    Nu_Fill(x0, yT, x0 + px, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
    Nu_Fill(x1 - px, yT, x1, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
}

/* ------------------------------------------------------------------ */
/* Event feed                                                          */
/* ------------------------------------------------------------------ */

static struct { char text[SHNET_EVENT_MAX]; unsigned int ms; } s_events[NU_EVENT_SLOTS];
static int s_eventCount;

static void Nu_PumpEvents(void)
{
    char line[SHNET_EVENT_MAX];
    const unsigned int now = SDL_GetTicks();
    int   i;

    while (ShNet_PopEvent(line, (int)sizeof(line)))
    {
        if (s_eventCount == NU_EVENT_SLOTS)
        {
            memmove(&s_events[0], &s_events[1], sizeof(s_events[0]) * (NU_EVENT_SLOTS - 1));
            s_eventCount--;
        }
        SDL_strlcpy(s_events[s_eventCount].text, line, SHNET_EVENT_MAX);
        s_events[s_eventCount].ms = now;
        s_eventCount++;
    }

    i = 0;
    while (i < s_eventCount)
    {
        if (now - s_events[i].ms > NU_EVENT_SHOW_MS + NU_EVENT_FADE_MS)
        {
            memmove(&s_events[i], &s_events[i + 1],
                    sizeof(s_events[0]) * (size_t)(s_eventCount - i - 1));
            s_eventCount--;
        }
        else
        {
            i++;
        }
    }
}

static void Nu_DrawEvents(int px)
{
    const unsigned int now = SDL_GetTicks();
    float y = s_vpH * 0.06f;
    int   i;

    if (!g_PcConfig.onlineEvents)
    {
        return;
    }
    for (i = 0; i < s_eventCount; i++)
    {
        unsigned int age = now - s_events[i].ms;
        float        a   = 1.0f;
        if (age > NU_EVENT_SHOW_MS)
        {
            a = 1.0f - (float)(age - NU_EVENT_SHOW_MS) / (float)NU_EVENT_FADE_MS;
        }
        if (a <= 0.0f)
        {
            continue;
        }
        Nu_DrawTextRight(s_events[i].text, s_vpW - s_vpH * 0.03f, y, px,
                         0.78f, 0.76f, 0.70f, a);
        y += (float)px * 1.3f;
    }
}

/* ------------------------------------------------------------------ */
/* Player list                                                         */
/* ------------------------------------------------------------------ */

static void Nu_DrawPlayerList(int px)
{
    const int   big   = (int)((float)px * 1.25f);
    const float pad   = (float)px * 0.9f;
    const float rowH  = (float)px * 1.45f;
    const int   count = ShNet_PeerCount();
    const int   shown = (count > NU_LIST_ROWS) ? NU_LIST_ROWS : count;
    const float w     = s_vpW * 0.42f < 420.0f ? 420.0f : s_vpW * 0.42f;
    const int   sess  = ShSession_MemberCount();
    const float h     = pad * 2.0f + (float)big * 1.5f + (float)px * 1.4f +
                        rowH * (float)(shown + 2) +
                        (sess > 0 ? rowH * (float)(sess + 2) : 0.0f);
    const float x     = (s_vpW - w) * 0.5f;
    const float y     = s_vpH * 0.12f;
    float       ty;
    int         i;
    char        buf[128];

    Nu_Panel(x, y, w, h, 1.0f);

    ty = y + pad;
    Nu_DrawText(ShNet_ServerName()[0] ? ShNet_ServerName() : "Silent Hill Online",
                x + pad, ty, big, 0.86f, 0.83f, 0.74f, 1.0f);
    {
        const int st = ShNet_Status();
        if (st == SHNET_ST_CONNECTED)
        {
            snprintf(buf, sizeof(buf), "%d online   %d ms", ShNet_PeerTotal(), ShNet_PingMs());
            Nu_DrawTextRight(buf, x + w - pad, ty, big, 0.55f, 0.62f, 0.55f, 1.0f);
        }
        else
        {
            Nu_DrawTextRight(ShNet_StatusText(), x + w - pad, ty, big,
                             0.72f, 0.52f, 0.42f, 1.0f);
        }
    }
    ty += (float)big * 1.5f;

    if (ShNet_Motd()[0])
    {
        Nu_DrawText(ShNet_Motd(), x + pad, ty, px, 0.58f, 0.56f, 0.52f, 1.0f);
    }
    ty += (float)px * 1.4f;

    {
        const float x0 = (x + pad) / s_vpW * 2.0f - 1.0f;
        const float x1 = (x + w - pad) / s_vpW * 2.0f - 1.0f;
        const float yy = 1.0f - (ty / s_vpH) * 2.0f;
        Nu_Fill(x0, yy, x1, yy - 2.0f / s_vpH, 0.4f, 0.39f, 0.36f, 0.5f);
    }
    ty += rowH * 0.4f;

    if (count == 0)
    {
        Nu_DrawText(ShNet_Status() == SHNET_ST_CONNECTED
                        ? "No one else is here."
                        : "Not connected.",
                    x + pad, ty, px, 0.5f, 0.48f, 0.45f, 1.0f);
    }

    for (i = 0; i < shown; i++)
    {
        const ShNetPeer* p = ShNet_Peer(i);
        const int        me = p && p->playerId == ShNet_SelfId();
        float            r = me ? 0.85f : 0.72f;
        float            g = me ? 0.80f : 0.70f;
        float            b = me ? 0.58f : 0.66f;

        if (!p)
        {
            continue;
        }
        Nu_DrawText(p->name, x + pad, ty, px, r, g, b, 1.0f);
        Nu_DrawText(p->mapIdx >= 0 && p->mapIdx < 64 ? Pc_MapAreaName(p->mapIdx) : "-",
                    x + w * 0.38f, ty, px, 0.56f, 0.56f, 0.54f, 1.0f);
        snprintf(buf, sizeof(buf), "%d ms", p->pingMs);
        Nu_DrawTextRight(buf, x + w - pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
        ty += rowH;
    }

    if (count > shown)
    {
        snprintf(buf, sizeof(buf), "and %d more", count - shown);
        Nu_DrawText(buf, x + pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
        ty += rowH;
    }

    /* The Steam session, when there is one. It is a different thing from the
     * roster above -- the people you invited, not everyone who happens to be
     * on the same master server -- so it gets its own block rather than being
     * mixed into the list. */
    if (ShSession_MemberCount() > 0)
    {
        char sline[128];
        int  n = ShSession_MemberCount();
        int  k;

        ty += rowH * 0.5f;
        {
            const float x0 = (x + pad) / s_vpW * 2.0f - 1.0f;
            const float x1 = (x + w - pad) / s_vpW * 2.0f - 1.0f;
            const float yy = 1.0f - (ty / s_vpH) * 2.0f;
            Nu_Fill(x0, yy, x1, yy - 2.0f / s_vpH, 0.4f, 0.39f, 0.36f, 0.5f);
        }
        ty += rowH * 0.4f;

        ShSession_StatusLine(sline, (int)sizeof(sline));
        Nu_DrawText(sline, x + pad, ty, px, 0.62f, 0.66f, 0.58f, 1.0f);
        ty += rowH;

        for (k = 0; k < n; k++)
        {
            const ShSessionMember* m = ShSession_Member(k);
            if (!m)
            {
                continue;
            }
            Nu_DrawText(m->name, x + pad + (float)px, ty, px,
                        m->linked ? 0.78f : 0.46f,
                        m->linked ? 0.76f : 0.45f,
                        m->linked ? 0.62f : 0.43f, 1.0f);
            if (m->mapIdx >= 0 && m->mapIdx < 64)
            {
                Nu_DrawText(Pc_MapAreaName(m->mapIdx), x + w * 0.38f, ty, px,
                            0.52f, 0.52f, 0.50f, 1.0f);
            }
            if (m->pingMs >= 0)
            {
                snprintf(buf, sizeof(buf), "%d ms", m->pingMs);
            }
            else
            {
                snprintf(buf, sizeof(buf), "%s", m->linked ? "--" : "no link");
            }
            Nu_DrawTextRight(buf, x + w - pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
            ty += rowH;
        }
    }

    ty = y + h - pad - (float)px;
    Nu_DrawText("F11 close     M leave a message", x + pad, ty, px,
                0.42f, 0.42f, 0.40f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Memo composer                                                       */
/* ------------------------------------------------------------------ */

static void Nu_DrawComposer(int px)
{
    const float pad  = (float)px * 0.9f;
    const float rowH = (float)px * 1.4f;
    const float w    = s_vpW * 0.46f < 460.0f ? 460.0f : s_vpW * 0.46f;
    const float h    = pad * 2.0f + rowH * (float)(NU_COMPOSER_ROWS + 4);
    const float x    = (s_vpW - w) * 0.5f;
    const float y    = s_vpH * 0.16f;
    const int   col  = ShNetMemo_ComposerColumn();
    const float colW = (w - pad * 3.0f) * 0.5f;
    float       ty;
    int         i;
    char        buf[128];

    Nu_Panel(x, y, w, h, 1.0f);

    ty = y + pad;
    Nu_DrawText("LEAVE A MESSAGE", x + pad, ty, px, 0.82f, 0.79f, 0.70f, 1.0f);
    ty += rowH * 1.3f;

    for (i = 0; i < 2; i++)
    {
        const int   isPhrase = (i == 0);
        const int   sel      = isPhrase ? ShNetMemo_ComposerPhrase() : ShNetMemo_ComposerWord();
        const int   total    = isPhrase ? ShNetMemo_PhraseCount() : ShNetMemo_WordCount();
        const float cx       = x + pad + (float)i * (colW + pad);
        const int   half     = NU_COMPOSER_ROWS / 2;
        float       cy       = ty;
        int         k;

        if (col == i)
        {
            const float x0 = (cx - pad * 0.4f) / s_vpW * 2.0f - 1.0f;
            const float x1 = (cx + colW) / s_vpW * 2.0f - 1.0f;
            const float yT = 1.0f - ((cy - pad * 0.3f) / s_vpH) * 2.0f;
            const float yB = 1.0f - ((cy + rowH * (float)NU_COMPOSER_ROWS) / s_vpH) * 2.0f;
            Nu_Fill(x0, yT, x1, yB, 0.16f, 0.16f, 0.14f, 0.55f);
        }

        for (k = -half; k <= half; k++)
        {
            /* Wrap the visible window so the list reads as a wheel: the entry
             * above the first is the last, which is how it navigates. */
            int idx = ((sel + k) % total + total) % total;
            int on  = (k == 0);
            const char* label = isPhrase ? ShNetMemo_Phrase(idx) : ShNetMemo_Word(idx);

            if (isPhrase)
            {
                /* Show the template with its slot filled by a placeholder, so
                 * the player reads a sentence rather than a printf string. */
                const char* p = label;
                int         o = 0;
                while (*p && o < (int)sizeof(buf) - 8)
                {
                    if (p[0] == '%' && p[1] == 's')
                    {
                        buf[o++] = '_'; buf[o++] = '_'; buf[o++] = '_';
                        p += 2;
                        continue;
                    }
                    buf[o++] = *p++;
                }
                buf[o] = '\0';
                label  = buf;
            }
            else
            {
                const char* grp = ShNetMemo_WordGroup(idx);
                if (grp && !on)
                {
                    Nu_DrawTextRight(grp, cx + colW, cy, (int)((float)px * 0.8f),
                                     0.38f, 0.42f, 0.38f, 1.0f);
                }
            }

            Nu_DrawText(label, cx + (on ? (float)px * 0.5f : 0.0f), cy, px,
                        on ? 0.92f : 0.46f,
                        on ? 0.88f : 0.45f,
                        on ? 0.72f : 0.43f, on ? 1.0f : 0.85f);
            cy += rowH;
        }
    }

    ty += rowH * (float)NU_COMPOSER_ROWS + rowH * 0.4f;
    ShNetMemo_ComposerPreview(buf, (int)sizeof(buf));
    Nu_DrawTextCentered(buf, x + w * 0.5f, ty, (int)((float)px * 1.3f),
                        0.88f, 0.84f, 0.62f, 1.0f);
    ty += rowH * 1.4f;
    Nu_DrawTextCentered(col == 0 ? "Up/Down choose     Enter next     Esc cancel"
                                 : "Up/Down word     Left/Right group     Enter leave it     Esc cancel",
                        x + w * 0.5f, ty, (int)((float)px * 0.9f),
                        0.42f, 0.42f, 0.40f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* The marker under your feet                                          */
/* ------------------------------------------------------------------ */

static void Nu_DrawNearbyMemo(int px)
{
    const int        idx = ShNetMemo_NearestReadable();
    const ShNetMemo* m;
    char             text[128];
    char             sub[128];
    const int        big = (int)((float)px * 1.4f);
    float            y;

    if (idx < 0)
    {
        return;
    }
    m = ShNet_Memo(idx);
    if (!m)
    {
        return;
    }

    ShNetMemo_Text(m, text, (int)sizeof(text));
    y = s_vpH * 0.80f;

    {
        const float tw = Nu_TextWidth(text, big);
        const float bx = (s_vpW - tw) * 0.5f - (float)px;
        Nu_Panel(bx, y - (float)px * 0.5f, tw + (float)px * 2.0f, (float)big * 2.6f, 0.85f);
    }

    Nu_DrawTextCentered(text, s_vpW * 0.5f, y, big,
                        m->kind == SHNET_MARK_DEATH ? 0.80f : 0.88f,
                        m->kind == SHNET_MARK_DEATH ? 0.42f : 0.84f,
                        m->kind == SHNET_MARK_DEATH ? 0.40f : 0.62f, 1.0f);

    if (m->kind == SHNET_MARK_MEMO)
    {
        snprintf(sub, sizeof(sub), "- %s   (%+d)", m->owner[0] ? m->owner : "someone",
                 (int)m->rating);
    }
    else
    {
        snprintf(sub, sizeof(sub), "%s", "");
    }
    if (sub[0])
    {
        Nu_DrawTextCentered(sub, s_vpW * 0.5f, y + (float)big * 1.25f,
                            (int)((float)px * 0.9f), 0.48f, 0.48f, 0.46f, 1.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

void ShNetUi_PreloadGL(void)
{
    if (s_glReady == 0)
    {
        Nu_GlInit();
        Nu_FontInit();
    }
}

void ShNetUi_TogglePlayerList(void)
{
    if (!g_PcConfig.onlineEnabled && !g_PcConfig.onlineSteam)
    {
        return;
    }
    g_ShNetPlayerListOpen = !g_ShNetPlayerListOpen;
    if (g_ShNetPlayerListOpen)
    {
        ShNet_RequestRoster();
    }
}

static void Nu_DrawChat(int px)
{
    const int   n    = ShNetChat_LineCount();
    const int   comp = ShNetChat_Composing();
    const float lh   = (float)px * 1.32f;
    const float x    = s_vpH * 0.03f;
    const float bot  = s_vpH * (comp ? 0.9f : 0.93f);
    float       y;
    int         i;

    if (n <= 0 && !comp)
    {
        return;
    }

    /* Backing slab only while composing -- idle lines float over the scene the
     * way a console-style feed does, so they never box off the corner. */
    if (comp)
    {
        float top = bot - lh * (float)(n + 1) - (float)px * 0.5f;
        Nu_Panel(x - (float)px * 0.5f, top, s_vpW * 0.42f, bot - top + lh,
                 0.85f);
    }

    y = bot - lh * (float)(n + (comp ? 1 : 0));
    for (i = 0; i < n; i++)
    {
        float a = ShNetChat_LineAlpha(i);
        int   g = (ShNetChat_LineScope(i) == SHNET_CHAT_GAME);
        if (a <= 0.0f)
        {
            y += lh;
            continue;
        }
        Nu_DrawText(ShNetChat_LineText(i), x, y, px,
                    g ? 0.86f : 0.72f,
                    g ? 0.80f : 0.78f,
                    g ? 0.62f : 0.86f, a);
        y += lh;
    }

    if (comp)
    {
        char        line[200];
        const int   g = (ShNetChat_Scope() == SHNET_CHAT_GAME);
        snprintf(line, sizeof(line), "%s %s_",
                 g ? "[Game]" : "[All]", ShNetChat_ComposeText());
        Nu_DrawText(line, x, y, px,
                    g ? 0.95f : 0.80f, g ? 0.90f : 0.86f, g ? 0.66f : 0.95f, 1.0f);
    }
}

void ShNetUi_Draw(void)
{
    GLint     vp[4];
    GLint     prevProg, prevTex, prevVao, prevBuf, prevActive;
    GLint     prevSrcRgb, prevDstRgb, prevSrcA, prevDstA, prevEqRgb, prevEqA;
    GLboolean prevDepth, prevBlend, prevCull, prevScissor;
    int       px;
    int       drawList, drawComposer, drawStatus;

    if (!g_PcConfig.onlineEnabled && !g_PcConfig.onlineSteam)
    {
        return;
    }
    if (s_glReady == 0)
    {
        ShNetUi_PreloadGL();
    }
    if (s_glReady != 1)
    {
        return;
    }

    Nu_PumpEvents();

    drawList     = g_ShNetPlayerListOpen;
    drawComposer = ShNetMemo_ComposerActive();
    /* SEAMLESS BY DESIGN. The living world is not something you connect to,
     * it is something you are in, so the normal path -- resolving, connecting,
     * reconnecting after a blip -- says NOTHING. Two exceptions, both cases
     * where the player has to actually do something:
     *
     *   REJECTED  the server refused us (wrong password, wrong version, full).
     *             It will never resolve itself.
     *   LOST      but only once it has stayed lost. A dropped packet or a map
     *             load should not put a banner on screen; a server that has
     *             genuinely gone away for a quarter of a minute should.
     *
     * F11 always tells the truth for anyone who wants to look. */
    {
        const int st = ShNet_Status();
        if (st == SHNET_ST_LOST)
        {
            if (s_lostSinceMs == 0)
            {
                s_lostSinceMs = SDL_GetTicks();
            }
        }
        else
        {
            s_lostSinceMs = 0;
        }
        drawStatus = (st == SHNET_ST_REJECTED) ||
                     (st == SHNET_ST_LOST && s_lostSinceMs != 0 &&
                      SDL_GetTicks() - s_lostSinceMs > NU_LOST_QUIET_MS);
    }

    if (!drawList && !drawComposer && !drawStatus && s_eventCount == 0 &&
        ShNetMemo_NearestReadable() < 0 && !ShNetMemo_JustPlaced() &&
        !ShNetChat_DisplayActive())
    {
        return;
    }
    /* A Steam-session-only build has no master-server status line to show, so
     * the connection banner must not claim one. */
    if (!g_PcConfig.onlineEnabled)
    {
        drawStatus = 0;
    }

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0)
    {
        s_vpW = (float)vp[2];
        s_vpH = (float)vp[3];
    }
    px = (int)(s_vpH * 0.022f);
    if (px < 11) px = 11;
    if (px > 34) px = 34;

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

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* Re-asserted every draw, not just at init: a stale a_uv is the overlay
     * blotch class, and it is invisible in testing because a 1x1 background
     * texture makes every uv sample the same texel. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    /* Explicit equation as well as function: PsyCross leaves the blend
     * EQUATION set to whatever the last world split wanted, and a subtract
     * left behind there is what turns an overlay into a black rectangle. */
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (drawStatus && !drawList)
    {
        char line[160];
        snprintf(line, sizeof(line), "Online: %s", ShNet_StatusText());
        /* Only ever a real problem by the time we get here, so it is allowed
         * to sit in the corner until it is fixed. */
        Nu_DrawText(line, s_vpH * 0.03f, s_vpH * 0.03f, (int)((float)px * 0.9f),
                    0.62f, 0.55f, 0.45f, 0.9f);
    }

    Nu_DrawEvents((int)((float)px * 0.9f));

    if (ShNetMemo_JustPlaced())
    {
        Nu_DrawTextCentered("Message left.", s_vpW * 0.5f, s_vpH * 0.74f, px,
                            0.80f, 0.78f, 0.60f, 1.0f);
    }

    if (ShNetChat_DisplayActive())
    {
        Nu_DrawChat((int)((float)px * 0.82f));
    }

    if (!drawComposer)
    {
        Nu_DrawNearbyMemo(px);
    }
    if (drawList)
    {
        Nu_DrawPlayerList(px);
    }
    if (drawComposer)
    {
        Nu_DrawComposer(px);
    }

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glActiveTexture((GLenum)prevActive);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb,
                        (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend)   glDisable(GL_BLEND);
    if (prevDepth)    glEnable(GL_DEPTH_TEST);
    if (prevCull)     glEnable(GL_CULL_FACE);
    if (prevScissor)  glEnable(GL_SCISSOR_TEST);
}
