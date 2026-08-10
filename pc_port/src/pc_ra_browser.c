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

#include <SDL.h>
#include <PsyX/common/glad.h>

#include "stb_truetype.h"

#include "pc_ra_browser.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"
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
    GLuint   texBadge;
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
static int      s_open;

/* Scroll position in pixels, and where the content ends. */
static float s_scroll;
static float s_scrollMax;
static float s_velocity;      /* px/sec, for release-flick and key repeat */

/* Viewport pixels per UI pixel. Pc_MouseCursor_UiPos reports in the 224-tall
 * text-authoring space while the panel is laid out in viewport pixels, so a
 * drag delta has to be converted. Published by Draw; the default covers the
 * first frame, before Draw has ever run. */
static float s_dragScale = 4.0f;

/* Drag tracking. */
static int   s_dragging;
static int   s_dragLastY;
static float s_dragScrollAtGrab;
static int   s_dragMovedPx;

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
static GLuint s_texPanel, s_texWhite;
static int    s_panelW, s_panelH;

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

#define RAB_PEND_MAX 16
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
        rab_retire(s_rows[i].texBadge);
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

void Pc_RaBrowser_ProvideBadge(const char* badgeName, const unsigned char* png, size_t len)
{
    int i;

    if (!badgeName || !badgeName[0] || !png || !len || s_pendCount >= RAB_PEND_MAX)
        return;
    /* Only keep bytes for a row that is actually in the list and still bare. */
    for (i = 0; i < s_rowCount; i++)
    {
        if (s_rows[i].texBadge == 0 && strcmp(s_rows[i].ach.badge, badgeName) == 0)
            break;
    }
    if (i >= s_rowCount)
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
            /* One decode serves every row sharing the badge, but each row owns
             * its own name so a per-row texture would be freed twice. Assign to
             * the first bare match and retire nothing. */
            for (i = 0; i < s_rowCount; i++)
            {
                if (s_rows[i].texBadge == 0 && strcmp(s_rows[i].ach.badge, s_pend[p].badge) == 0)
                {
                    s_rows[i].texBadge = tex;
                    tex = 0;
                    break;
                }
            }
            if (tex)
                rab_retire(tex);   /* nobody wanted it after all */
        }
    }
    s_pendCount = 0;
}

/* ------------------------------------------------------------------ */
/* Open / close / input                                                */
/* ------------------------------------------------------------------ */

void Pc_RaBrowser_Open(void)
{
    if (s_open)
        return;
    rab_build_list();
    if (s_rowCount == 0)
    {
        SH_DBG("[RABROWSE] no achievement set loaded - not opening");
        return;
    }
    s_open      = 1;
    s_scroll    = 0.0f;
    s_velocity  = 0.0f;
    s_dragging  = 0;
    s_armClose  = 0;
    s_lastTick  = SDL_GetTicks();
}

int Pc_RaBrowser_IsOpen(void)
{
    return s_open;
}

static void rab_close(void)
{
    int i;

    s_open     = 0;
    s_dragging = 0;
    s_velocity = 0.0f;
    /* Badge bytes are drained by Draw, which stops running the moment this
     * returns — anything still queued would leak. */
    for (i = 0; i < s_pendCount; i++)
        free(s_pend[i].png);
    s_pendCount = 0;
    /* Rows keep their textures: reopening is common and the badge art would
     * otherwise be re-downloaded. They are released only when the list is
     * rebuilt, which happens on the next open. */
}

/* padDown = the raw held-button mask from the caller. Passed in rather than
 * read here so this TU stays free of the game headers — the toast's sibling
 * modules that pulled in game.h alongside <windows.h>-adjacent code are exactly
 * where this project's header collisions live. */
