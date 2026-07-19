/* Config-only PC minimap overlay (top-right corner).
 *
 * SPIKE stage: a raw-GL overlay that proves the whole pipeline end-to-end — the
 * post-capture GL hook, the config gate, a front-most top-right panel, and LIVE
 * tracking of Harry (a north-up grid that scrolls as he moves + an arrow that
 * rotates with his heading). The actual per-area paper-map texture is the next
 * layer that replaces the grid; the world->screen math here is what it will reuse.
 *
 * Drawn PC-side in OpenGL (not through the PSX OT), so it never touches PSX VRAM.
 * Called from DbgOverlay_Render (the g_PsyX_PostCaptureHook), which runs every
 * frame after the world is drawn. Off by default: g_PcConfig.minimap == 0 makes
 * the whole function early-return before any GL state is touched (byte-identical). */

#include "game.h"
#include "pc_config.h"
#include <PsyX/common/glad.h>
#include <math.h>

static int    s_inited = 0;      /* GL objects created (even if compile failed) */
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static GLint  s_u_color = -1;

#define MM_PI2 6.2831853f

static void mm_init(void)
{
    GLuint vs, fs;
    GLint  ok = 0;
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    /* GLSL 1.10 style (matches dbg_overlay.c). NO `precision` qualifier — it is
     * invalid in desktop GLSL 1.10 and made the driver crash on the linked-but-
     * broken program. */
    static const char* fs_src =
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = u_color; }\n";

    s_inited = 1;

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); return; }

    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); return; }

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) { glDeleteProgram(s_prog); s_prog = 0; return; }

    s_u_color = glGetUniformLocation(s_prog, "u_color");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 1024 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* Upload `nverts` xy pairs and draw them as `mode` in the given RGBA. */
static void mm_draw(GLenum mode, const float* xy, int nverts, float r, float g, float b, float a)
{
    if (nverts <= 0) return;
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nverts * 2 * sizeof(float), xy);
    glUniform4f(s_u_color, r, g, b, a);
    glDrawArrays(mode, 0, nverts);
}

void Pc_MinimapDraw(void)
{
    GLint     vp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevBsrc = 0, prevBdst = 0;
    GLboolean prevDepth, prevBlend, prevScissor;
    float     aspect, hh, hw, cx, cy, x0, y0, x1, y1;
    float     harryX, harryZ, ang, sY, sX;
    float     buf[1024]; /* 512 verts — grid is ~a few dozen lines; capped below */
    int       n;
    int       sx, sy, sw, sh;
    const float CELL = 40.0f;   /* world units per map cell (CHUNK_CELL_SIZE = Q12(40)) */
    const float VIEW = 100.0f;  /* world half-extent shown around Harry (zoom) */
    float wx, wz, startX, startZ;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay) return;

    if (!s_inited) mm_init();
    if (s_prog == 0) return; /* shader failed — stay disabled, no crash */

    /* --- save the GL state we touch --- */
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBdst);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    /* --- panel rect (square, top-right, NDC) --- */
    aspect = (float)vp[2] / (float)vp[3];
    hh = 0.17f;          /* half-height in NDC */
    hw = hh / aspect;    /* half-width -> square on screen */
    cx = 0.97f - hw;     /* panel center */
    cy = 0.97f - hh;
    x0 = cx - hw; x1 = cx + hw; y0 = cy - hh; y1 = cy + hh;

    /* panel background (translucent dark) */
    {
        float q[12] = { x0,y0, x1,y0, x1,y1,  x0,y0, x1,y1, x0,y1 };
        mm_draw(GL_TRIANGLES, q, 6, 0.04f, 0.05f, 0.08f, 0.55f);
    }

    /* clip everything below to the panel */
    sx = (int)((x0 * 0.5f + 0.5f) * vp[2]) + vp[0];
    sy = (int)((y0 * 0.5f + 0.5f) * vp[3]) + vp[1];
    sw = (int)((x1 - x0) * 0.5f * vp[2]);
    sh = (int)((y1 - y0) * 0.5f * vp[3]);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);

    /* --- north-up scrolling grid, centered on Harry --- */
    harryX = (float)g_SysWork.playerWork.player.position.vx / 4096.0f;
    harryZ = (float)g_SysWork.playerWork.player.position.vz / 4096.0f;
    sY = hh / VIEW;        /* NDC-Y per world unit */
    sX = sY / aspect;      /* keep world square */

    n = 0;
    startX = floorf((harryX - VIEW) / CELL) * CELL;
    for (wx = startX; wx <= harryX + VIEW && n < 500; wx += CELL) {
        float nx = cx + (wx - harryX) * sX;
        buf[n*2] = nx; buf[n*2+1] = y0; n++;
        buf[n*2] = nx; buf[n*2+1] = y1; n++;
    }
    startZ = floorf((harryZ - VIEW) / CELL) * CELL;
    for (wz = startZ; wz <= harryZ + VIEW && n < 500; wz += CELL) {
        float ny = cy + (wz - harryZ) * sY; /* world +Z -> up (tunable) */
        buf[n*2] = x0; buf[n*2+1] = ny; n++;
        buf[n*2] = x1; buf[n*2+1] = ny; n++;
    }
    mm_draw(GL_LINES, buf, n, 0.35f, 0.42f, 0.5f, 0.6f);

    /* --- Harry arrow at panel center, rotated by heading --- */
    ang = (float)g_SysWork.playerWork.player.rotation.vy / 4096.0f * MM_PI2; /* dir tunable */
    {
        float s = 0.045f;                 /* arrow size (NDC-Y) */
        /* local tri pointing +Y (north at ang=0): tip, back-left, back-right */
        float lx[3] = { 0.0f, -0.6f, 0.6f };
        float ly[3] = { 1.0f, -0.7f, -0.7f };
        float ca = cosf(ang), sa = sinf(ang);
        float tri[6];
        int i;
        for (i = 0; i < 3; i++) {
            float rx = lx[i] * ca - ly[i] * sa;
            float ry = lx[i] * sa + ly[i] * ca;
            tri[i*2]   = cx + (rx * s) / aspect;
            tri[i*2+1] = cy + (ry * s);
        }
        mm_draw(GL_TRIANGLES, tri, 3, 1.0f, 0.85f, 0.2f, 1.0f);
    }

    /* --- restore --- */
    glDisable(GL_SCISSOR_TEST);
    if (!prevScissor) glDisable(GL_SCISSOR_TEST);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)prevBsrc, (GLenum)prevBdst);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
