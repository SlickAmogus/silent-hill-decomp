/* Config-only PC minimap overlay (top-left corner).
 *
 * PC-native: the per-area paper-map TIM is read as raw bytes, decoded to RGBA and
 * uploaded to our own GL texture, then drawn as a GL quad on top of the frame.
 * Nothing goes through PSX VRAM, so it never fights the gameplay VRAM the paper-map
 * screen normally clobbers (that residency conflict is why this route was chosen).
 *
 * Split in two:
 *   Pc_MinimapUpdate() - game-side (game_main.c). Detects an area/paper-map change
 *                        and does the Fs read + decode there, where touching the
 *                        file queue is safe.
 *   Pc_MinimapDraw()   - GL-side (post-capture hook via DbgOverlay_Render). Uploads
 *                        any pending RGBA and draws the panel.
 *
 * Harry's map cell comes from the game's own world->map transform (func_80067914)
 * polled in query mode, so the minimap can't drift from the real paper map.
 * Areas with no paper map fall back to a scrolling grid.
 *
 * Off by default: g_PcConfig.minimap == 0 early-returns before any work. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_PaperMapFileIdxs */
#include "pc_config.h"
#include "hires_override.h"
#include "sh_log.h"
#include <PsyX/common/glad.h>
#include <math.h>
#include <stdlib.h>

/* Set while polling func_80067914 for coords only (suppresses its arrow draw). */
int g_PcMapQueryOnly = 0;

static int    s_inited = 0;
static int    s_dead   = 0;  /* latched off after any GL failure */
static int    s_trace  = 0;  /* checkpoint frames still to log */
static GLuint s_progCol = 0, s_progTex = 0, s_vao = 0, s_vbo = 0;
static GLint  s_uColor = -1, s_uTex = -1, s_uCircCol = -1, s_uCircTex = -1;
/* Panel bounds in NDC, set each frame — used to give every vertex its normalised
 * in-panel position so the circular mask applies uniformly. */
static float  s_px0, s_py0, s_px1, s_py1;

/* The first crash reports landed inside the GPU driver with no resolvable frame,
 * so trace the GL sequence for the first few drawn frames — the last checkpoint
 * in the log names the call that faulted. Cheap: stops after s_trace runs out. */
#define MM_TRACE(...) do { if (s_trace > 0) SH_DBG("[MINIMAP] " __VA_ARGS__); } while (0)

static GLuint s_mapTex = 0;
static int    s_mapTexW = 0, s_mapTexH = 0;
static int    s_mapIdxLoaded = -1;   /* paperMapIdx we last attempted */
static unsigned char* s_pendingRGBA = NULL; /* decoded, awaiting GL upload */
static int    s_pendingW = 0, s_pendingH = 0;
static unsigned char* s_rawBuf = NULL;      /* raw TIM bytes, read in flight */
static unsigned int   s_rawSize = 0;
static int    s_readPending = 0;
static int    s_idleFrames = 0;             /* consecutive frames with an idle FS queue */

#define MM_PI2 6.2831853f
/* Map-cell extents the paper map spans (screen -160..+160 / -240..+240 at 1x).
 * Used to place Harry on the map image — tune if the marker tracks off. */
/* 1.0 = crop the panel to a disc, 0.0 = square. */
#define MM_CIRCLE 0.0f
#define MM_HALF_X 160.0f
#define MM_HALF_Z 240.0f

static GLuint mm_prog(const char* vs_src, const char* fs_src)
{
    GLuint vs, fs, p;
    GLint  ok = 0;

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); return 0; }

    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

