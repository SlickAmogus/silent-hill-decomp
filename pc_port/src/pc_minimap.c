/* Config-only PC minimap overlay (top-right corner).
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
static GLuint s_progCol = 0, s_progTex = 0, s_vao = 0, s_vbo = 0;
static GLint  s_uColor = -1, s_uTex = -1;

static GLuint s_mapTex = 0;
static int    s_mapTexW = 0, s_mapTexH = 0;
static int    s_mapIdxLoaded = -1;   /* paperMapIdx we last attempted */
static unsigned char* s_pendingRGBA = NULL; /* decoded, awaiting GL upload */
static int    s_pendingW = 0, s_pendingH = 0;

#define MM_PI2 6.2831853f
/* Map-cell extents the paper map spans (screen -160..+160 / -240..+240 at 1x).
 * Used to place Harry on the map image — tune if the marker tracks off. */
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
    static const char* fs_col =
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = u_color; }\n";
    static const char* fs_tex =
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

    s_inited = 1;

    s_progCol = mm_prog(vs_src, fs_col);
    s_progTex = mm_prog(vs_src, fs_tex);
    if (s_progCol == 0) return;
    s_uColor = glGetUniformLocation(s_progCol, "u_color");
    if (s_progTex) s_uTex = glGetUniformLocation(s_progTex, "u_tex");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
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

static void mm_draw_col(GLenum mode, const float* v, int nverts,
                        float r, float g, float b, float a)
{
    glUseProgram(s_progCol);
    glUniform4f(s_uColor, r, g, b, a);
    mm_upload_draw(mode, v, nverts);
}

/* ---- game-side: load the area's paper-map TIM when it changes ---- */
void Pc_MinimapUpdate(void)
{
    int idx;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return;

    idx = (int)g_SavegamePtr->paperMapIdx;
    if (idx == s_mapIdxLoaded) return;
    s_mapIdxLoaded = idx; /* attempt once per change, even if it fails */

    if (s_pendingRGBA) { free(s_pendingRGBA); s_pendingRGBA = NULL; }

    {
        e_FsFile       f    = (e_FsFile)(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[idx]);
        s32            size = Fs_GetFileSize(f);
        unsigned char* raw;

        if (size <= 0) return;
        raw = (unsigned char*)malloc((size_t)size + 2048);
        if (raw == NULL) return;

        Fs_QueueStartRead(f, raw);
        Fs_QueueWaitForEmpty();

        if (HiresOverride_DecodeToRGBA(raw, (unsigned int)size,
                                       &s_pendingRGBA, &s_pendingW, &s_pendingH) != 0)
        {
            s_pendingRGBA = NULL;
        }
        free(raw);
        SH_DBG("[MINIMAP] paperMapIdx=%d file=%d -> %dx%d %s", idx, (int)f,
               s_pendingW, s_pendingH, s_pendingRGBA ? "decoded" : "FAILED");
    }
}

