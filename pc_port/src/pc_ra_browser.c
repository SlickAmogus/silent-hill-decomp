/*
 * pc_ra_browser.c - main-menu achievement browser.
 *
 * A tall panel over the title screen listing the whole set: unlocked first,
 * then locked by ascending point value. Rows are badge-left, name over
 * description. Drag with the mouse, spin the wheel, or hold the arrow keys to
 * scroll; any other button closes.
 *
 * Structurally a sibling of pc_ra_toast.c and it repeats that file's hard-won
 * constraints deliberately rather than sharing code with it:
 *   - its OWN GL program / VAO / textures, with full state save+restore. The
 *     shared dbg_overlay program belongs to the console, collision and minimap
 *     overlays; clobbering its uniforms makes those invisible.
 *   - LEGACY GLSL (attribute / varying / gl_FragColor, no #version). Mixing
 *     dialects links a program that silently draws nothing.
 *   - all GL work deferred to Draw. PsyCross caches its bound texture and skips
 *     redundant binds, so a bind or delete issued from the game thread desyncs
 *     that cache for the rest of the run.
 *   - wall-clock timing, because the draw hook runs post-capture.
 *
 * Text is stb_truetype, baked per row at device-pixel size. The toast owns the
 * only stb_truetype implementation in the build, so this file uses the header
 * as a consumer.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <PsyX/common/glad.h>

#include "stb_truetype.h"

#include "pc_ra_browser.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"
#include "pc_ui_sound.h"
#include "sh_log.h"

/* stb_image is vendored for the texture-pack loader; the badge PNGs come
 * through the same decoder the hires override uses. */
extern int HiresOverride_DecodeToRGBA(const unsigned char* bytes, unsigned int len,
                                      unsigned char** outRgba, int* outW, int* outH);

#define RAB_MAX_ACH   512
#define RAB_GARBAGE   64

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    PcRaAch  ach;
    GLuint   texName;    /* baked title */
    GLuint   texDesc;    /* baked description */
    GLuint   texPts;     /* baked "10 pts" */
    int      nameW, nameH;
    int      descW, descH;
    int      ptsW, ptsH;
    int      bakedAtRowH; /* row height the text was baked for; re-bake on resize */
} s_RabRow;

static s_RabRow s_rows[RAB_MAX_ACH];
static int      s_rowCount;

/* Open/close are ANIMATED, so "is the panel up" is a phase, not a flag. The
 * menu keeps yielding input for the whole of CLOSING, or the pad would drive
 * the title screen while the panel is still sliding off. */
enum { RAB_CLOSED = 0, RAB_OPENING, RAB_SHOWN, RAB_CLOSING };
static int    s_phase;
static Uint32 s_phaseStart;
#define RAB_OPEN_MS  360u
#define RAB_CLOSE_MS 300u

/* Badge art, cached for the whole session and keyed by name.
 *
 * This used to hang off the row and be freed whenever the list was rebuilt,
 * while the request de-dup was permanent -- so the second time the panel opened
 * nothing was re-requested and every row stayed blank. Requests are also issued
 * ONCE, for the entire set, at the first open: asking per visible row meant a
 * scroll re-asked continuously and overran the arrival queue, which is why rows
 * went blank progressively from the top as they scrolled. */
typedef struct
{
    char   name[16];
    GLuint tex;
} s_RabBadge;

static s_RabBadge s_badges[RAB_MAX_ACH];
static int        s_badgeCount;
static int        s_badgesRequested;

/* One clip for both halves: the close is the same swish, quieter. */
static PcUiSound* s_sndSwish;
static int        s_soundsTried;
#define RAB_CLOSE_GAIN 0.75f

/* Scroll position in pixels, and where the content ends. */
static float s_scroll;
static float s_scrollMax;
static float s_velocity;      /* px/sec, for release-flick and key repeat */

/* Viewport height in pixels, published by Draw so the drag can turn a
 * normalized pointer delta into the same pixels the layout uses. The default
 * covers the first frame, before Draw has ever run. */
static float s_viewH = 1080.0f;
static float s_viewW = 1920.0f;

/* Drag tracking. s_dragMoved separates a click from a drag: releasing after a
 * scroll must not also open whatever row happened to be under the cursor. */
static int   s_dragging;
static float s_dragLastY;
static float s_dragMoved;

/* Scrollbar thumb drag: grabbing the bar maps pointer travel onto the whole
 * scroll range, unlike a content drag which is 1:1. */
static int   s_barDragging;
static float s_barGrabY;
static float s_barGrabScroll;

/* Row under the pointer, and the row opened for detail (-1 = none/list view). */
static int   s_hoverRow = -1;
static int   s_detailRow = -1;

/* Geometry published by Draw for Update to hit-test against: the two run in
 * different places (game thread vs post-capture hook) and only Draw knows the
 * viewport. All in viewport pixels, y up. */
static float s_geoListT, s_geoListB, s_geoRowPitch, s_geoRowH;
static float s_geoPanelL, s_geoPanelR;
static float s_geoBarL, s_geoBarR;

/* Close is edge-triggered on a RELEASE-then-PRESS so the Map button that opened
 * the panel cannot also dismiss it on the same press. */
static int   s_armClose;

static Uint32 s_lastTick;

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locTex, s_locColor;
static int    s_glReady;
static GLuint s_texPanel, s_texWhite, s_texCursor;
static int    s_panelW, s_panelH;
static int    s_cursorW, s_cursorH;

static GLuint s_garbage[RAB_GARBAGE];
static int    s_garbageCount;

/* Badge PNGs land on the game thread but may only be decoded/uploaded inside
 * Draw, so they queue here as raw bytes. */
typedef struct
{
    char           badge[16];
    unsigned char* png;
    size_t         len;
} s_RabPendBadge;

/* Sized for a whole set arriving at once: every badge is requested up front, so
 * a 16-slot queue silently dropped ~50 of them and those rows -- never
 * re-requested -- stayed blank for the session. */
#define RAB_PEND_MAX 512
static s_RabPendBadge s_pend[RAB_PEND_MAX];
static int            s_pendCount;

