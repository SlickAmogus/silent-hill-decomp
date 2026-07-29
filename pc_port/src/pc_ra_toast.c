/*
 * pc_ra_toast.c - animated achievement-unlock toast.
 *
 * Timing is wall-clock (SDL_GetTicks), never the game clock: the popup is drawn
 * from the PsyX post-capture hook, so it must keep animating while the game is
 * paused, in a menu, or on a frozen frame.
 *
 * Everything here is self-contained -- its own GL program, VAO/VBO and textures,
 * with a full state save/restore around the draw -- so it cannot disturb the
 * shared overlay program the console/minimap/collision views rely on.
 *
 * All sizing derives from the CURRENT VIEWPORT in pixels, then converts to NDC
 * at the last step. The post-capture viewport is the presented game image (the
 * centred 4:3 sub-rect under pillarbox), so the popup stays centred on the
 * picture and correctly proportioned in every widescreen mode.
 */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "sh_log.h"
#include "pc_config.h"
#include "pc_ra_toast.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* Implementation lives in hires_override.c; we only call it (as pc_decals.c does). */
#include "stb_image.h"

/* ------------------------------------------------------------------ */
/* Phases                                                              */
/* ------------------------------------------------------------------ */

typedef enum
{
    TP_RISE = 0,   /* badge flies up from below the bottom edge */
    TP_EXPAND,     /* panel widens; badge slides left, text emerges right */
    TP_NAME1,
    TP_XF1,        /* cross-fade name -> description */
    TP_DESC,
    TP_XF2,        /* cross-fade description -> name */
    TP_NAME2,
    TP_COLLAPSE,   /* text absorbed back into the badge */
    TP_SINK,       /* badge flies back out the way it came */
    TP_COUNT
} e_ToastPhase;

static const Uint32 TP_MS[TP_COUNT] = { 420, 360, 2300, 240, 2800, 240, 1300, 360, 340 };

#define TOAST_QUEUE_MAX 4
#define TOAST_TITLE_MAX 96
#define TOAST_DESC_MAX  256
#define TOAST_BADGE_MAX 16

typedef struct
{
    char     title[TOAST_TITLE_MAX];
    char     desc[TOAST_DESC_MAX];
    char     badge[TOAST_BADGE_MAX];
    unsigned points;
} s_ToastItem;

static s_ToastItem s_queue[TOAST_QUEUE_MAX];
static int         s_queueHead, s_queueCount;

static s_ToastItem s_live;
static int         s_liveActive;
static Uint32      s_liveStart;
static int         s_soundPlayed;

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locTex, s_locColor;
static int    s_glReady;

static GLuint s_texBadge, s_texName, s_texDesc, s_texPanel;

/* Unlock events arrive on the game-logic thread, but PsyCross caches the bound
 * texture (g_lastBoundTexture) and skips redundant binds. Any bind or delete
 * issued outside the draw desyncs that cache and the whole scene keeps sampling
 * our texture for the rest of the run, so all GL work is deferred to Draw. */
#define TOAST_GARBAGE_MAX 16
static GLuint s_garbage[TOAST_GARBAGE_MAX];
static int    s_garbageCount;

static unsigned char* s_pendBadge;
static int            s_pendBadgeW, s_pendBadgeH;
static int    s_texNameW, s_texNameH, s_texDescW, s_texDescH;
static int    s_panelW, s_panelH;
static int    s_badgeReady;

static GLuint toast_make_shader(GLenum type, const char* src, const char* what)
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
        SH_DBG("[RATOAST] %s shader failed: %s", what, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void toast_gl_init(void)
{
    /* Legacy GLSL dialect, matching dbg_overlay.c -- the shipped contexts
     * accept it, and mixing dialects silently links a program that draws
     * nothing. */
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
        "uniform vec4 u_color;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(u_tex, v_uv) * u_color;\n"
        "}\n";

    GLuint vs, fs;
    GLint  ok = 0;
    GLint  initPrevVao = 0, initPrevBuf = 0;

    s_glReady = -1; /* attempted */

    vs = toast_make_shader(GL_VERTEX_SHADER, vs_src, "vertex");
    fs = toast_make_shader(GL_FRAGMENT_SHADER, fs_src, "fragment");
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
        SH_DBG("[RATOAST] program link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }

    s_locTex   = glGetUniformLocation(s_prog, "u_tex");
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    /* This runs on the first toast draw, BEFORE the draw's own state save, so
     * whatever it leaves bound would be captured as "previous" and restored on
     * top of PsyCross -- which caches its bindings and then skips rebinding,
     * drawing the rest of the game through our vertex buffer. Hand back exactly
     * what was bound on entry. */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &initPrevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &initPrevBuf);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray((GLuint)initPrevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)initPrevBuf);

    s_glReady = 1;
}