/* ---- GL-side: upload any pending map image, then draw the panel ---- */
void Pc_MinimapDraw(void)
{
    GLint     vp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevBsrc = 0, prevBdst = 0, prevTex = 0;
    GLboolean prevDepth, prevBlend, prevScissor;
    float     aspect, hh, hw, cx, cy, x0, y0, x1, y1;
    float     buf[2048 / 4 * 4];
    int       n, sx, sy, sw, sh;
    s32       packed;
    int       haveCell = 0;
    float     mapU = 0.5f, mapV = 0.5f;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return;

    if (!s_inited) mm_init();
    if (s_progCol == 0) return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;

    /* upload a freshly decoded map image */
    if (s_pendingRGBA && s_pendingW > 0 && s_pendingH > 0)
    {
        if (s_mapTex == 0) glGenTextures(1, &s_mapTex);
        glBindTexture(GL_TEXTURE_2D, s_mapTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_pendingW, s_pendingH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_pendingRGBA);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_mapTexW = s_pendingW; s_mapTexH = s_pendingH;
        free(s_pendingRGBA); s_pendingRGBA = NULL;
    }

    /* Harry's map cell, straight from the game's own transform (query mode = no
     * paper-map arrow drawn). Returns 0 when this area has no map. */
    g_PcMapQueryOnly = 1;
    packed = func_80067914((s32)g_SavegamePtr->paperMapIdx, 0, 0, (u16)Q12(1.0f));
    g_PcMapQueryOnly = 0;
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
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBdst);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    /* --- panel rect (square, top-right, NDC) --- */
    aspect = (float)vp[2] / (float)vp[3];
    hh = 0.17f;
    hw = hh / aspect;
    cx = 0.97f - hw;
    cy = 0.97f - hh;
    x0 = cx - hw; x1 = cx + hw; y0 = cy - hh; y1 = cy + hh;

    n = 0;
    mm_put(buf, n++, x0, y0, 0, 0); mm_put(buf, n++, x1, y0, 0, 0); mm_put(buf, n++, x1, y1, 0, 0);
    mm_put(buf, n++, x0, y0, 0, 0); mm_put(buf, n++, x1, y1, 0, 0); mm_put(buf, n++, x0, y1, 0, 0);
    mm_draw_col(GL_TRIANGLES, buf, n, 0.04f, 0.05f, 0.08f, 0.55f);

    sx = (int)((x0 * 0.5f + 0.5f) * vp[2]) + vp[0];
    sy = (int)((y0 * 0.5f + 0.5f) * vp[3]) + vp[1];
    sw = (int)((x1 - x0) * 0.5f * vp[2]);
    sh = (int)((y1 - y0) * 0.5f * vp[3]);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);

    if (s_mapTex != 0 && s_progTex != 0)
    {
        /* whole area map in the panel; v flipped (texture rows grow downward) */
        n = 0;
        mm_put(buf, n++, x0, y0, 0.0f, 1.0f); mm_put(buf, n++, x1, y0, 1.0f, 1.0f);
        mm_put(buf, n++, x1, y1, 1.0f, 0.0f);
        mm_put(buf, n++, x0, y0, 0.0f, 1.0f); mm_put(buf, n++, x1, y1, 1.0f, 0.0f);
        mm_put(buf, n++, x0, y1, 0.0f, 0.0f);
        glUseProgram(s_progTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_mapTex);
        glUniform1i(s_uTex, 0);
        mm_upload_draw(GL_TRIANGLES, buf, n);
        glBindTexture(GL_TEXTURE_2D, 0);
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
            mm_put(buf, n++, nx, y0, 0, 0); mm_put(buf, n++, nx, y1, 0, 0);
        }
        st = floorf((harryZ - VIEW) / CELL) * CELL;
        for (wz = st; wz <= harryZ + VIEW && n < 400; wz += CELL) {
            float ny = cy + (wz - harryZ) * sY;
            mm_put(buf, n++, x0, ny, 0, 0); mm_put(buf, n++, x1, ny, 0, 0);
        }
        mm_draw_col(GL_LINES, buf, n, 0.35f, 0.42f, 0.5f, 0.6f);
    }

    /* --- Harry marker: on the map at his cell, else panel centre --- */
    {
        float px = haveCell ? (x0 + mapU * (x1 - x0)) : cx;
        float py = haveCell ? (y1 - mapV * (y1 - y0)) : cy;
        float s  = 0.040f;
        float ang = (float)g_SysWork.playerWork.player.rotation.vy / 4096.0f * MM_PI2;
        float lx[3] = { 0.0f, -0.6f, 0.6f };
        float ly[3] = { 1.0f, -0.7f, -0.7f };
        float ca = cosf(ang), sa = sinf(ang);
        int   i;
        n = 0;
        for (i = 0; i < 3; i++) {
            float rx = lx[i] * ca - ly[i] * sa;
            float ry = lx[i] * sa + ly[i] * ca;
            mm_put(buf, n++, px + (rx * s) / aspect, py + ry * s, 0, 0);
        }
        mm_draw_col(GL_TRIANGLES, buf, n, 1.0f, 0.85f, 0.2f, 1.0f);
    }

    /* --- restore --- */
    if (!prevScissor) glDisable(GL_SCISSOR_TEST);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)prevBsrc, (GLenum)prevBdst);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
