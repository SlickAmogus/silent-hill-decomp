/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character Select's preview column: the chosen swap, fitted to the character
 * it will draw, turning slowly on a stand. Rendered into a private framebuffer
 * with its own depth buffer (the window's is not guaranteed one) and composited
 * into the native screen's frame from the post-capture hook, like every other
 * PC-side GL panel. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "pc_chara_preview.h"
#include "pc_chara_select.h"
#include "pc_skin_chara.h"
#include "sh_log.h"

extern void  PsyX_GetDisplayViewport(int* outX, int* outY, int* outW, int* outH);
extern int   g_PsxWideMenu;
extern float PsyX_WideMenuMargin(void);

#define CP_FBO_W     384
#define CP_FBO_H     640
#define CP_MAX_MATS  32
#define CP_TURN_RATE 0.6f /* radians per second */

static int    s_glReady;
static GLuint s_meshProg, s_quadProg;
static GLint  s_locMvp, s_locMv, s_locLight, s_locQuadTex;
static GLuint s_meshVao, s_meshVbo, s_quadVao, s_quadVbo;
static GLuint s_fbo, s_fboTex, s_fboDepth;

static const float* s_stream;
static int          s_corners;
static void*        s_model;
static char         s_modelKey[128];
static float        s_bounds[6];
static int          s_matCount;
static PcSkinPreviewMat s_mats[CP_MAX_MATS];
static float        s_yaw;
static Uint32       s_lastMs;

static GLuint cp_make_shader(GLenum type, const char* src)
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
        SH_DBG("[CHARAPREV] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint cp_make_program(const char* vs_src, const char* fs_src, const char* a0, const char* a1, const char* a2)
{
    GLuint vs = cp_make_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = cp_make_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog;
    GLint  ok = 0;

    if (!vs || !fs)
        return 0;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, a0);
    glBindAttribLocation(prog, 1, a1);
    if (a2 != NULL)
        glBindAttribLocation(prog, 2, a2);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[CHARAPREV] link failed: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void cp_gl_init(void)
{
    static const char* mesh_vs =
        "attribute vec3 a_pos;\n"
        "attribute vec3 a_nrm;\n"
        "attribute vec2 a_uv;\n"
        "uniform mat4 u_mvp;\n"
        "uniform mat4 u_mv;\n"
        "varying vec2 v_uv;\n"
        "varying vec3 v_nrm;\n"
        "void main() {\n"
        "  v_uv = a_uv;\n"
        /* No mat3() cast: the legacy dialect (GLSL 110 on desktop) has none. */
        "  v_nrm = u_mv[0].xyz * a_nrm.x + u_mv[1].xyz * a_nrm.y + u_mv[2].xyz * a_nrm.z;\n"
        "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "}\n";
    static const char* mesh_fs =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "varying vec3 v_nrm;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec3 u_light;\n"
        "void main() {\n"
        "  vec4 c = texture2D(u_tex, v_uv);\n"
        "  if (c.a < 0.5) discard;\n"
        "  float d = abs(dot(normalize(v_nrm), u_light));\n"
        "  gl_FragColor = vec4(c.rgb * (0.55 + 0.45 * d), 1.0);\n"
        "}\n";
    static const char* quad_vs =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* quad_fs =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevFbo = 0, prevTex = 0, prevRb = 0;
    GLenum status;

    s_glReady = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);

    s_meshProg = cp_make_program(mesh_vs, mesh_fs, "a_pos", "a_nrm", "a_uv");
    s_quadProg = cp_make_program(quad_vs, quad_fs, "a_pos", "a_uv", NULL);
    if (!s_meshProg || !s_quadProg)
        goto restore;
    /* PsyCross caches the current program: whatever is bound on return must
     * be what it believes is bound. */
    glUseProgram(s_meshProg);
    glUniform1i(glGetUniformLocation(s_meshProg, "u_tex"), 0);
    s_locMvp   = glGetUniformLocation(s_meshProg, "u_mvp");
    s_locMv    = glGetUniformLocation(s_meshProg, "u_mv");
    s_locLight = glGetUniformLocation(s_meshProg, "u_light");
    glUseProgram(s_quadProg);
    s_locQuadTex = glGetUniformLocation(s_quadProg, "u_tex");
    glUniform1i(s_locQuadTex, 0);

    glGenVertexArrays(1, &s_meshVao);
    glBindVertexArray(s_meshVao);
    glGenBuffers(1, &s_meshVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_meshVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));

    glGenVertexArrays(1, &s_quadVao);
    glBindVertexArray(s_quadVao);
    glGenBuffers(1, &s_quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glGenTextures(1, &s_fboTex);
    glBindTexture(GL_TEXTURE_2D, s_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CP_FBO_W, CP_FBO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &s_fboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_fboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, CP_FBO_W, CP_FBO_H);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fboTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_fboDepth);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        SH_DBG("[CHARAPREV] framebuffer incomplete: 0x%x", (unsigned)status);
        goto restore;
    }
    s_glReady = 1;