/* NDC quad; yTop > yBottom because NDC y grows upward. */
static void toast_quad(GLuint tex, float x0, float yTop, float x1, float yBot, float alpha)
{
    float v[6][4];
    if (!tex)
        return;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = 0.0f; v[0][3] = 0.0f;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = 0.0f; v[1][3] = 1.0f;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = 1.0f; v[2][3] = 0.0f;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = 1.0f; v[3][3] = 0.0f;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = 0.0f; v[4][3] = 1.0f;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = 1.0f; v[5][3] = 1.0f;

    glUniform4f(s_locColor, 1.0f, 1.0f, 1.0f, alpha);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static GLuint toast_upload_rgba(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Text is baked at exact device-pixel size and drawn 1:1, so nearest keeps
     * it crisp and avoids the bilinear ghosting the HD font pack hit. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

#define FONT_NAME 0   /* Oswald  - achievement title */
#define FONT_BODY 1   /* Barlow  - description + points */
#define FONT_COUNT 2

static stbtt_fontinfo s_font[FONT_COUNT];
static unsigned char* s_fontData[FONT_COUNT];
static int            s_fontOk[FONT_COUNT];
static int            s_fontsTried;

static unsigned char* toast_read_file(const char* path, size_t* outLen)
{
    FILE*  f = fopen(path, "rb");
    long   n;
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
    if (outLen) *outLen = (size_t)n;
    return buf;
}

static void toast_load_font(int idx, const char* const* paths, int pathCount)
{
    int i;
    for (i = 0; i < pathCount; i++)
    {
        size_t len = 0;
        unsigned char* data = toast_read_file(paths[i], &len);
        if (!data)
            continue;
        /* stbtt keeps raw pointers into this buffer, so it must live as long as
         * the font is used -- never free it. */
        if (stbtt_InitFont(&s_font[idx], data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData[idx] = data;
            s_fontOk[idx]   = 1;
            SH_DBG("[RATOAST] font %d: %s", idx, paths[i]);
            return;
        }
        free(data);
    }
    SH_DBG("[RATOAST] font %d: no usable file - text disabled", idx);
}

static void toast_fonts_init(void)
{
    static const char* namePaths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    static const char* bodyPaths[] = {
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    if (s_fontsTried)
        return;
    s_fontsTried = 1;
    toast_load_font(FONT_NAME, namePaths, (int)(sizeof(namePaths) / sizeof(namePaths[0])));
    toast_load_font(FONT_BODY, bodyPaths, (int)(sizeof(bodyPaths) / sizeof(bodyPaths[0])));
}

/* Minimal UTF-8 decode: RA titles carry curly quotes, dashes and accents. */
static int toast_utf8_next(const char** p)
{
    const unsigned char* s = (const unsigned char*)*p;
    int cp;
    if (!*s) return 0;
    if (s[0] < 0x80)                       { cp = s[0];                                   *p += 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);   *p += 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2])
                                            { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3; }
    else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
                                            { cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); *p += 4; }
    else                                    { cp = '?'; *p += 1; }
    return cp;
}

static float toast_measure(int fontIdx, const char* text, float px)
{
    const char* p = text;
    float scale = stbtt_ScaleForPixelHeight(&s_font[fontIdx], px);
    float w = 0.0f;
    int   prev = 0, cp;
    while ((cp = toast_utf8_next(&p)) != 0)
    {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&s_font[fontIdx], cp, &adv, &lsb);
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&s_font[fontIdx], prev, cp) * scale;
        w += adv * scale;
        prev = cp;
    }
    return w;
}

typedef struct
{
    const char*   text;
    int           fontIdx;
    float         px;
    unsigned char tint;   /* 255 = white, lower = dimmed */
} s_ToastLine;