static void mm_init(void)
{
    /* GLSL 1.10 style (matches dbg_overlay.c) — no `precision` qualifier, which is
     * invalid on desktop GL and made the driver fault on the linked program. */
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    /* u_circle crops the panel to a disc: v_uv is the vertex's normalised position
     * within the panel, so anything outside the unit circle is discarded (a v flip
     * on the map quad doesn't matter — the mask is symmetric). */
    static const char* fs_col =
        "varying vec2 v_uv;\n"
        "uniform vec4 u_color;\n"
        "uniform float u_circle;\n"
        "void main() {\n"
        "    if (u_circle > 0.5) { vec2 d = v_uv * 2.0 - 1.0; if (dot(d, d) > 1.0) discard; }\n"
        "    gl_FragColor = u_color;\n"
        "}\n";
    static const char* fs_tex =
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform float u_circle;\n"
        "void main() {\n"
        "    if (u_circle > 0.5) { vec2 d = v_uv * 2.0 - 1.0; if (dot(d, d) > 1.0) discard; }\n"
        "    gl_FragColor = texture2D(u_tex, v_uv);\n"
        "}\n";

    s_inited = 1;
    s_trace  = 6;

    MM_TRACE("init: begin");
    s_progCol = mm_prog(vs_src, fs_col);
    s_progTex = mm_prog(vs_src, fs_tex);
    MM_TRACE("init: progCol=%u progTex=%u", (unsigned)s_progCol, (unsigned)s_progTex);
    if (s_progCol == 0) { s_dead = 1; return; }
    s_uColor  = glGetUniformLocation(s_progCol, "u_color");
    s_uCircCol = glGetUniformLocation(s_progCol, "u_circle");
    if (s_progTex) {
        s_uTex     = glGetUniformLocation(s_progTex, "u_tex");
        s_uCircTex = glGetUniformLocation(s_progTex, "u_circle");
    }

    /* VAOs are core-profile only; if the entry point is missing this would jump
     * through a null pointer, so verify it before use. */
    if (glGenVertexArrays == NULL || glBindVertexArray == NULL) {
        SH_DBG("[MINIMAP] no VAO entry points — disabling");
        s_dead = 1;
        return;
    }
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    MM_TRACE("init: vao=%u vbo=%u", (unsigned)s_vao, (unsigned)s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 2048 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* verts are x,y,u,v quads. */
static void mm_put(float* v, int i, float x, float y, float u, float t)
{
    v[i*4] = x; v[i*4+1] = y; v[i*4+2] = u; v[i*4+3] = t;
}

static void mm_upload_draw(GLenum mode, const float* v, int nverts)
{
    if (nverts <= 0) return;
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nverts * 4 * sizeof(float), v);
    glDrawArrays(mode, 0, nverts);
}

/* Position-only vertex: derives its in-panel uv so the circle mask can clip it. */
static void mm_putp(float* v, int i, float x, float y)
{
    mm_put(v, i, x, y,
           (x - s_px0) / (s_px1 - s_px0),
           (y - s_py0) / (s_py1 - s_py0));
}

static void mm_draw_col(GLenum mode, const float* v, int nverts,
                        float r, float g, float b, float a)
{
    glUseProgram(s_progCol);
    glUniform4f(s_uColor, r, g, b, a);
    glUniform1f(s_uCircCol, MM_CIRCLE);
    mm_upload_draw(mode, v, nverts);
}

/* ---- game-side: load the area's paper-map TIM when it changes ---- */
void Pc_MinimapUpdate(void)
{
    int idx;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return;

    /* A read in flight: poll (never pump) until the queue drains, then decode. */
    if (s_readPending)
    {
        if (Fs_QueueGetLength() > 0) return;
        s_readPending = 0;
        if (HiresOverride_DecodeToRGBA(s_rawBuf, s_rawSize,
                                       &s_pendingRGBA, &s_pendingW, &s_pendingH) != 0)
        {
            s_pendingRGBA = NULL;
        }
        free(s_rawBuf); s_rawBuf = NULL;
        SH_DBG("[MINIMAP] map %dx%d %s", s_pendingW, s_pendingH,
               s_pendingRGBA ? "decoded" : "FAILED");
        return;
    }

    idx = (int)g_SavegamePtr->paperMapIdx;
    if (idx == s_mapIdxLoaded) return;

    /* The previous guard (queue length 0) was not enough: during an area load the
     * queue dips to empty between the game's own reads, and we slipped a read in
     * there — the crash landed immediately after our decode, mid [TEXVRAM] burst.
     * Worse, Fs_QueueWaitForEmpty PUMPS the queue, running post-load callbacks
     * (which do GL texture uploads) at a point the game never expects them.
     *
     * So: never block, and only start once the queue has been continuously idle
     * for a good while — long past any loading burst. The map is cosmetic; it can
     * appear a couple of seconds late. */
    if (Fs_QueueGetLength() > 0) { s_idleFrames = 0; return; }
    if (++s_idleFrames < 180) return;

    {
        e_FsFile f    = (e_FsFile)(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[idx]);
        s32      size = Fs_GetFileSize(f);

        if (size <= 0) { s_mapIdxLoaded = idx; return; }
        s_rawBuf = (unsigned char*)malloc((size_t)size + 2048);
        if (s_rawBuf == NULL) return; /* retry later */

        s_mapIdxLoaded = idx;
        s_rawSize      = (unsigned int)size;
        if (s_pendingRGBA) { free(s_pendingRGBA); s_pendingRGBA = NULL; }

        Fs_QueueStartRead(f, s_rawBuf);
        s_readPending = 1;   /* completion polled above — no Fs_QueueWaitForEmpty */
        SH_DBG("[MINIMAP] queued map read: paperMapIdx=%d file=%d size=%d", idx, (int)f, (int)size);
    }
}

/* ---- GL-side: upload any pending map image, then draw the panel ---- */
void Pc_MinimapDraw(void)
{
    GLint     vp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevBsrc = 0, prevBdst = 0, prevTex = 0;
    GLint     prevActive = GL_TEXTURE0;
    GLint     prevFb = 0, prevBeqRgb = GL_FUNC_ADD, prevBeqA = GL_FUNC_ADD;
    GLboolean prevDepth, prevBlend;
    float     aspect, hh, hw, cx, cy, x0, y0, x1, y1;
    float     buf[2048 / 4 * 4];
    int       n;
    s32       packed;
    int       haveCell = 0;
    float     mapU = 0.5f, mapV = 0.5f;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return;

    if (!s_inited) mm_init();
    if (s_dead || s_progCol == 0) return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;
    MM_TRACE("draw: vp=%d,%d %dx%d", vp[0], vp[1], vp[2], vp[3]);

    /* Harry's map cell, straight from the game's own transform (query mode = no
     * paper-map arrow drawn). Returns 0 when this area has no map. */
    MM_TRACE("draw: querying map cell");
    g_PcMapQueryOnly = 1;
    packed = func_80067914((s32)g_SavegamePtr->paperMapIdx, 0, 0, (u16)Q12(1.0f));
    g_PcMapQueryOnly = 0;
    MM_TRACE("draw: map cell packed=0x%08x", (unsigned)packed);
    if (packed != 0)
    {
        float mx = (float)(s16)(packed & 0xFFFF);
        float mz = (float)(s16)((packed >> 16) & 0xFFFF);
        mapU = (mx + MM_HALF_X) / (2.0f * MM_HALF_X);
        mapV = (mz + MM_HALF_Z) / (2.0f * MM_HALF_Z);
        if (mapU < 0.0f) mapU = 0.0f;
        if (mapU > 1.0f) mapU = 1.0f;
        if (mapV < 0.0f) mapV = 0.0f;
        if (mapV > 1.0f) mapV = 1.0f;
        haveCell = 1;
    }

    /* --- save GL state --- */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    /* All our texture work happens on unit 0, so save unit 0's binding
     * specifically (and the active unit) — reading the binding without
     * selecting the unit first would record some other unit's texture. */
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBdst);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBeqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBeqA);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);

    MM_TRACE("draw: state saved (prog=%d vao=%d buf=%d)", prevProg, prevVao, prevBuf);
    /* Draw to the DEFAULT framebuffer, exactly as dbg_overlay.c does in this same
     * hook. Without this we were rendering into whatever FBO PsyX had bound at
     * EndScene (its internal PSX framebuffer target) instead of the screen. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    MM_TRACE("draw: bound vao/vbo");

    /* Upload a freshly decoded map image. MUST happen after the state save above:
     * doing it earlier bound our texture over whatever PsyX had bound on the live
     * texture unit and then left 0 there, and the later glGetIntegerv read back
     * that 0 as the "previous" binding — so PsyX kept rendering with an unbound
     * texture and the driver's async worker thread faulted a frame later. */
    if (s_pendingRGBA && s_pendingW > 0 && s_pendingH > 0)
    {
        MM_TRACE("draw: uploading map tex %dx%d", s_pendingW, s_pendingH);
        if (s_mapTex == 0) glGenTextures(1, &s_mapTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_mapTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_pendingW, s_pendingH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_pendingRGBA);
        s_mapTexW = s_pendingW; s_mapTexH = s_pendingH;
        free(s_pendingRGBA); s_pendingRGBA = NULL;
        MM_TRACE("draw: map tex uploaded (tex=%u)", (unsigned)s_mapTex);
    }

    /* --- panel rect (square, top-right, NDC) --- */
    aspect = (float)vp[2] / (float)vp[3];
    hh = 0.17f;
    hw = hh / aspect;
    cx = -0.97f + hw;   /* top-LEFT */
    cy = 0.97f - hh;
    x0 = cx - hw; x1 = cx + hw; y0 = cy - hh; y1 = cy + hh;
    s_px0 = x0; s_py0 = y0; s_px1 = x1; s_py1 = y1;

    n = 0;
    mm_putp(buf, n++, x0, y0); mm_putp(buf, n++, x1, y0); mm_putp(buf, n++, x1, y1);
    mm_putp(buf, n++, x0, y0); mm_putp(buf, n++, x1, y1); mm_putp(buf, n++, x0, y1);
    mm_draw_col(GL_TRIANGLES, buf, n, 0.04f, 0.05f, 0.08f, 0.55f);
    MM_TRACE("draw: panel drawn");

    if (s_mapTex != 0 && s_progTex != 0)
    {
        /* whole area map in the panel; v flipped (texture rows grow downward) */
        n = 0;
        mm_put(buf, n++, x0, y0, 0.0f, 1.0f); mm_put(buf, n++, x1, y0, 1.0f, 1.0f);
        mm_put(buf, n++, x1, y1, 1.0f, 0.0f);
        mm_put(buf, n++, x0, y0, 0.0f, 1.0f); mm_put(buf, n++, x1, y1, 1.0f, 0.0f);
        mm_put(buf, n++, x0, y1, 0.0f, 0.0f);
        MM_TRACE("draw: map quad (tex=%u prog=%u)", (unsigned)s_mapTex, (unsigned)s_progTex);
        glUseProgram(s_progTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_mapTex);
        glUniform1i(s_uTex, 0);
        glUniform1f(s_uCircTex, MM_CIRCLE);
        mm_upload_draw(GL_TRIANGLES, buf, n);
        glBindTexture(GL_TEXTURE_2D, 0);
        MM_TRACE("draw: map quad done");
    }
    else
    {
        /* no paper map for this area — north-up grid so the panel still tracks */
        const float CELL = 40.0f, VIEW = 100.0f;
        float harryX = (float)g_SysWork.playerWork.player.position.vx / 4096.0f;
        float harryZ = (float)g_SysWork.playerWork.player.position.vz / 4096.0f;
        float sY = hh / VIEW, sX = (hh / VIEW) / aspect, wx, wz, st;
        n = 0;
        st = floorf((harryX - VIEW) / CELL) * CELL;
        for (wx = st; wx <= harryX + VIEW && n < 400; wx += CELL) {
            float nx = cx + (wx - harryX) * sX;
            mm_putp(buf, n++, nx, y0); mm_putp(buf, n++, nx, y1);
        }
        st = floorf((harryZ - VIEW) / CELL) * CELL;
        for (wz = st; wz <= harryZ + VIEW && n < 400; wz += CELL) {
            float ny = cy + (wz - harryZ) * sY;
            mm_putp(buf, n++, x0, ny); mm_putp(buf, n++, x1, ny);
        }
        mm_draw_col(GL_LINES, buf, n, 0.35f, 0.42f, 0.5f, 0.6f);
    }

    /* --- Harry marker: on the map at his cell, else panel centre --- */
    {
        float px = haveCell ? (x0 + mapU * (x1 - x0)) : cx;
        float py = haveCell ? (y1 - mapV * (y1 - y0)) : cy;
        /* nothing clips us any more, so keep the arrow inside the panel */
        float mx0 = x0 + 0.012f, mx1 = x1 - 0.012f;
        float my0 = y0 + 0.020f, my1 = y1 - 0.020f;
        if (px < mx0) px = mx0;
        if (px > mx1) px = mx1;
        if (py < my0) py = my0;
        if (py > my1) py = my1;
        float s  = 0.013f;   /* small arrow */
        float ang = (float)g_SysWork.playerWork.player.rotation.vy / 4096.0f * MM_PI2;
        float lx[3] = { 0.0f, -0.6f, 0.6f };
        float ly[3] = { 1.0f, -0.7f, -0.7f };
        float ca = cosf(ang), sa = sinf(ang);
        int   i;
        n = 0;
        for (i = 0; i < 3; i++) {
            float rx = lx[i] * ca - ly[i] * sa;
            float ry = lx[i] * sa + ly[i] * ca;
            mm_putp(buf, n++, px + (rx * s) / aspect, py + ry * s);
        }
        mm_draw_col(GL_TRIANGLES, buf, n, 1.0f, 0.85f, 0.2f, 1.0f);
        MM_TRACE("draw: marker drawn");
    }

    /* --- restore --- */
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)prevBsrc, (GLenum)prevBdst);
    glBlendEquationSeparate((GLenum)prevBeqRgb, (GLenum)prevBeqA);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFb);
    /* Put unit 0's binding back, THEN reselect PsyX's active unit — doing it the
     * other way round binds our saved texture onto the wrong unit. */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevActive);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    MM_TRACE("draw: restored — frame ok");
    if (s_trace > 0) s_trace--;
}