restore:
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prevRb);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}

/* Column-major 4x4 helpers. */
static void m4_identity(float* m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(const float* a, const float* b, float* o)
{
    float r[16];
    int   i, j, k;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + j] * b[i * 4 + k];
            r[i * 4 + j] = s;
        }
    memcpy(o, r, sizeof(r));
}

/* The model for the current pick, reloaded only when the pick changes. */
static int cp_model_sync(void)
{
    const char* file;
    const char* target = Pc_CharaSelect_PreviewTarget(&file);
    char        key[128];
    const float* stream = NULL;
    int         corners = 0;

    if (target == NULL || file == NULL)
    {
        s_model = NULL;
        s_modelKey[0] = '\0';
        return 0;
    }
    snprintf(key, sizeof(key), "%s|%s", target, file);
    if (strcmp(key, s_modelKey) != 0)
    {
        snprintf(s_modelKey, sizeof(s_modelKey), "%s", key);
        s_model  = Pc_SkinChara_PreviewLoad(target, file);
        s_stream = NULL;
        s_corners = 0;
        s_matCount = 0;
        if (s_model == NULL)
            SH_DBG("[CHARAPREV] no preview for %s on %s", file, target);
    }
    if (s_model == NULL)
        return 0;
    s_matCount = Pc_SkinChara_PreviewMesh(s_model, &stream, &corners, s_mats, CP_MAX_MATS, s_bounds);
    if (s_matCount == 0 || corners == 0)
        return 0;
    if (stream != s_stream || corners != s_corners)
    {
        GLint prevVao = 0, prevBuf = 0;

        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        glBindVertexArray(s_meshVao);
        glBindBuffer(GL_ARRAY_BUFFER, s_meshVbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)corners * 8 * sizeof(float), stream, GL_STATIC_DRAW);
        glBindVertexArray((GLuint)prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
        s_stream  = stream;
        s_corners = corners;
    }
    return 1;
}