/*
 * Rasterize lines into one RGBA texture: white glyphs with a black outline.
 *
 * Glyphs are MAX-blended into a coverage buffer rather than written directly --
 * stbtt_MakeCodepointBitmap* overwrites its destination rect, and kerned or
 * overhanging glyphs have overlapping boxes, so writing straight in erases part
 * of the previous glyph.
 */
static GLuint toast_bake(const s_ToastLine* lines, int count, int maxW, int* outW, int* outH)
{
    int   i, x, y, ox, oy;
    int   outline;
    float totalH = 0.0f, widest = 0.0f;
    int   W, H;
    unsigned char *cov, *dil, *rgba, *scratch;
    GLuint tex;
    float  lineTop[8];
    float  linePx[8];

    if (count > 8) count = 8;
    for (i = 0; i < count; i++)
    {
        float px = lines[i].px;
        float w;
        if (!s_fontOk[lines[i].fontIdx])
            return 0;
        w = toast_measure(lines[i].fontIdx, lines[i].text, px);
        if (maxW > 0 && w > (float)maxW && w > 1.0f)
        {
            float shrink = (float)maxW / w;
            if (shrink < 0.55f) shrink = 0.55f;
            px *= shrink;
            w = toast_measure(lines[i].fontIdx, lines[i].text, px);
        }
        linePx[i]  = px;
        lineTop[i] = totalH;
        totalH += px * 1.20f;
        if (w > widest) widest = w;
    }
    if (widest < 1.0f || totalH < 1.0f)
        return 0;
    if (maxW > 0 && widest > (float)maxW) widest = (float)maxW;

    outline = (int)(lines[0].px / 22.0f);
    if (outline < 2) outline = 2;

    W = (int)ceilf(widest) + 2 * outline + 2;
    H = (int)ceilf(totalH) + 2 * outline + 2;
    if (W <= 0 || H <= 0 || W > 4096 || H > 1024)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    dil     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !dil || !rgba || !scratch)
    {
        free(cov); free(dil); free(rgba); free(scratch);
        return 0;
    }

    for (i = 0; i < count; i++)
    {
        const char* p     = lines[i].text;
        float       px    = linePx[i];
        float       scale = stbtt_ScaleForPixelHeight(&s_font[lines[i].fontIdx], px);
        int         asc, desc, gap, cp, prev = 0;
        float       penX = (float)outline;
        float       baseY;

        stbtt_GetFontVMetrics(&s_font[lines[i].fontIdx], &asc, &desc, &gap);
        baseY = (float)outline + lineTop[i] + asc * scale;

        while ((cp = toast_utf8_next(&p)) != 0)
        {
            int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
            float shiftX;

            if (prev)
                penX += stbtt_GetCodepointKernAdvance(&s_font[lines[i].fontIdx], prev, cp) * scale;
            shiftX = penX - floorf(penX);

            stbtt_GetCodepointBitmapBoxSubpixel(&s_font[lines[i].fontIdx], cp, scale, scale,
                                                shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
            gw = gx1 - gx0;
            gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw <= W && gh <= H)
            {
                memset(scratch, 0, (size_t)gw * gh);
                stbtt_MakeCodepointBitmapSubpixel(&s_font[lines[i].fontIdx], scratch,
                                                  gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
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
                        if (v > cov[dy * W + dx])
                            cov[dy * W + dx] = v;   /* max-blend */
                    }
                }
            }
            stbtt_GetCodepointHMetrics(&s_font[lines[i].fontIdx], cp, &adv, &lsb);
            penX += adv * scale;
            prev = cp;
            if (penX > (float)(W - outline))
                break;
        }
    }

    /* Dilate coverage by the outline radius to get the black rim. */
    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            unsigned char best = 0;
            for (oy = -outline; oy <= outline; oy++)
            {
                int sy = y + oy;
                if (sy < 0 || sy >= H) continue;
                for (ox = -outline; ox <= outline; ox++)
                {
                    int sx = x + ox;
                    unsigned char v;
                    if (sx < 0 || sx >= W) continue;
                    if (ox * ox + oy * oy > outline * outline) continue;
                    v = cov[sy * W + sx];
                    if (v > best) best = v;
                }
            }
            dil[y * W + x] = best;
        }
    }

    /* Straight alpha, matching the shared GL_SRC_ALPHA blend. */
    for (i = 0; i < W * H; i++)
    {
        float aT = cov[i] / 255.0f;
        float aO = dil[i] / 255.0f;
        float a  = aT + aO * (1.0f - aT);
        unsigned char v = (a > 0.0f) ? (unsigned char)(255.0f * aT / a) : 0;
        rgba[i * 4 + 0] = v;
        rgba[i * 4 + 1] = v;
        rgba[i * 4 + 2] = v;
        rgba[i * 4 + 3] = (unsigned char)(255.0f * a);
    }

    tex = toast_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;

    free(cov); free(dil); free(rgba); free(scratch);
    return tex;
}