static GLuint rab_make_shader(GLenum type, const char* src, const char* what)
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
        SH_DBG("[RABROWSE] %s shader failed: %s", what, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void rab_gl_init(void)
{
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

    s_glReady = -1;

    vs = rab_make_shader(GL_VERTEX_SHADER, vs_src, "vertex");
    fs = rab_make_shader(GL_FRAGMENT_SHADER, fs_src, "fragment");
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
        SH_DBG("[RABROWSE] program link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }

    s_locTex   = glGetUniformLocation(s_prog, "u_tex");
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    /* Runs before the draw's own state save, so hand back exactly what was
     * bound on entry — otherwise our buffer is captured as "previous" and
     * restored on top of PsyCross, which then draws the game through it. */
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

static GLuint rab_upload_rgba(const unsigned char* rgba, int w, int h)
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

static void rab_retire(GLuint tex)
{
    if (!tex)
        return;
    if (s_garbageCount < RAB_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
}

static void rab_gl_pump(void)
{
    int i;
    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;
}

/* Sub-rect quad in NDC. yTop > yBot because NDC y grows upward. `uv*` select a
 * window of the source texture, which is how a row's text is clipped against
 * the panel edges without a scissor. */
static void rab_quad_uv(GLuint tex, float x0, float yTop, float x1, float yBot,
                        float u0, float v0, float u1, float v1,
                        float r, float g, float b, float a)
{
    float v[6][4];
    if (!tex)
        return;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = u0; v[0][3] = v0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = u0; v[1][3] = v1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = u1; v[2][3] = v0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = u1; v[3][3] = v0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = u0; v[4][3] = v1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = u1; v[5][3] = v1;

    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void rab_quad(GLuint tex, float x0, float yTop, float x1, float yBot, float a)
{
    rab_quad_uv(tex, x0, yTop, x1, yBot, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, a);
}

/* ------------------------------------------------------------------ */
/* Fonts                                                               */
/* ------------------------------------------------------------------ */

#define RAB_FONT_NAME 0
#define RAB_FONT_BODY 1
#define RAB_FONT_COUNT 2

static stbtt_fontinfo s_font[RAB_FONT_COUNT];
static unsigned char* s_fontData[RAB_FONT_COUNT];
static int            s_fontOk[RAB_FONT_COUNT];
static int            s_fontsTried;

static unsigned char* rab_read_file(const char* path, size_t* outLen)
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
    if (outLen) *outLen = (size_t)n;
    return buf;
}

static void rab_load_font(int idx, const char* const* paths, int pathCount)
{
    int i;
    for (i = 0; i < pathCount; i++)
    {
        size_t len = 0;
        unsigned char* data = rab_read_file(paths[i], &len);
        if (!data)
            continue;
        /* stbtt keeps raw pointers into this buffer — never free it. */
        if (stbtt_InitFont(&s_font[idx], data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData[idx] = data;
            s_fontOk[idx]   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[RABROWSE] font %d: no usable file - text disabled", idx);
}

static void rab_fonts_init(void)
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
    rab_load_font(RAB_FONT_NAME, namePaths, (int)(sizeof(namePaths) / sizeof(namePaths[0])));
    rab_load_font(RAB_FONT_BODY, bodyPaths, (int)(sizeof(bodyPaths) / sizeof(bodyPaths[0])));
}

static int rab_utf8_next(const char** p)
{
    const unsigned char* s = (const unsigned char*)*p;
    int cp;
    if (!*s) return 0;
    if (s[0] < 0x80)                        { cp = s[0];                                  *p += 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);   *p += 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2])
                                            { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3; }
    else                                    { cp = '?';                                   *p += 1; }
    return cp;
}

/*
 * Rasterize one line: white glyphs, no outline (rows sit on an opaque panel, so
 * the toast's dilated rim would only muddy text this small).
 *
 * Glyphs are MAX-blended into a coverage buffer — stbtt_MakeCodepointBitmap*
 * OVERWRITES its destination rect, so kerned or overhanging glyphs erase part
 * of the neighbour when written straight in.
 */
static GLuint rab_bake_line(int fontIdx, const char* text, float px, int maxW,
                            int* outW, int* outH)
{
    const char* p;
    float  scale, penX, baseY;
    int    asc, desc, gap, cp, prev = 0;
    int    W, H, i, pad = 1;
    unsigned char *cov, *rgba, *scratch;
    GLuint tex;

    if (!s_fontOk[fontIdx] || !text || !text[0] || px < 4.0f)
        return 0;

    scale = stbtt_ScaleForPixelHeight(&s_font[fontIdx], px);
    stbtt_GetFontVMetrics(&s_font[fontIdx], &asc, &desc, &gap);

    W = maxW + 2 * pad;
    H = (int)ceilf(px * 1.35f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    p     = text;
    while ((cp = rab_utf8_next(&p)) != 0)
    {
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;

        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font[fontIdx], prev, cp) * scale;
        shiftX = penX - floorf(penX);

        stbtt_GetCodepointBitmapBoxSubpixel(&s_font[fontIdx], cp, scale, scale,
                                            shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font[fontIdx], scratch,
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
                        cov[dy * W + dx] = v;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font[fontIdx], cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
        if (penX > (float)(W - pad))
            break;   /* hard clip; the row is only so wide */
    }

    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }

    tex = rab_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

/* Word-wrap `text` to `maxW` and rasterize the lot into one texture. Built on
 * the same coverage/max-blend pass as rab_bake_line; the only new part is
 * choosing the break points, which is done by measuring candidate substrings
 * rather than guessing an average glyph width. */
static GLuint rab_bake_wrapped(int fontIdx, const char* text, float px, int maxW,
                               int maxLines, int* outW, int* outH)
{
    char  lines[12][256];
    int   lineCount = 0;
    const char* p = text;
    char  cur[256];
    int   curLen = 0;
    GLuint tex;
    int   i, W, H, pad = 1;
    unsigned char *cov, *rgba, *scratch;
    float scale;
    int   asc, desc, gap;

    if (!s_fontOk[fontIdx] || !text || !text[0] || px < 4.0f || maxW < 16)
        return 0;
    if (maxLines > 12) maxLines = 12;

    scale = stbtt_ScaleForPixelHeight(&s_font[fontIdx], px);
    stbtt_GetFontVMetrics(&s_font[fontIdx], &asc, &desc, &gap);

    cur[0] = ' ';
    while (*p && lineCount < maxLines)
    {
        const char* wordStart = p;
        char        trial[256];
        int         wordLen;
        float       w = 0.0f;
        const char* q;
        int         prev = 0, cp;

        while (*p && *p != ' ') p++;
        wordLen = (int)(p - wordStart);
        while (*p == ' ') p++;
        if (wordLen <= 0 || wordLen >= (int)sizeof(trial))
            continue;

        /* Candidate = current line + this word. */
        if (curLen + (curLen ? 1 : 0) + wordLen >= (int)sizeof(cur))
            break;
        memcpy(trial, cur, (size_t)curLen);
        if (curLen) trial[curLen] = ' ';
        memcpy(trial + curLen + (curLen ? 1 : 0), wordStart, (size_t)wordLen);
        trial[curLen + (curLen ? 1 : 0) + wordLen] = ' ';

        for (q = trial; (cp = rab_utf8_next(&q)) != 0; )
        {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&s_font[fontIdx], cp, &adv, &lsb);
            if (prev) w += stbtt_GetCodepointKernAdvance(&s_font[fontIdx], prev, cp) * scale;
            w += adv * scale;
            prev = cp;
        }

        if (w > (float)maxW && curLen > 0)
        {
            memcpy(lines[lineCount++], cur, (size_t)curLen + 1);
            curLen = wordLen;
            memcpy(cur, wordStart, (size_t)wordLen);
            cur[curLen] = ' ';
        }
        else
        {
            curLen = (int)strlen(trial);
            memcpy(cur, trial, (size_t)curLen + 1);
        }
    }
    if (curLen > 0 && lineCount < maxLines)
        memcpy(lines[lineCount++], cur, (size_t)curLen + 1);
    if (lineCount == 0)
        return 0;

    W = maxW + 2 * pad;
    H = (int)ceilf(px * 1.32f) * lineCount + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 2048)
        return 0;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    for (i = 0; i < lineCount; i++)
    {
        const char* t = lines[i];
        float penX = (float)pad;
        float baseY = (float)pad + (float)i * ceilf(px * 1.32f) + asc * scale;
        int   prev = 0, cp;

        while ((cp = rab_utf8_next(&t)) != 0)
        {
            int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
            float shiftX;

            if (prev)
                penX += stbtt_GetCodepointKernAdvance(&s_font[fontIdx], prev, cp) * scale;
            shiftX = penX - floorf(penX);
            stbtt_GetCodepointBitmapBoxSubpixel(&s_font[fontIdx], cp, scale, scale,
                                                shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
            gw = gx1 - gx0; gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw <= W && gh <= H)
            {
                memset(scratch, 0, (size_t)gw * gh);
                stbtt_MakeCodepointBitmapSubpixel(&s_font[fontIdx], scratch, gw, gh, gw,
                                                  scale, scale, shiftX, 0.0f, cp);
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
            stbtt_GetCodepointHMetrics(&s_font[fontIdx], cp, &adv, &lsb);
            penX += adv * scale;
            prev = cp;
        }
    }

    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    tex = rab_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

/* Flat panel: near-black, slightly lighter down the page, with a rust rule top
 * and bottom. Opaque, per the request — the title art must not read through. */
static void rab_build_panel(int w, int h)
{
    unsigned char* px;
    int x, y;

    if (s_texPanel && s_panelW == w && s_panelH == h)
        return;
    if (s_texPanel) { rab_retire(s_texPanel); s_texPanel = 0; }
    if (w <= 0 || h <= 0) return;

    px = (unsigned char*)calloc((size_t)w * h, 4);
    if (!px) return;

    for (y = 0; y < h; y++)
    {
        float fy = (float)y / (float)(h - 1);
        for (x = 0; x < w; x++)
        {
            int i = (y * w + x) * 4;
            unsigned char cr = (unsigned char)(14 + 10 * fy);
            unsigned char cg = (unsigned char)(13 + 9 * fy);
            unsigned char cb = (unsigned char)(15 + 10 * fy);

            if (y < 3 || y >= h - 3 || x < 2 || x >= w - 2)
            {
                cr = 120; cg = 28; cb = 20;
            }
            px[i + 0] = cr; px[i + 1] = cg; px[i + 2] = cb; px[i + 3] = 255;
        }
    }
    s_texPanel = rab_upload_rgba(px, w, h);
    s_panelW = w; s_panelH = h;
    free(px);
}

/* Classic arrow, built once. The game's own cursor is drawn by the menu into
 * the captured frame, so the panel — which draws after the capture — covers it.
 * Drawing our own on top is the only way it stays visible over the panel. */
static void rab_build_cursor(void)
{
    enum { CW = 12, CH = 19 };
    /* 0 = transparent, 1 = white fill, 2 = black edge. */
    static const char art[CH][CW + 1] = {
        "2...........",
        "22..........",
        "212.........",
        "2112........",
        "21112.......",
        "211112......",
        "2111112.....",
        "21111112....",
        "211111112...",
        "2111111112..",
        "21111111112.",
        "211111222222",
        "21112112....",
        "2112.2112...",
        "212..2112...",
        "22....2112..",
        "2......2112.",
        "........212.",
        ".........22.",
    };
    unsigned char px[CH][CW][4];
    int x, y;

    if (s_texCursor)
        return;
    for (y = 0; y < CH; y++)
    {
        for (x = 0; x < CW; x++)
        {
            char c = art[y][x];
            unsigned char v = (c == '1') ? 255 : 0;
            px[y][x][0] = v; px[y][x][1] = v; px[y][x][2] = v;
            px[y][x][3] = (c == '.') ? 0 : 255;
        }
    }
    s_texCursor = rab_upload_rgba(&px[0][0][0], CW, CH);
    s_cursorW = CW; s_cursorH = CH;
}

static void rab_build_white(void)
{
    unsigned char one[4] = { 255, 255, 255, 255 };
    if (s_texWhite)
        return;
    s_texWhite = rab_upload_rgba(one, 1, 1);
}

/* ------------------------------------------------------------------ */
/* List                                                                */
/* ------------------------------------------------------------------ */

/* Unlocked first, then locked; within each group by ascending points, then by
 * title so the order is stable between openings. */
static int rab_cmp(const void* a, const void* b)
{
    const s_RabRow* x = (const s_RabRow*)a;
    const s_RabRow* y = (const s_RabRow*)b;

    if (x->ach.unlocked != y->ach.unlocked)
        return y->ach.unlocked - x->ach.unlocked;
    if (x->ach.points != y->ach.points)
        return (x->ach.points < y->ach.points) ? -1 : 1;
    return strcmp(x->ach.title, y->ach.title);
}

static void rab_free_rows(void)
{
    int i;
    for (i = 0; i < s_rowCount; i++)
    {
        /* Badge textures live in the session cache and are deliberately NOT
         * retired here — they outlive any one opening of the panel. */
        rab_retire(s_rows[i].texName);
        rab_retire(s_rows[i].texDesc);
        rab_retire(s_rows[i].texPts);
    }
    memset(s_rows, 0, sizeof(s_rows));
    s_rowCount = 0;
}

static void rab_build_list(void)
{
    PcRaAch* snap;
    int      n, i;

    rab_free_rows();

    snap = (PcRaAch*)calloc(RAB_MAX_ACH, sizeof(*snap));
    if (!snap)
        return;
    n = Pc_Ra_SnapshotAchievements(snap, RAB_MAX_ACH);
    for (i = 0; i < n; i++)
        s_rows[i].ach = snap[i];
    s_rowCount = n;
    free(snap);

    qsort(s_rows, (size_t)s_rowCount, sizeof(s_rows[0]), rab_cmp);
    SH_DBG("[RABROWSE] opened with %d achievements", s_rowCount);
}

/* ------------------------------------------------------------------ */
/* Badges                                                              */
/* ------------------------------------------------------------------ */

/* Session badge cache. Entries are created when the set is first snapshotted
 * and never removed, so reopening the panel reuses the art already downloaded. */
static s_RabBadge* rab_badge_find(const char* name)
{
    int i;
    if (!name || !name[0])
        return NULL;
    for (i = 0; i < s_badgeCount; i++)
    {
        if (strcmp(s_badges[i].name, name) == 0)
            return &s_badges[i];
    }
    return NULL;
}

static s_RabBadge* rab_badge_intern(const char* name)
{
    s_RabBadge* e = rab_badge_find(name);
    if (e || !name || !name[0] || s_badgeCount >= RAB_MAX_ACH)
        return e;
    e = &s_badges[s_badgeCount++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    return e;
}

void Pc_RaBrowser_ProvideBadge(const char* badgeName, const unsigned char* png, size_t len)
{
    s_RabBadge* e;
    int i;

    if (!badgeName || !badgeName[0] || !png || !len || s_pendCount >= RAB_PEND_MAX)
        return;
    /* Only keep bytes for a badge this panel knows about and has not decoded. */
    e = rab_badge_find(badgeName);
    if (!e || e->tex != 0)
        return;
    for (i = 0; i < s_pendCount; i++)
    {
        if (strcmp(s_pend[i].badge, badgeName) == 0)
            return;   /* already queued */
    }

    s_pend[s_pendCount].png = (unsigned char*)malloc(len);
    if (!s_pend[s_pendCount].png)
        return;
    memcpy(s_pend[s_pendCount].png, png, len);
    s_pend[s_pendCount].len = len;
    strncpy(s_pend[s_pendCount].badge, badgeName, sizeof(s_pend[0].badge) - 1);
    s_pend[s_pendCount].badge[sizeof(s_pend[0].badge) - 1] = '\0';
    s_pendCount++;
}

/* Decode + upload whatever arrived. Draw-time only: see the GL note up top. */
static void rab_drain_badges(void)
{
    int p, i;

    for (p = 0; p < s_pendCount; p++)
    {
        unsigned char* rgba = NULL;
        int w = 0, h = 0;
        GLuint tex = 0;

        if (HiresOverride_DecodeToRGBA(s_pend[p].png, (unsigned int)s_pend[p].len,
                                       &rgba, &w, &h) == 0 && rgba && w > 0 && h > 0)
        {
            tex = rab_upload_rgba(rgba, w, h);
        }
        free(rgba);
        free(s_pend[p].png);

        if (tex)
        {
            /* One texture per badge NAME, shared by every row that uses it —
             * a per-row copy would be deleted more than once. */
            s_RabBadge* e = rab_badge_find(s_pend[p].badge);
            if (e && e->tex == 0)
                e->tex = tex;
            else
                rab_retire(tex);
        }
    }
    s_pendCount = 0;
}

/* ------------------------------------------------------------------ */
/* Open / close / input                                                */
/* ------------------------------------------------------------------ */

static void rab_sounds_init(void)
{
    if (s_soundsTried)
        return;
    s_soundsTried = 1;
    s_sndSwish = PcUiSound_Load("gamedata/sound/swish.wav");
}

void Pc_RaBrowser_Open(void)
{
    int i;

    if (s_phase == RAB_OPENING || s_phase == RAB_SHOWN)
        return;

    rab_build_list();
    if (s_rowCount == 0)
    {
        SH_DBG("[RABROWSE] no achievement set loaded - not opening");
        return;
    }

    /* Request every badge ONCE per session, here rather than per visible row.
     * Per-row requesting re-asked on every frame of a scroll, which overran the
     * arrival queue and left rows permanently blank. */
    for (i = 0; i < s_rowCount; i++)
        rab_badge_intern(s_rows[i].ach.badge);
    if (!s_badgesRequested)
    {
        s_badgesRequested = 1;
        for (i = 0; i < s_badgeCount; i++)
            Pc_Ra_RequestBadge(s_badges[i].name);
        SH_DBG("[RABROWSE] requested %d badge images", s_badgeCount);
    }

    rab_sounds_init();
    PcUiSound_Play(s_sndSwish);

    s_phase      = RAB_OPENING;
    s_phaseStart = SDL_GetTicks();
    s_scroll     = 0.0f;
    s_velocity   = 0.0f;
    s_dragging   = 0;
    s_armClose   = 0;
    s_lastTick   = SDL_GetTicks();
}

int Pc_RaBrowser_IsOpen(void)
{
    /* True through CLOSING as well: the panel is still on screen and the menu
     * underneath must keep yielding input until it has slid away. */
    return s_phase != RAB_CLOSED;
}

static void rab_begin_close(void)
{
    int i;

    if (s_phase == RAB_CLOSING || s_phase == RAB_CLOSED)
        return;

    rab_sounds_init();
    PcUiSound_PlayGain(s_sndSwish, RAB_CLOSE_GAIN);

    s_phase      = RAB_CLOSING;
    s_phaseStart = SDL_GetTicks();
    s_dragging   = 0;
    s_velocity   = 0.0f;

    /* Badge bytes are drained by Draw. Draw keeps running through CLOSING, so
     * anything still queued is fine; only a hard close needs the sweep. */
    (void)i;
}

static void rab_finish_close(void)
{
    int i;

    s_phase    = RAB_CLOSED;
    s_dragging = 0;
    for (i = 0; i < s_pendCount; i++)
        free(s_pend[i].png);
    s_pendCount = 0;
    /* Rows and cached badge art are kept: reopening is common, and the art
     * must never be re-downloaded. */
}
/* Input signals are resolved by the caller against the player's own bindings
 * (see the header), so this TU needs no game headers. */
void Pc_RaBrowser_Update(int closeRequested, int up, int down)
{
    const Uint8* keys;
    Uint32 now;
    float  dt;
    float  fx, fy;

    if (s_phase == RAB_CLOSED)
        return;

    now = SDL_GetTicks();
    dt  = (float)(now - s_lastTick) / 1000.0f;
    s_lastTick = now;
    if (dt <= 0.0f)  dt = 1.0f / 60.0f;
    if (dt > 0.25f)  dt = 0.25f;   /* a load hitch must not fling the list */

    if (s_phase == RAB_CLOSING)
    {
        if (now - s_phaseStart >= RAB_CLOSE_MS)
            rab_finish_close();
        return;   /* no input while it slides away */
    }
    if (s_phase == RAB_OPENING && now - s_phaseStart >= RAB_OPEN_MS)
        s_phase = RAB_SHOWN;

    keys = SDL_GetKeyboardState(NULL);

    /* Scroll: the game's own movement bindings, plus the raw arrows/PgUp/PgDn
     * so a player who rebound the pad still has something obvious that works.
     * Suppressed while a detail card is open — the list behind it must not move
     * out from under the card. */
    if (s_detailRow < 0)
    {
        const float KEY_SPEED = 620.0f;
        int kUp   = up   || (keys && keys[SDL_SCANCODE_UP]);
        int kDown = down || (keys && keys[SDL_SCANCODE_DOWN]);

        if (kUp)   s_scroll -= KEY_SPEED * dt;
        if (kDown) s_scroll += KEY_SPEED * dt;
        if (keys)
        {
            if (keys[SDL_SCANCODE_PAGEUP])   s_scroll -= KEY_SPEED * 3.0f * dt;
            if (keys[SDL_SCANCODE_PAGEDOWN]) s_scroll += KEY_SPEED * 3.0f * dt;
            if (keys[SDL_SCANCODE_HOME]) s_scroll = 0.0f;
            if (keys[SDL_SCANCODE_END])  s_scroll = s_scrollMax;
        }
    }

    /* Wheel: one notch ~= a third of a page. */
    if (s_detailRow < 0)
    {
        int step = Pc_MouseCursor_WheelStep();
        if (step)
        {
            s_scroll  -= (float)step * 140.0f;
            s_velocity = 0.0f;
        }
    }

    /* Pointer: hover, content drag, scrollbar drag, and the click that opens a
     * row's detail card.
     *
     * Content drag is PC-direction, NOT phone-direction: dragging DOWN moves the
     * view DOWN. The first cut inverted it (grab-the-paper style), which reads
     * wrong next to a scrollbar and against the wheel. */
    if (Pc_MouseCursor_ViewportPos(&fx, &fy))
    {
        float px = fx * s_viewW;
        float py = s_viewH - fy * s_viewH;   /* viewport pixels, y up */
        int   overBar = (px >= s_geoBarL && px <= s_geoBarR &&
                         py <= s_geoListT && py >= s_geoListB);

        /* Hover only inside the list, and never mid-drag. */
        s_hoverRow = -1;
        if (s_detailRow < 0 && !s_dragging && !s_barDragging &&
            px >= s_geoPanelL && px <= s_geoPanelR &&
            py <= s_geoListT && py >= s_geoListB && s_geoRowPitch > 0.0f)
        {
            float within = (s_geoListT - py) + s_scroll;
            int   idx    = (int)(within / s_geoRowPitch);
            /* The gap between rows is not part of either. */
            if (idx >= 0 && idx < s_rowCount &&
                (within - (float)idx * s_geoRowPitch) <= s_geoRowH)
                s_hoverRow = idx;
        }

        if (Pc_MouseCursor_LeftHeld() && s_detailRow < 0)
        {
            if (!s_dragging && !s_barDragging)
            {
                if (overBar && s_scrollMax > 1.0f)
                {
                    s_barDragging   = 1;
                    s_barGrabY      = py;
                    s_barGrabScroll = s_scroll;
                }
                else
                {
                    s_dragging  = 1;
                    s_dragLastY = py;
                    s_dragMoved = 0.0f;
                }
                s_velocity = 0.0f;
            }
            else if (s_barDragging)
            {
                /* Thumb travel covers (list height - thumb height) of screen for
                 * the whole scroll range, so the content moves proportionally
                 * further than the pointer. */
                float listH  = s_geoListT - s_geoListB;
                float frac   = listH / (listH + s_scrollMax);
                float thumbH = listH * frac;
                float travel = listH - thumbH;
                if (travel > 1.0f)
                    s_scroll = s_barGrabScroll + (s_barGrabY - py) * (s_scrollMax / travel);
            }
            else
            {
                float delta = py - s_dragLastY;
                if (delta != 0.0f)
                {
                    s_dragMoved += (delta < 0.0f) ? -delta : delta;
                    s_scroll   += delta;          /* down drags down */
                    s_velocity  = delta / dt;
                }
                s_dragLastY = py;
            }
        }
        else
        {
            /* Release. A press that never travelled is a click: open the row it
             * landed on, or dismiss an open detail card. */
            if ((s_dragging && s_dragMoved < 4.0f) || (!s_dragging && !s_barDragging))
            {
                if (Pc_MouseCursor_LeftClicked())
                {
                    if (s_detailRow >= 0)
                        s_detailRow = -1;
                    else if (s_hoverRow >= 0)
                        s_detailRow = s_hoverRow;
                }
            }
            if (s_dragging && s_dragMoved < 4.0f)
                s_velocity = 0.0f;   /* a tap must not flick */
            s_dragging    = 0;
            s_barDragging = 0;
        }
    }
    else
    {
        s_dragging    = 0;
        s_barDragging = 0;
        s_hoverRow    = -1;
    }

    /* Flick decay. Frozen behind an open card for the same reason as the wheel. */
    if (s_detailRow >= 0)
        s_velocity = 0.0f;
    else if (!s_dragging && (s_velocity > 1.0f || s_velocity < -1.0f))
    {
        s_scroll   += s_velocity * dt;
        s_velocity -= s_velocity * 6.0f * dt;
    }
    else if (!s_dragging)
    {
        s_velocity = 0.0f;
    }

    if (s_scroll < 0.0f)         s_scroll = 0.0f;
    if (s_scroll > s_scrollMax)  s_scroll = s_scrollMax;

    /* Close is DELIBERATELY narrow: Cancel or Map on the pad (resolved by the
     * caller), Escape, or a right-click. "Any button" dismissed the panel the
     * moment a player touched anything -- including the keys they were trying
     * to scroll with. s_armClose still gates the first frames, because the Map
     * press that opened the panel is normally still held. */
    if (!s_armClose)
    {
        if (!closeRequested && !(keys && keys[SDL_SCANCODE_ESCAPE]))
            s_armClose = 1;
        return;
    }
    if (closeRequested || (keys && keys[SDL_SCANCODE_ESCAPE]) ||
        Pc_MouseCursor_RightClicked())
    {
        /* Back out one level at a time: a detail card closes to the list, and
         * only then does the panel itself close. */
        if (s_detailRow >= 0)
            s_detailRow = -1;
        else
            rab_begin_close();
    }
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

/* Bake this row's three text textures at the current row height. Rows re-bake
 * when the window is resized, which is why the height they were made for is
 * remembered. */
static void rab_bake_row(s_RabRow* r, int rowH, int textW)
{
    char pts[32];
    float namePx = (float)rowH * 0.34f;
    float descPx = (float)rowH * 0.26f;

    if (r->bakedAtRowH == rowH && r->texName)
        return;

    rab_retire(r->texName);
    rab_retire(r->texDesc);
    rab_retire(r->texPts);
    r->texName = r->texDesc = r->texPts = 0;

    r->texName = rab_bake_line(RAB_FONT_NAME, r->ach.title, namePx, textW,
                               &r->nameW, &r->nameH);
    r->texDesc = rab_bake_line(RAB_FONT_BODY, r->ach.desc, descPx, textW,
                               &r->descW, &r->descH);
    snprintf(pts, sizeof(pts), "%u", r->ach.points);
    r->texPts = rab_bake_line(RAB_FONT_NAME, pts, namePx * 0.9f, rowH,
                              &r->ptsW, &r->ptsH);
    r->bakedAtRowH = rowH;
}

void Pc_RaBrowser_Draw(void)
{
    GLint vp[4];
    float vpW, vpH;
    float panelW, panelH, panelL, panelR, panelT, panelB;
    float slide = 0.0f, dim = 1.0f;
    float pad, headH, listT, listB, listH, rowH, rowGap, badgeS, textL, textW;
    int   i, first, last;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0;
    GLint  prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO;
    GLint  prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_phase == RAB_CLOSED)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW = (float)vp[2];
    vpH = (float)vp[3];

    if (!s_glReady)
        rab_gl_init();
    if (s_glReady != 1)
        return;

    if (!s_fontsTried)
        rab_fonts_init();

    /* Save BEFORE the bakes below — those create textures, so sampling the
     * binding afterwards captures one of ours and "restores" it into
     * PsyCross's cache. */
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
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    rab_gl_pump();
    rab_drain_badges();

    /* Layout in viewport pixels, origin bottom-left. Tall and narrow: the panel
     * is sized off the viewport HEIGHT so it keeps its shape in every aspect
     * ratio, then clamped so it still fits a very narrow window. */
    panelH = 0.86f * vpH;
    panelW = 0.62f * panelH;
    if (panelW > 0.90f * vpW) panelW = 0.90f * vpW;
    panelL = (vpW - panelW) * 0.5f;
    panelR = panelL + panelW;
    panelB = (vpH - panelH) * 0.5f;
    panelT = panelB + panelH;

    /* Slide in from below the screen and back out the same way. Wall clock, not
     * the game clock: the hook runs post-capture and the title screen may be
     * sitting on a held frame. */
    {
        Uint32 age = SDL_GetTicks() - s_phaseStart;
        float  travel = panelT + 8.0f;   /* far enough to clear the top edge */
        float  t, e;

        if (s_phase == RAB_OPENING)
        {
            t = (float)age / (float)RAB_OPEN_MS;
            if (t > 1.0f) t = 1.0f;
            e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   /* out-cubic */
            slide = (1.0f - e) * travel;
            dim   = e;
        }
        else if (s_phase == RAB_CLOSING)
        {
            t = (float)age / (float)RAB_CLOSE_MS;
            if (t > 1.0f) t = 1.0f;
            e = t * t * t;                                      /* in-cubic */
            slide = e * travel;
            dim   = 1.0f - t;
        }
        panelB -= slide;
        panelT -= slide;
    }

    pad    = panelW * 0.045f;
    headH  = panelH * 0.075f;
    listT  = panelT - headH;
    listB  = panelB + pad * 0.6f;
    listH  = listT - listB;

    /* Small enough that a decent number of rows fit a page. */
    rowH   = panelH * 0.082f;
    rowGap = rowH * 0.14f;
    badgeS = rowH * 0.88f;
    textL  = panelL + pad + badgeS + pad * 0.6f;
    textW  = (int)(panelR - pad - textL) > 8 ? (panelR - pad - textL) : 8.0f;

    s_viewH       = vpH;
    s_viewW       = vpW;
    s_geoListT    = listT;
    s_geoListB    = listB;
    s_geoRowPitch = rowH + rowGap;
    s_geoRowH     = rowH;
    s_geoPanelL   = panelL;
    s_geoPanelR   = panelR;
    s_scrollMax  = (float)s_rowCount * (rowH + rowGap) - listH;
    if (s_scrollMax < 0.0f) s_scrollMax = 0.0f;
    if (s_scroll > s_scrollMax) s_scroll = s_scrollMax;
    if (s_scroll < 0.0f) s_scroll = 0.0f;

    rab_build_panel((int)panelW, (int)panelH);
    rab_build_white();
    rab_build_cursor();

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glUniform1i(s_locTex, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* PsyCross leaves GL_FUNC_REVERSE_SUBTRACT set whenever the frame's last
     * prim was a PSX subtractive one and never restores it; without this the
     * panel blends dst-src and clamps to a solid black slab. */
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

#define NX(px_) (-1.0f + 2.0f * (px_) / vpW)
#define NY(py_) (-1.0f + 2.0f * (py_) / vpH)

    /* Darken the title screen instead of hiding it. The menu behind is drawn by
     * the game into the captured frame; this is a translucent wash over the
     * whole viewport, faded with the slide so the screen brightens back as the
     * panel leaves. */
    rab_quad_uv(s_texWhite, NX(0.0f), NY(vpH), NX(vpW), NY(0.0f),
                0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.72f * dim);

    rab_quad(s_texPanel, NX(panelL), NY(panelT), NX(panelR), NY(panelB), 1.0f);

    /* Only the rows intersecting the viewport are baked and drawn — the set can
     * be hundreds long and every row costs three glyph rasterizations. */
    first = (int)(s_scroll / (rowH + rowGap));
    last  = (int)((s_scroll + listH) / (rowH + rowGap)) + 1;
    if (first < 0) first = 0;
    if (last > s_rowCount) last = s_rowCount;

    for (i = first; i < last; i++)
    {
        s_RabRow* r = &s_rows[i];
        float rowTop = listT - ((float)i * (rowH + rowGap) - s_scroll);
        float rowBot = rowTop - rowH;
        float badgeT, badgeB2, tint;
        float clipT, clipB;
        GLuint badgeTex;

        /* Clip against the list window by trimming the quad and its UVs; the
         * panel has no scissor of its own and a partial row must not spill over
         * the header or off the bottom edge. */
        if (rowBot > listT || rowTop < listB)
            continue;

        rab_bake_row(r, (int)rowH, (int)textW);

        /* Hover plate. Drawn first so the row's own art sits on top of it. */
        if (i == s_hoverRow && s_detailRow < 0)
        {
            float t = (rowTop < listT) ? rowTop : listT;
            float b = (rowBot > listB) ? rowBot : listB;
            if (t > b)
                rab_quad_uv(s_texWhite, NX(panelL + 3.0f), NY(t), NX(panelR - 3.0f), NY(b),
                            0.0f, 0.0f, 1.0f, 1.0f, 0.42f, 0.16f, 0.12f, 0.55f);
        }

        /* Locked rows are dimmed rather than hidden: the description is the
         * hint, so it has to stay readable. */
        tint = r->ach.unlocked ? 1.0f : 0.45f;

        badgeT  = rowTop - (rowH - badgeS) * 0.5f;
        badgeB2 = badgeT - badgeS;
        clipT   = (rowTop < listT) ? rowTop : listT;
        clipB   = (rowBot > listB) ? rowBot : listB;

        /* Badge, or a placeholder block until its PNG lands. */
        badgeTex = 0;
        {
            const s_RabBadge* be = rab_badge_find(r->ach.badge);
            if (be) badgeTex = be->tex;
        }
        if (badgeTex)
        {
            float t = (badgeT < clipT) ? badgeT : clipT;
            float b = (badgeB2 > clipB) ? badgeB2 : clipB;
            if (t > b)
            {
                float v0 = (badgeT - t) / badgeS;
                float v1 = (badgeT - b) / badgeS;
                rab_quad_uv(badgeTex, NX(panelL + pad), NY(t),
                            NX(panelL + pad + badgeS), NY(b),
                            0.0f, v0, 1.0f, v1, tint, tint, tint, 1.0f);
            }
        }
        else
        {
            float t = (badgeT < clipT) ? badgeT : clipT;
            float b = (badgeB2 > clipB) ? badgeB2 : clipB;
            if (t > b)
                rab_quad_uv(s_texWhite, NX(panelL + pad), NY(t),
                            NX(panelL + pad + badgeS), NY(b),
                            0.0f, 0.0f, 1.0f, 1.0f, 0.16f, 0.15f, 0.17f, 1.0f);
            /* No request here: the whole set is asked for once at open. */
        }

        /* Name on top, description under it. */
        if (r->texName)
        {
            float tTop = rowTop - rowH * 0.06f;
            float tBot = tTop - (float)r->nameH;
            float t = (tTop < clipT) ? tTop : clipT;
            float b = (tBot > clipB) ? tBot : clipB;
            if (t > b)
            {
                float v0 = (tTop - t) / (float)r->nameH;
                float v1 = (tTop - b) / (float)r->nameH;
                rab_quad_uv(r->texName, NX(textL), NY(t), NX(textL + (float)r->nameW), NY(b),
                            0.0f, v0, 1.0f, v1, tint, tint, tint, 1.0f);
            }
        }
        if (r->texDesc)
        {
            float tTop = rowTop - rowH * 0.06f - (float)r->nameH * 0.92f;
            float tBot = tTop - (float)r->descH;
            float t = (tTop < clipT) ? tTop : clipT;
            float b = (tBot > clipB) ? tBot : clipB;
            if (t > b)
            {
                float v0 = (tTop - t) / (float)r->descH;
                float v1 = (tTop - b) / (float)r->descH;
                float d  = tint * 0.78f;
                rab_quad_uv(r->texDesc, NX(textL), NY(t), NX(textL + (float)r->descW), NY(b),
                            0.0f, v0, 1.0f, v1, d, d, d, 1.0f);
            }
        }
        /* Points, right-aligned. */
        if (r->texPts)
        {
            float pR   = panelR - pad;
            float tTop = rowTop - rowH * 0.08f;
            float tBot = tTop - (float)r->ptsH;
            float t = (tTop < clipT) ? tTop : clipT;
            float b = (tBot > clipB) ? tBot : clipB;
            if (t > b)
            {
                float v0 = (tTop - t) / (float)r->ptsH;
                float v1 = (tTop - b) / (float)r->ptsH;
                float g  = r->ach.unlocked ? 0.85f : 0.40f;
                rab_quad_uv(r->texPts, NX(pR - (float)r->ptsW), NY(t), NX(pR), NY(b),
                            0.0f, v0, 1.0f, v1, g, g * 0.82f, g * 0.35f, 1.0f);
            }
        }
    }

    /* Header last so a scrolled row cannot paint over it. */
    {
        static GLuint s_texHead;
        static int    s_headW, s_headH, s_headForH;
        int unlocked = 0, ptsGot = 0, ptsAll = 0;
        char head[96];

        for (i = 0; i < s_rowCount; i++)
        {
            ptsAll += (int)s_rows[i].ach.points;
            if (s_rows[i].ach.unlocked) { unlocked++; ptsGot += (int)s_rows[i].ach.points; }
        }
        snprintf(head, sizeof(head), "ACHIEVEMENTS   %d/%d   %d/%d pts",
                 unlocked, s_rowCount, ptsGot, ptsAll);

        if (s_headForH != (int)headH)
        {
            rab_retire(s_texHead);
            s_texHead  = rab_bake_line(RAB_FONT_NAME, head, headH * 0.42f,
                                       (int)(panelW - 2.0f * pad), &s_headW, &s_headH);
            s_headForH = (int)headH;
        }
        /* Opaque band behind the title. Flat-tinted white, NOT a sub-rect of
         * the panel texture — the panel's top rows are its rust border, so
         * sampling them stretched a red bar across the whole header. */
        rab_quad_uv(s_texWhite, NX(panelL + 2.0f), NY(panelT - 3.0f),
                    NX(panelR - 2.0f), NY(listT),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.055f, 0.051f, 0.059f, 1.0f);
        /* Rule under it, separating header from list. */
        rab_quad_uv(s_texWhite, NX(panelL + 2.0f), NY(listT),
                    NX(panelR - 2.0f), NY(listT - 2.0f),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.47f, 0.11f, 0.08f, 1.0f);
        if (s_texHead)
        {
            float hx = panelL + (panelW - (float)s_headW) * 0.5f;
            float hy = panelT - headH * 0.30f;
            rab_quad_uv(s_texHead, NX(hx), NY(hy), NX(hx + (float)s_headW), NY(hy - (float)s_headH),
                        0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.92f, 0.86f, 1.0f);
        }
    }

    s_geoBarL = s_geoBarR = -1.0f;   /* no bar unless the block below sets one */

    /* Scrollbar, only when there is something to scroll. */
    if (s_scrollMax > 1.0f)
    {
        /* Wider than it looks so the thumb is actually grabbable; the visible
         * bar stays slim. */
        float trackW = panelW * 0.010f;
        float trackL = panelR - trackW - 2.0f;
        s_geoBarL = trackL - panelW * 0.012f;
        s_geoBarR = panelR;
        float frac   = listH / (listH + s_scrollMax);
        float thumbH = listH * frac;
        float thumbT;
        if (thumbH < rowH * 0.4f) thumbH = rowH * 0.4f;
        thumbT = listT - (listH - thumbH) * (s_scroll / s_scrollMax);
        rab_quad_uv(s_texWhite, NX(trackL), NY(listT), NX(trackL + trackW), NY(listB),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.22f, 0.20f, 0.22f, 1.0f);
        rab_quad_uv(s_texWhite, NX(trackL), NY(thumbT), NX(trackL + trackW), NY(thumbT - thumbH),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.55f, 0.20f, 0.15f, 1.0f);
    }

    /* Detail card: covers the list (not the header) with the full description,
     * points and, for an earned one, when it was unlocked. Rebuilt only when the
     * selected row or the layout changes — this rasterizes several wrapped lines
     * and must not run every frame. */
    if (s_detailRow >= 0 && s_detailRow < s_rowCount)
    {
        static GLuint s_dTitle, s_dDesc, s_dMeta;
        static int    s_dTitleW, s_dTitleH, s_dDescW, s_dDescH, s_dMetaW, s_dMetaH;
        static int    s_dForRow = -1, s_dForH = -1;

        const s_RabRow* r  = &s_rows[s_detailRow];
        float cardT = listT - pad * 0.4f;
        float cardB = listB + pad * 0.4f;
        float cardL = panelL + pad * 0.5f;
        float cardR = panelR - pad * 0.5f;
        float innerW = cardR - cardL - 2.0f * pad;
        float bigS   = panelH * 0.16f;
        float y;

        if (s_dForRow != s_detailRow || s_dForH != (int)panelH)
        {
            char meta[128];

            rab_retire(s_dTitle); rab_retire(s_dDesc); rab_retire(s_dMeta);
            s_dTitle = s_dDesc = s_dMeta = 0;

            s_dTitle = rab_bake_wrapped(RAB_FONT_NAME, r->ach.title, panelH * 0.040f,
                                        (int)innerW, 3, &s_dTitleW, &s_dTitleH);
            s_dDesc  = rab_bake_wrapped(RAB_FONT_BODY, r->ach.desc, panelH * 0.030f,
                                        (int)innerW, 10, &s_dDescW, &s_dDescH);

            if (r->ach.unlocked && r->ach.unlockTime > 0)
            {
                time_t     tt = (time_t)r->ach.unlockTime;
                struct tm* lt = localtime(&tt);
                if (lt)
                    snprintf(meta, sizeof(meta), "%u points   -   Unlocked %04d-%02d-%02d %02d:%02d",
                             r->ach.points, lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                             lt->tm_hour, lt->tm_min);
                else
                    snprintf(meta, sizeof(meta), "%u points   -   Unlocked", r->ach.points);
            }
            else if (r->ach.unlocked)
            {
                /* Earned, but the server did not give a timestamp — say so
                 * rather than inventing one. */
                snprintf(meta, sizeof(meta), "%u points   -   Unlocked", r->ach.points);
            }
            else
            {
                snprintf(meta, sizeof(meta), "%u points   -   Locked", r->ach.points);
            }
            s_dMeta = rab_bake_wrapped(RAB_FONT_BODY, meta, panelH * 0.026f,
                                       (int)innerW, 2, &s_dMetaW, &s_dMetaH);
            s_dForRow = s_detailRow;
            s_dForH   = (int)panelH;
        }

        /* Opaque card over the list. */
        rab_quad_uv(s_texWhite, NX(cardL), NY(cardT), NX(cardR), NY(cardB),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.055f, 0.051f, 0.059f, 1.0f);
        rab_quad_uv(s_texWhite, NX(cardL), NY(cardT), NX(cardR), NY(cardT - 2.0f),
                    0.0f, 0.0f, 1.0f, 1.0f, 0.47f, 0.11f, 0.08f, 1.0f);

        y = cardT - pad;
        {
            const s_RabBadge* be = rab_badge_find(r->ach.badge);
            float bx = cardL + (cardR - cardL - bigS) * 0.5f;
            float tint = r->ach.unlocked ? 1.0f : 0.5f;
            if (be && be->tex)
                rab_quad_uv(be->tex, NX(bx), NY(y), NX(bx + bigS), NY(y - bigS),
                            0.0f, 0.0f, 1.0f, 1.0f, tint, tint, tint, 1.0f);
            else
                rab_quad_uv(s_texWhite, NX(bx), NY(y), NX(bx + bigS), NY(y - bigS),
                            0.0f, 0.0f, 1.0f, 1.0f, 0.16f, 0.15f, 0.17f, 1.0f);
            y -= bigS + pad * 0.8f;
        }
        if (s_dTitle)
        {
            float tx = cardL + (cardR - cardL - (float)s_dTitleW) * 0.5f;
            rab_quad_uv(s_dTitle, NX(tx), NY(y), NX(tx + (float)s_dTitleW), NY(y - (float)s_dTitleH),
                        0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            y -= (float)s_dTitleH + pad * 0.4f;
        }
        if (s_dMeta)
        {
            float tx = cardL + (cardR - cardL - (float)s_dMetaW) * 0.5f;
            float g  = r->ach.unlocked ? 0.90f : 0.55f;
            rab_quad_uv(s_dMeta, NX(tx), NY(y), NX(tx + (float)s_dMetaW), NY(y - (float)s_dMetaH),
                        0.0f, 0.0f, 1.0f, 1.0f, g, g * 0.82f, g * 0.42f, 1.0f);
            y -= (float)s_dMetaH + pad * 0.8f;
        }
        if (s_dDesc)
        {
            rab_quad_uv(s_dDesc, NX(cardL + pad), NY(y),
                        NX(cardL + pad + (float)s_dDescW), NY(y - (float)s_dDescH),
                        0.0f, 0.0f, 1.0f, 1.0f, 0.86f, 0.86f, 0.86f, 1.0f);
        }
    }

    /* Pointer, last of all. UiPos is the 224-tall authoring space; the viewport
     * is the presented picture, so both axes scale by the same factor the drag
     * already uses. */
    if (s_texCursor && s_phase != RAB_CLOSING)
    {
        float fx, fy;
        if (Pc_MouseCursor_ViewportPos(&fx, &fy))
        {
            float scale = vpH / 400.0f;   /* ~19px arrow at 1080p */
            float sx = fx * vpW;
            float sy = vpH - fy * vpH;
            float w  = (float)s_cursorW * scale;
            float h  = (float)s_cursorH * scale;
            rab_quad_uv(s_texCursor, NX(sx), NY(sy), NX(sx + w), NY(sy - h),
                        0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

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
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
}