void Pc_CharaPreview_Draw(void)
{
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = 0, prevFbo = 0;
    GLint     prevVp[4] = { 0, 0, 0, 0 };
    GLint     prevSrcRgb = GL_SRC_ALPHA, prevDstRgb = GL_ONE_MINUS_SRC_ALPHA, prevSrcA = GL_ONE, prevDstA = GL_ONE_MINUS_SRC_ALPHA;
    GLint     prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull, prevScissor;
    GLboolean prevDepthMask = GL_TRUE;
    int       vp[4], rx0, ry0, rx1, ry1, i;
    Uint32    now;
    float     mv[16], proj[16], mvp[16], rot[16], flip[16], trans[16];
    float     cx, cy, cz, height, dist, f, aspect, cs, sn, light[3];

    if (!Pc_CharaSelect_IsOpen())
    {
        s_lastMs = 0;
        return;
    }
    if (!s_glReady) cp_gl_init();
    if (s_glReady != 1) return;
    if (!cp_model_sync())
        return;

    now = SDL_GetTicks();
    if (s_lastMs != 0)
        s_yaw += CP_TURN_RATE * (float)(now - s_lastMs) / 1000.0f;
    s_lastMs = now;
    if (s_yaw > 6.2831853f) s_yaw -= 6.2831853f;

    PsyX_GetDisplayViewport(&vp[0], &vp[1], &vp[2], &vp[3]);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
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
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    /* The model into its own framebuffer. Game space is y-down, +z toward the
     * viewer: the view flips y and looks down -z from in front. */
    cx = 0.5f * (s_bounds[0] + s_bounds[3]);
    cy = 0.5f * (s_bounds[1] + s_bounds[4]);
    cz = 0.5f * (s_bounds[2] + s_bounds[5]);
    height = s_bounds[4] - s_bounds[1];
    if (height < 1.0f) height = 1.0f;
    aspect = (float)CP_FBO_W / (float)CP_FBO_H;
    f      = 1.0f / tanf(0.5f * 0.42f); /* ~24 degree vertical field */
    dist   = 0.5f * height * f * 1.15f;

    m4_identity(trans);
    trans[12] = -cx; trans[13] = -cy; trans[14] = -cz;
    cs = cosf(s_yaw); sn = sinf(s_yaw);
    m4_identity(rot);
    rot[0] = cs;  rot[2] = sn;
    rot[8] = -sn; rot[10] = cs;
    m4_identity(flip);
    flip[5]  = -1.0f;
    flip[14] = -dist;
    m4_mul(rot, trans, mv);
    m4_mul(flip, mv, mv);
    memset(proj, 0, sizeof(proj));
    proj[0]  = f / aspect;
    proj[5]  = f;
    {
        float zn = dist - height, zf = dist + height;
        if (zn < 1.0f) zn = 1.0f;
        proj[10] = -(zf + zn) / (zf - zn);
        proj[14] = -2.0f * zf * zn / (zf - zn);
        proj[11] = -1.0f;
    }
    m4_mul(proj, mv, mvp);
    light[0] = 0.35f; light[1] = 0.45f; light[2] = 0.82f;

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, CP_FBO_W, CP_FBO_H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    /* The desktop 3.3 context may not export the float form; the default
     * clear depth is 1.0 either way. */
    if (glad_glClearDepthf != NULL)
        glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(s_meshProg);
    glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(s_locMv, 1, GL_FALSE, mv);
    glUniform3fv(s_locLight, 1, light);
    glBindVertexArray(s_meshVao);
    glBindBuffer(GL_ARRAY_BUFFER, s_meshVbo);
    for (i = 0; i < s_matCount; i++)
    {
        if (s_mats[i].texture == 0 || s_mats[i].count == 0)
            continue;
        glBindTexture(GL_TEXTURE_2D, s_mats[i].texture);
        glDrawArrays(GL_TRIANGLES, s_mats[i].first, s_mats[i].count);
    }

    /* Composite into the screen's frame. The menu space is 320x224 over the
     * presented picture. */
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    Pc_CharaSelect_PreviewRect(&rx0, &ry0, &rx1, &ry1);
    {
        /* The menu field is -m..320+m columns by 224 rows over the picture. */
        float m  = g_PsxWideMenu ? PsyX_WideMenuMargin() : 0.0f;
        float fw = 320.0f + 2.0f * m;
        float x0 = ((float)rx0 + m) / fw * 2.0f - 1.0f;
        float x1 = ((float)rx1 + m) / fw * 2.0f - 1.0f;
        float y0 = 1.0f - (float)ry0 / 224.0f * 2.0f;
        float y1 = 1.0f - (float)ry1 / 224.0f * 2.0f;
        /* Keep the model's proportions: fit the framebuffer's aspect inside
         * the frame, measured in window pixels. */
        float pw = (x1 - x0) * 0.5f * (float)vp[2];
        float ph = (y0 - y1) * 0.5f * (float)vp[3];
        float fa = (float)CP_FBO_W / (float)CP_FBO_H;
        float q[24];

        if (pw > ph * fa)
        {
            float nw = ph * fa / (float)vp[2] * 2.0f, cx = 0.5f * (x0 + x1);
            x0 = cx - 0.5f * nw;
            x1 = cx + 0.5f * nw;
        }
        else if (ph > pw / fa)
        {
            float nh = pw / fa / (float)vp[3] * 2.0f, cy = 0.5f * (y0 + y1);
            y1 = cy - 0.5f * nh;
            y0 = cy + 0.5f * nh;
        }
        {
            float qq[24] = { x0, y1, 0, 0,  x1, y1, 1, 0,  x1, y0, 1, 1,
                             x0, y1, 0, 0,  x1, y0, 1, 1,  x0, y0, 0, 1 };
            memcpy(q, qq, sizeof(q));
        }

        glUseProgram(s_quadProg);
        glBindVertexArray(s_quadVao);
        glBindBuffer(GL_ARRAY_BUFFER, s_quadVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(q), q);
        glBindTexture(GL_TEXTURE_2D, s_fboTex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    glDepthMask(prevDepthMask);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