/* Backdrop: dark rounded panel with a rust accent along the bottom. */
static void toast_build_panel(int w, int h)
{
    unsigned char* px;
    int x, y;
    const float r = (float)h * 0.18f;

    if (s_texPanel && s_panelW == w && s_panelH == h)
        return;
    if (s_texPanel) { glDeleteTextures(1, &s_texPanel); s_texPanel = 0; }
    if (w <= 0 || h <= 0) return;

    px = (unsigned char*)calloc((size_t)w * h, 4);
    if (!px) return;

    for (y = 0; y < h; y++)
    {
        float fy = (float)y / (float)(h - 1);
        for (x = 0; x < w; x++)
        {
            float dx = 0.0f, dy = 0.0f, d, a;
            int   i = (y * w + x) * 4;
            unsigned char cr, cg, cb, ca;

            if (x < r)          dx = r - (float)x;
            else if (x > w - r) dx = (float)x - (w - r);
            if (y < r)          dy = r - (float)y;
            else if (y > h - r) dy = (float)y - (h - r);
            d = sqrtf(dx * dx + dy * dy);
            a = (d <= r) ? 1.0f : 0.0f;
            if (d > r - 1.0f && d <= r) a = r - d;      /* 1px AA on the corner */

            cr = (unsigned char)(10 - 6 * fy);
            cg = (unsigned char)(10 - 6 * fy);
            cb = (unsigned char)(12 - 6 * fy);
            ca = (unsigned char)((214 + 18 * fy) * a);

            if (y >= h - 3 && a > 0.5f)                  /* rust accent rule */
            {
                cr = 120; cg = 28; cb = 20; ca = (unsigned char)(230 * a);
            }
            px[i + 0] = cr; px[i + 1] = cg; px[i + 2] = cb; px[i + 3] = ca;
        }
    }
    s_texPanel = toast_upload_rgba(px, w, h);
    s_panelW = w; s_panelH = h;
    free(px);
}

/* ------------------------------------------------------------------ */
/* Sound                                                               */
/* ------------------------------------------------------------------ */

enum { SND_UNINIT = 0, SND_AL, SND_SDL, SND_DISABLED };
static int             s_sndState;
static ALuint          s_alBuf, s_alSrc;
static SDL_AudioDeviceID s_sdlDev;
static Uint8*          s_sdlWav;
static Uint32          s_sdlLen;