void Pc_RaBrowser_Update(unsigned padDown)
{
    const Uint8* keys;
    Uint32 now;
    float  dt;
    int    anyDown = 0;
    int    mx, my;

    if (!s_open)
        return;

    now = SDL_GetTicks();
    dt  = (float)(now - s_lastTick) / 1000.0f;
    s_lastTick = now;
    if (dt <= 0.0f)  dt = 1.0f / 60.0f;
    if (dt > 0.25f)  dt = 0.25f;   /* a load hitch must not fling the list */

    keys = SDL_GetKeyboardState(NULL);

    /* Arrow keys scroll continuously while held. */
    if (keys)
    {
        const float KEY_SPEED = 620.0f;
        if (keys[SDL_SCANCODE_UP])   s_scroll -= KEY_SPEED * dt;
        if (keys[SDL_SCANCODE_DOWN]) s_scroll += KEY_SPEED * dt;
        if (keys[SDL_SCANCODE_PAGEUP])   s_scroll -= KEY_SPEED * 3.0f * dt;
        if (keys[SDL_SCANCODE_PAGEDOWN]) s_scroll += KEY_SPEED * 3.0f * dt;
        if (keys[SDL_SCANCODE_HOME]) s_scroll = 0.0f;
        if (keys[SDL_SCANCODE_END])  s_scroll = s_scrollMax;
    }

    /* Wheel: one notch ~= a third of a page. */
    {
        int step = Pc_MouseCursor_WheelStep();
        if (step)
        {
            s_scroll  -= (float)step * 140.0f;
            s_velocity = 0.0f;
        }
    }

    /* Drag. The grab anchor is kept in content space so the row under the
     * cursor stays under the cursor however far the pointer travels. */
    if (Pc_MouseCursor_UiPos(&mx, &my))
    {
        if (Pc_MouseCursor_LeftHeld())
        {
            if (!s_dragging)
            {
                s_dragging         = 1;
                s_dragLastY        = my;
                s_dragScrollAtGrab = s_scroll;
                s_dragMovedPx      = 0;
                s_velocity         = 0.0f;
            }
            else
            {
                int delta = my - s_dragLastY;
                if (delta > 0 || delta < 0)
                {
                    s_dragMovedPx += (delta < 0) ? -delta : delta;
                    /* UI pixels are the 320x240 virtual space; the panel is laid
                     * out in viewport pixels, so scale by the ratio the draw used. */
                    s_scroll  -= (float)delta * s_dragScale;
                    s_velocity = -(float)delta * s_dragScale / dt;
                }
                s_dragLastY = my;
            }
        }
        else if (s_dragging)
        {
            s_dragging = 0;   /* release: keep s_velocity for the flick */
        }
    }
    else
    {
        s_dragging = 0;
    }

    /* Flick decay. */
    if (!s_dragging && (s_velocity > 1.0f || s_velocity < -1.0f))
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

    /* Close on any button that is not a scroll control. Edge-triggered through
     * s_armClose: the Map press that opened the panel is still down on this
     * first frame, and without the arm it would close instantly. */
    if (padDown != 0)
        anyDown = 1;
    if (keys)
    {
        int sc;
        for (sc = SDL_SCANCODE_A; sc < SDL_NUM_SCANCODES; sc++)
        {
            if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ||
                sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_PAGEDOWN ||
                sc == SDL_SCANCODE_HOME || sc == SDL_SCANCODE_END ||
                sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT)
                continue;
            if (keys[sc]) { anyDown = 1; break; }
        }
    }

    if (!s_armClose)
    {
        if (!anyDown)
            s_armClose = 1;   /* everything released — arm the close */
    }
    else if (anyDown)
    {
        rab_close();
        return;
    }

    /* A click that did not drag also dismisses, matching "press anything". */
    if (s_armClose && Pc_MouseCursor_RightClicked())
        rab_close();
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
    float pad, headH, listT, listB, listH, rowH, rowGap, badgeS, textL, textW;
    int   i, first, last;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0;
    GLint  prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO;
    GLint  prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (!s_open)
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

    s_dragScale  = vpH / 224.0f;
    s_scrollMax  = (float)s_rowCount * (rowH + rowGap) - listH;
    if (s_scrollMax < 0.0f) s_scrollMax = 0.0f;
    if (s_scroll > s_scrollMax) s_scroll = s_scrollMax;
    if (s_scroll < 0.0f) s_scroll = 0.0f;

    rab_build_panel((int)panelW, (int)panelH);
    rab_build_white();

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

        /* Clip against the list window by trimming the quad and its UVs; the
         * panel has no scissor of its own and a partial row must not spill over
         * the header or off the bottom edge. */
        if (rowBot > listT || rowTop < listB)
            continue;

        rab_bake_row(r, (int)rowH, (int)textW);

        /* Locked rows are dimmed rather than hidden: the description is the
         * hint, so it has to stay readable. */
        tint = r->ach.unlocked ? 1.0f : 0.45f;

        badgeT  = rowTop - (rowH - badgeS) * 0.5f;
        badgeB2 = badgeT - badgeS;
        clipT   = (rowTop < listT) ? rowTop : listT;
        clipB   = (rowBot > listB) ? rowBot : listB;

        /* Badge, or a placeholder block until its PNG lands. */
        if (r->texBadge)
        {
            float t = (badgeT < clipT) ? badgeT : clipT;
            float b = (badgeB2 > clipB) ? badgeB2 : clipB;
            if (t > b)
            {
                float v0 = (badgeT - t) / badgeS;
                float v1 = (badgeT - b) / badgeS;
                rab_quad_uv(r->texBadge, NX(panelL + pad), NY(t),
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
            /* Ask for the art now that the row is actually on screen. */
            Pc_Ra_RequestBadge(r->ach.badge);
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

    /* Scrollbar, only when there is something to scroll. */
    if (s_scrollMax > 1.0f)
    {
        float trackW = panelW * 0.010f;
        float trackL = panelR - trackW - 2.0f;
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