static void toast_sound_init(void)
{
    SDL_AudioSpec spec;
    Uint8* wav = NULL;
    Uint32 len = 0;

    if (s_sndState != SND_UNINIT)
        return;

    if (!SDL_LoadWAV("gamedata/sound/trophy.wav", &spec, &wav, &len))
    {
        SH_DBG("[RATOAST] trophy.wav: %s - unlock sound disabled", SDL_GetError());
        s_sndState = SND_DISABLED;
        return;
    }
    if (spec.format != AUDIO_S16LSB || spec.channels < 1 || spec.channels > 2)
    {
        SH_DBG("[RATOAST] trophy.wav must be 16-bit PCM mono/stereo - sound disabled");
        SDL_FreeWAV(wav);
        s_sndState = SND_DISABLED;
        return;
    }

    /* The SPU can be running the software renderer, in which case OpenAL was
     * never initialised and alGen* would just raise AL_INVALID_OPERATION. */
    if (alcGetCurrentContext())
    {
        alGenBuffers(1, &s_alBuf);
        alGenSources(1, &s_alSrc);
        alBufferData(s_alBuf, spec.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                     wav, (ALsizei)len, spec.freq);
        SDL_FreeWAV(wav);                       /* AL copied it */
        alSourcei(s_alSrc, AL_BUFFER, (ALint)s_alBuf);
        alSourcei(s_alSrc, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(s_alSrc, AL_POSITION, 0.0f, 0.0f, 0.0f);
        s_sndState = SND_AL;
    }
    else
    {
        SDL_AudioSpec want = spec;
        want.samples  = 1024;
        want.callback = NULL;
        if (!SDL_WasInit(SDL_INIT_AUDIO))
            SDL_InitSubSystem(SDL_INIT_AUDIO);
        s_sdlDev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (!s_sdlDev)
        {
            SH_DBG("[RATOAST] SDL audio: %s", SDL_GetError());
            SDL_FreeWAV(wav);
            s_sndState = SND_DISABLED;
            return;
        }
        s_sdlWav = wav;                          /* kept for every replay */
        s_sdlLen = len;
        SDL_PauseAudioDevice(s_sdlDev, 0);
        s_sndState = SND_SDL;
    }
}

static void toast_sound_play(void)
{
    toast_sound_init();
    if (s_sndState == SND_AL)
    {
        alSourcef(s_alSrc, AL_GAIN, 1.0f);
        alSourceStop(s_alSrc);
        alSourcePlay(s_alSrc);
    }
    else if (s_sndState == SND_SDL)
    {
        SDL_ClearQueuedAudio(s_sdlDev);
        SDL_QueueAudio(s_sdlDev, s_sdlWav, s_sdlLen);
    }
}

/* ------------------------------------------------------------------ */
/* Badge                                                               */
/* ------------------------------------------------------------------ */

int Pc_RaToast_WantsBadge(const char* badgeName)
{
    int i;
    if (!badgeName || !badgeName[0])
        return 0;
    if (s_liveActive && !s_badgeReady && strcmp(s_live.badge, badgeName) == 0)
        return 1;
    for (i = 0; i < s_queueCount; i++)
    {
        const s_ToastItem* it = &s_queue[(s_queueHead + i) % TOAST_QUEUE_MAX];
        if (strcmp(it->badge, badgeName) == 0)
            return 1;
    }
    return 0;
}

void Pc_RaToast_ProvideBadge(const char* badgeName, const unsigned char* png, size_t len)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba;

    if (!s_liveActive || s_badgeReady || s_pendBadge || !png || len == 0)
        return;
    if (!badgeName || strcmp(s_live.badge, badgeName) != 0)
        return;

    rgba = stbi_load_from_memory(png, (int)len, &w, &h, &comp, 4);
    if (!rgba)
    {
        SH_DBG("[RATOAST] badge %s: decode failed", badgeName);
        return;
    }
    if (s_pendBadge)
        stbi_image_free(s_pendBadge);
    s_pendBadge  = rgba;
    s_pendBadgeW = w;
    s_pendBadgeH = h;
    SH_DBG("[RATOAST] badge %s decoded (%dx%d), upload queued", badgeName, w, h);
}

/* ------------------------------------------------------------------ */
/* Queue                                                               */
/* ------------------------------------------------------------------ */

static void toast_retire(GLuint tex)
{
    if (tex && s_garbageCount < TOAST_GARBAGE_MAX)
        s_garbage[s_garbageCount++] = tex;
}

/* Render-thread only, and only from inside Draw's save/restore block. */
static void toast_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;

    if (s_pendBadge)
    {
        toast_retire(s_texBadge);
        s_texBadge   = toast_upload_rgba(s_pendBadge, s_pendBadgeW, s_pendBadgeH);
        s_badgeReady = 1;
        stbi_image_free(s_pendBadge);
        s_pendBadge = NULL;
    }
}

static void toast_clear_textures(void)
{
    toast_retire(s_texBadge); s_texBadge = 0;
    toast_retire(s_texName);  s_texName  = 0;
    toast_retire(s_texDesc);  s_texDesc  = 0;
    if (s_pendBadge) { stbi_image_free(s_pendBadge); s_pendBadge = NULL; }
    s_badgeReady = 0;
}

static void toast_begin(const s_ToastItem* item)
{
    toast_clear_textures();
    s_live        = *item;
    s_liveActive  = 1;
    s_liveStart   = SDL_GetTicks();
    s_soundPlayed = 0;
}

static void toast_finish(void)
{
    toast_clear_textures();
    s_liveActive = 0;
    if (s_queueCount > 0)
    {
        s_ToastItem next = s_queue[s_queueHead];
        s_queueHead = (s_queueHead + 1) % TOAST_QUEUE_MAX;
        s_queueCount--;
        toast_begin(&next);
    }
}

void Pc_RaToast_Show(const char* title, const char* desc, const char* badgeName, unsigned points)
{
    s_ToastItem item;
    memset(&item, 0, sizeof(item));
    if (title)     { strncpy(item.title, title, sizeof(item.title) - 1); }
    if (desc)      { strncpy(item.desc,  desc,  sizeof(item.desc)  - 1); }
    if (badgeName) { strncpy(item.badge, badgeName, sizeof(item.badge) - 1); }
    item.points = points;

    if (!s_liveActive)
    {
        toast_begin(&item);
        return;
    }
    if (s_queueCount < TOAST_QUEUE_MAX)
    {
        s_queue[(s_queueHead + s_queueCount) % TOAST_QUEUE_MAX] = item;
        s_queueCount++;
    }
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static float e_outCubic(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
static float e_inCubic (float t) { return t * t * t; }
static float e_smooth  (float t) { return t * t * (3.0f - 2.0f * t); }

void Pc_RaToast_Draw(void)
{
    GLint vp[4];
    float vpW, vpH;
    Uint32 age, acc = 0;
    int    ph = 0;
    float  t, eRise = 1.0f, eExpand = 1.0f, aName = 0.0f, aDesc = 0.0f;
    float  B, padX, padY, panelH, colW, textW, expW, restBottom;
    float  riseOfs, w, cx, bottom, top, left, right;
    float  badgeL, badgeB, textL, textCY;

    /* GL state we touch, saved and restored around the draw. */
    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0;
    GLint  prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO;
    GLint  prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLboolean prevBlend, prevDepth, prevCull;

    if (!s_liveActive)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    vpW = (float)vp[2];
    vpH = (float)vp[3];
    if (vp[2] <= 0 || vp[3] <= 0)
        return;

    if (!s_glReady)
        toast_gl_init();
    if (s_glReady != 1)
        return;

    /* Phase resolution on wall clock. */
    age = SDL_GetTicks() - s_liveStart;
    while (ph < TP_COUNT && age >= acc + TP_MS[ph]) { acc += TP_MS[ph]; ph++; }
    if (ph >= TP_COUNT) { toast_finish(); return; }
    t = (float)(age - acc) / (float)TP_MS[ph];

    switch (ph)
    {
    case TP_RISE:     eRise = e_outCubic(t); eExpand = 0.0f; break;
    case TP_EXPAND:   eExpand = e_smooth(t); aName = t;      break;
    case TP_NAME1:    aName = 1.0f;                          break;
    case TP_XF1:      aName = 1.0f - t; aDesc = t;           break;
    case TP_DESC:     aDesc = 1.0f;                          break;
    case TP_XF2:      aDesc = 1.0f - t; aName = t;           break;
    case TP_NAME2:    aName = 1.0f;                          break;
    case TP_COLLAPSE: eExpand = 1.0f - e_smooth(t); aName = 1.0f - t; break;
    case TP_SINK:     eRise = 1.0f - e_inCubic(t); eExpand = 0.0f;    break;
    default: break;
    }

    if (!s_soundPlayed)
    {
        s_soundPlayed = 1;
        toast_sound_play();
    }

    /* Saved BEFORE the text/panel bakes below -- those create textures, so
     * sampling the binding afterwards would capture one of ours and "restore"
     * it into PsyCross's cache. */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB,   &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB,   &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    toast_gl_pump();

    /* Layout in viewport pixels (origin = viewport bottom-left). */
    B          = 0.150f * vpH;
    padX       = 0.028f * vpH;
    padY       = 0.022f * vpH;
    panelH     = B + 2.0f * padY;
    colW       = B + 2.0f * padX;
    textW      = 3.60f * B;
    expW       = B + 3.0f * padX + textW;
    if (expW > 0.88f * vpW) { expW = 0.88f * vpW; textW = expW - B - 3.0f * padX; }
    restBottom = 0.055f * vpH;

    riseOfs = (1.0f - eRise) * (panelH + restBottom + 2.0f);
    w       = colW + (expW - colW) * eExpand;
    cx      = vpW * 0.5f;
    bottom  = restBottom - riseOfs;
    top     = bottom + panelH;
    left    = cx - w * 0.5f;
    right   = cx + w * 0.5f;

    badgeL = left + padX;
    badgeB = bottom + padY;
    textL  = left + B + 2.0f * padX;
    textCY = bottom + panelH * 0.5f;

    /* Bake text lazily once the panel starts opening. */
    if (!s_texName && s_fontsTried == 0)
        toast_fonts_init();
    if (!s_texName && (s_fontOk[FONT_NAME] || s_fontOk[FONT_BODY]))
    {
        char        pts[32];
        s_ToastLine ln[2];
        snprintf(pts, sizeof(pts), "%u points", s_live.points);
        ln[0].text = s_live.title; ln[0].fontIdx = FONT_NAME; ln[0].px = 0.062f * vpH; ln[0].tint = 255;
        ln[1].text = pts;          ln[1].fontIdx = FONT_BODY; ln[1].px = 0.026f * vpH; ln[1].tint = 190;
        s_texName = toast_bake(ln, 2, (int)textW, &s_texNameW, &s_texNameH);

        if (s_live.desc[0])
        {
            /* Wrap the description to two lines, truncating past that. */
            static char l1[TOAST_DESC_MAX], l2[TOAST_DESC_MAX];
            s_ToastLine dl[2];
            float       px = 0.034f * vpH;
            int         n  = (int)strlen(s_live.desc), cut = n, i2;
            l1[0] = l2[0] = '\0';
            if (toast_measure(FONT_BODY, s_live.desc, px) > textW)
            {
                for (i2 = 0; i2 < n; i2++)
                {
                    char save;
                    if (s_live.desc[i2] != ' ') continue;
                    save = s_live.desc[i2];
                    ((char*)s_live.desc)[i2] = '\0';
                    if (toast_measure(FONT_BODY, s_live.desc, px) <= textW) cut = i2;
                    ((char*)s_live.desc)[i2] = save;
                }
            }
            if (cut >= n) { strncpy(l1, s_live.desc, sizeof(l1) - 1); }
            else
            {
                memcpy(l1, s_live.desc, (size_t)cut); l1[cut] = '\0';
                strncpy(l2, s_live.desc + cut + 1, sizeof(l2) - 1);
            }
            dl[0].text = l1; dl[0].fontIdx = FONT_BODY; dl[0].px = px; dl[0].tint = 255;
            dl[1].text = l2; dl[1].fontIdx = FONT_BODY; dl[1].px = px; dl[1].tint = 255;
            s_texDesc = toast_bake(dl, l2[0] ? 2 : 1, (int)textW, &s_texDescW, &s_texDescH);
        }
    }

    toast_build_panel((int)colW, (int)panelH);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glUniform1i(s_locTex, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

#define NX(px_) (-1.0f + 2.0f * (px_) / vpW)
#define NY(py_) (-1.0f + 2.0f * (py_) / vpH)

    /* Backdrop, stretched to the current width. */
    toast_quad(s_texPanel, NX(left), NY(top), NX(right), NY(bottom), 1.0f);

    /* Text, clipped to the opening panel by fading with the expansion. */
    if (s_texName && aName > 0.01f)
    {
        float tw = (float)s_texNameW, th = (float)s_texNameH;
        float ty = textCY + th * 0.5f;
        toast_quad(s_texName, NX(textL), NY(ty), NX(textL + tw), NY(ty - th), aName * eExpand);
    }
    if (s_texDesc && aDesc > 0.01f)
    {
        float tw = (float)s_texDescW, th = (float)s_texDescH;
        float ty = textCY + th * 0.5f;
        toast_quad(s_texDesc, NX(textL), NY(ty), NX(textL + tw), NY(ty - th), aDesc * eExpand);
    }

    /* Badge last so it sits over the text as the panel collapses. */
    if (s_texBadge)
        toast_quad(s_texBadge, NX(badgeL), NY(badgeB + B), NX(badgeL + B), NY(badgeB), 1.0f);

#undef NX
#undef NY

    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glUseProgram((GLuint)prevProg);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb,
                        (GLenum)prevSrcA,   (GLenum)prevDstA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
}

/* Reads the fonts and the trophy WAV off disk. MUST be driven from the game
 * loop, never from Pc_RaToast_Draw: the draw runs inside the GL hook, and
 * pulling files through there mid-frame races the Fs queue the same way the
 * minimap's paper-map TIM does. Doing it lazily at unlock time landed the reads
 * on top of the post-cutscene map transition and wedged the load. */
void Pc_RaToast_Preload(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    toast_fonts_init();
    toast_sound_init();
}
