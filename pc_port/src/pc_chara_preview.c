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
#include "pc_ilm_preview.h"
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
static int          s_showing;
/* A model not yet in memory is loaded only once its row has stayed
 * highlighted this long: the mouse sweeps over a dozen rows in a moment, and
 * loading each one it touched hitched the menu. Models already held show at
 * once, as does the first one after the screen opens. */
#define CP_DWELL_MS 500
static char         s_wantKey[128];
static Uint32       s_wantSince;
static int          s_pending;
static int          s_opened;

/* ---- a clip of the character's own animation, played on a click ---------- */

extern s_AnimInfo ALESSA_ANIM_INFOS[22], BLOODY_LISA_ANIM_INFOS[4], DAHLIA_ANIM_INFOS[52], KAUFMANN_ANIM_INFOS[46],
                  LISA_ANIM_INFOS[36], MONSTER_CYBIL_ANIM_INFOS[44], GHOST_CHILD_ALESSA_ANIM_INFOS[18],
                  GHOST_DOCTOR_ANIM_INFOS[4], INCUBATOR_ANIM_INFOS[8], CHERYL_ANIM_INFOS[], CYBIL_ANIM_INFOS[];
extern const int  CHERYL_ANIM_INFO_COUNT, CYBIL_ANIM_INFO_COUNT;
extern s_AnimInfo HARRY_BASE_ANIM_INFOS[];

#define CP_ANM_BONES 18
#define CP_ANM_MAX   64

static unsigned char* s_anm;
static int            s_anmSize;
static char           s_anmSlot[16];
static int            s_clipK0, s_clipK1; /* keyframes, inclusive */
static float          s_clipRate;         /* keyframes per second */
static float          s_clipT;            /* keyframes into the clip */
static int            s_clipOn;

/* The slot's animation file and its table of clips. */
static int cp_anim_set(const char* slot, int* fileIdx, const s_AnimInfo** infos, int* count)
{
    static const struct
    {
        const char* slot;
        int         charaId;
    } ids[] = {
        { "HERO", Chara_Harry }, { "SIBYL", Chara_Cybil }, { "MSB", Chara_MonsterCybil }, { "LISA", Chara_Lisa },
        { "BLISA", Chara_BloodyLisa }, { "DARIA", Chara_Dahlia }, { "KAU", Chara_Kaufmann }, { "SRL", Chara_Cheryl },
        { "AR", Chara_Alessa }, { "TAR", Chara_GhostChildAlessa }, { "MAR", Chara_Incubator }, { "DOC", Chara_GhostDoctor },
    };
    int i;

    for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++)
    {
        if (strcmp(ids[i].slot, slot) != 0)
            continue;
        *fileIdx = CHARA_FILE_INFOS[ids[i].charaId].animFileIdx;
        switch (ids[i].charaId)
        {
            case Chara_Harry:            *infos = HARRY_BASE_ANIM_INFOS;         *count = 57; break;
            case Chara_Cybil:            *infos = CYBIL_ANIM_INFOS;              *count = CYBIL_ANIM_INFO_COUNT; break;
            case Chara_MonsterCybil:     *infos = MONSTER_CYBIL_ANIM_INFOS;      *count = 44; break;
            case Chara_Lisa:             *infos = LISA_ANIM_INFOS;               *count = 36; break;
            case Chara_BloodyLisa:       *infos = BLOODY_LISA_ANIM_INFOS;        *count = 4; break;
            case Chara_Dahlia:           *infos = DAHLIA_ANIM_INFOS;             *count = 52; break;
            case Chara_Kaufmann:         *infos = KAUFMANN_ANIM_INFOS;           *count = 46; break;
            case Chara_Cheryl:           *infos = CHERYL_ANIM_INFOS;             *count = CHERYL_ANIM_INFO_COUNT; break;
            case Chara_Alessa:           *infos = ALESSA_ANIM_INFOS;             *count = 22; break;
            case Chara_GhostChildAlessa: *infos = GHOST_CHILD_ALESSA_ANIM_INFOS; *count = 18; break;
            case Chara_Incubator:        *infos = INCUBATOR_ANIM_INFOS;          *count = 8; break;
            default:                     *infos = GHOST_DOCTOR_ANIM_INFOS;       *count = 4; break;
        }
        return *fileIdx > 0;
    }
    return 0;
}

static int cp_anm_valid(const unsigned char* a, int size)
{
    int dataOff, kfSize, bones, kfCount;

    if (a == NULL || size < 0x14)
        return 0;
    dataOff = a[0] | (a[1] << 8);
    kfSize  = a[4] | (a[5] << 8);
    bones   = a[6];
    kfCount = a[0x10] | (a[0x11] << 8);
    return bones >= CP_ANM_BONES && bones <= CP_ANM_MAX && a[0x12] < 16 && a[2] * 9 + a[3] * 3 == kfSize && kfSize > 0 &&
           0x14 + bones * 6 <= dataOff && dataOff + kfSize * kfCount <= size;
}

/* The bones at a point between two keyframes, as Anim_BoneUpdate blends them
 * and the bone hierarchy stacks them: what the game would draw. */
static void cp_anm_pose(const unsigned char* a, int k0, int k1, float alpha, float (*rot)[9], float (*pos)[3])
{
    static float wr[CP_ANM_MAX][9], wt[CP_ANM_MAX][3];
    const int    dataOff = a[0] | (a[1] << 8), transCount = a[3], kfSize = a[4] | (a[5] << 8), bones = a[6];
    const float  scale = (float)(1 << a[0x12]);
    const signed char* f0 = (const signed char*)a + dataOff + kfSize * k0;
    const signed char* f1 = (const signed char*)a + dataOff + kfSize * k1;
    int          i, k;

    memset(wr[0], 0, sizeof(wr[0]));
    memset(wt[0], 0, sizeof(wt[0]));
    wr[0][0] = wr[0][4] = wr[0][8] = 1.0f;
    for (i = 1; i < bones; i++)
    {
        const signed char* b = (const signed char*)a + 0x14 + i * 6;
        int                parent = b[0], rIdx = b[1], tIdx = b[2];
        float              lr[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, lt[3];

        for (k = 0; k < 3; k++)
            lt[k] = (float)b[3 + k] * scale;
        if (tIdx >= 0)
        {
            for (k = 0; k < 3; k++)
                lt[k] = ((float)f0[tIdx * 3 + k] + (float)(f1[tIdx * 3 + k] - f0[tIdx * 3 + k]) * alpha) * scale;
            if (tIdx == 0)
                lt[1] -= (float)a[0x13];
        }
        if (rIdx >= 0)
            for (k = 0; k < 9; k++)
            {
                const int o = transCount * 3 + rIdx * 9 + k;
                lr[k] = ((float)f0[o] + (float)(f1[o] - f0[o]) * alpha) / 128.0f;
            }
        /* Parents come first in every character's table. */
        if (parent < 0 || parent >= i)
            parent = 0;
        {
            const float* pr = wr[parent];
            int          r, c;

            for (r = 0; r < 3; r++)
            {
                for (c = 0; c < 3; c++)
                    wr[i][r * 3 + c] = pr[r * 3] * lr[c] + pr[r * 3 + 1] * lr[3 + c] + pr[r * 3 + 2] * lr[6 + c];
                wt[i][r] = pr[r * 3] * lt[0] + pr[r * 3 + 1] * lt[1] + pr[r * 3 + 2] * lt[2] + wt[parent][r];
            }
        }
    }
    memcpy(rot, wr, sizeof(float) * 9 * CP_ANM_BONES);
    memcpy(pos, wt, sizeof(float) * 3 * CP_ANM_BONES);
}

static void cp_anim_stop(void)
{
    if (s_clipOn)
        Pc_SkinChara_PreviewPose(NULL, NULL);
    s_clipOn = 0;
}

/* A click: one of the slot's clips, picked at random, played once through. */
static void cp_anim_start(const char* slot)
{
    const s_AnimInfo* infos = NULL;
    int               fileIdx = 0, count = 0, i, n = 0, pick, kfCount;

    if (slot == NULL || !cp_anim_set(slot, &fileIdx, &infos, &count))
        return;
    if (strcmp(slot, s_anmSlot) != 0 || s_anm == NULL)
    {
        free(s_anm);
        s_anm = Pc_IlmPreview_ReadFile(fileIdx, &s_anmSize);
        snprintf(s_anmSlot, sizeof(s_anmSlot), "%s", slot);
        if (!cp_anm_valid(s_anm, s_anmSize))
        {
            free(s_anm);
            s_anm = NULL;
        }
    }
    if (s_anm == NULL)
        return;
    kfCount = s_anm[0x10] | (s_anm[0x11] << 8);
    for (i = 0; i < count; i++)
        if (infos[i].startKeyframeIdx >= 0 && infos[i].endKeyframeIdx >= infos[i].startKeyframeIdx + 4 &&
            infos[i].endKeyframeIdx < kfCount)
            n++;
    if (n == 0)
        return;
    pick = (int)(SDL_GetTicks() * 2654435761u >> 8) % n;
    for (i = 0; i < count; i++)
        if (infos[i].startKeyframeIdx >= 0 && infos[i].endKeyframeIdx >= infos[i].startKeyframeIdx + 4 &&
            infos[i].endKeyframeIdx < kfCount && pick-- == 0)
        {
            s_clipK0   = infos[i].startKeyframeIdx;
            s_clipK1   = infos[i].endKeyframeIdx;
            /* Keyframes a second, where the table states it; some of Harry's
             * are worked out by a function as he moves. */
            s_clipRate = infos[i].hasVariableDuration ? 15.0f : (float)infos[i].duration.constant / 4096.0f;
            if (s_clipRate < 4.0f)  s_clipRate = 4.0f;
            if (s_clipRate > 30.0f) s_clipRate = 30.0f;
            s_clipT  = 0.0f;
            s_clipOn = 1;
            return;
        }
}

/* Advances the clip and hands the loader its pose; the rest pose again when
 * it ends. */
static void cp_anim_tick(float dt)
{
    static float rot[CP_ANM_BONES][9], pos[CP_ANM_BONES][3];
    int          k;

    if (!s_clipOn || s_anm == NULL)
        return;
    s_clipT += dt * s_clipRate;
    if (s_clipT >= (float)(s_clipK1 - s_clipK0))
    {
        cp_anim_stop();
        return;
    }
    k = (int)s_clipT;
    cp_anm_pose(s_anm, s_clipK0 + k, s_clipK0 + k + 1, s_clipT - (float)k, rot, pos);
    Pc_SkinChara_PreviewPose((const float (*)[9])rot, (const float (*)[3])pos);
}

static int cp_dwell_ok(const char* key, int cached)
{
    Uint32 now = SDL_GetTicks();

    if (strcmp(key, s_wantKey) != 0)
    {
        snprintf(s_wantKey, sizeof(s_wantKey), "%s", key);
        s_wantSince = now;
    }
    if (cached || !s_opened || now - s_wantSince >= CP_DWELL_MS)
    {
        s_opened = 1;
        return 1;
    }
    s_pending = 1;
    return 0;
}
/* The stock model when no swap is chosen: built off the disc once per pick. */
static IlmPreview   s_ilm;
static int          s_ilmId = -1;
static GLuint       s_ilmTex[IP_MAX_MAT];
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
static void cp_ilm_release(void)
{
    int i;

    for (i = 0; i < IP_MAX_MAT; i++)
        if (s_ilmTex[i] != 0)
        {
            glDeleteTextures(1, &s_ilmTex[i]);
            s_ilmTex[i] = 0;
        }
    Pc_IlmPreview_Free(&s_ilm);
    s_ilmId = -1;
}

static void cp_upload_stream(const float* stream, int corners)
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

/* No swap file chosen: the stock body the row would draw, read off the disc. */
static int cp_stock_sync(void)
{
    int id = Pc_CharaSelect_PreviewStockId();
    int i;

    s_model = NULL;
    s_modelKey[0] = '\0';
    if (id <= 0)
        return 0;
    {
        char key[32];

        snprintf(key, sizeof(key), "stock|%d", id);
        if (!cp_dwell_ok(key, id == s_ilmId))
            return 0;
    }
    if (id != s_ilmId)
    {
        GLint prevTex = 0;

        cp_ilm_release();
        s_ilmId = id;
        s_stream = NULL;
        s_corners = 0;
        if (!Pc_IlmPreview_Build(id, &s_ilm))
            return 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        for (i = 0; i < s_ilm.matCount; i++)
        {
            if (s_ilm.rgba[i] == NULL)
                continue;
            glGenTextures(1, &s_ilmTex[i]);
            glBindTexture(GL_TEXTURE_2D, s_ilmTex[i]);
            /* PSX art: point-sampled, as the game draws it. */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_ilm.texW, s_ilm.texH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         s_ilm.rgba[i]);
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (s_ilm.stream == NULL || s_ilm.corners == 0)
        return 0;
    s_matCount = s_ilm.matCount;
    for (i = 0; i < s_matCount; i++)
    {
        s_mats[i].texture = s_ilmTex[i];
        s_mats[i].first   = s_ilm.mats[i].first;
        s_mats[i].count   = s_ilm.mats[i].count;
    }
    memcpy(s_bounds, s_ilm.bounds, sizeof(s_bounds));
    if (s_stream != s_ilm.stream || s_corners != s_ilm.corners)
        cp_upload_stream(s_ilm.stream, s_ilm.corners);
    return 1;
}

static int cp_model_sync(void)
{
    const char* file;
    const char* target = Pc_CharaSelect_PreviewTarget(&file);
    char        key[128];
    const float* stream = NULL;
    int         corners = 0;

    if (target != NULL && file == NULL)
        return cp_stock_sync();
    if (target == NULL)
    {
        s_model = NULL;
        s_modelKey[0] = '\0';
        s_wantKey[0] = '\0';
        return 0;
    }
    snprintf(key, sizeof(key), "%s|%s", target, file);
    {
        static int s_gen;

        if (Pc_SkinChara_FilesGen() != s_gen)
        {
            s_gen         = Pc_SkinChara_FilesGen();
            s_model       = NULL;
            s_modelKey[0] = 0;
            s_stream      = NULL;
        }
    }
    if (!cp_dwell_ok(key, strcmp(key, s_modelKey) == 0 || Pc_SkinChara_PreviewCached(target, file)))
        return 0;
    if (strcmp(key, s_modelKey) != 0)
    {
        snprintf(s_modelKey, sizeof(s_modelKey), "%s", key);
        cp_anim_stop();
        s_model  = Pc_SkinChara_PreviewLoad(target, file);
        s_stream = NULL;
        s_corners = 0;
        s_matCount = 0;
        if (s_model == NULL)
            SH_DBG("[CHARAPREV] no preview for %s on %s", file, target);
    }
    if (s_model == NULL)
        return 0;
    {
        /* The view stays framed on the standing model while a clip moves it. */
        float       moving[6];
        const int   was = s_clipOn;
        static int  s_posedLast;

        s_matCount = Pc_SkinChara_PreviewMesh(s_model, &stream, &corners, s_mats, CP_MAX_MATS, s_clipOn ? moving : s_bounds);
        if (s_matCount == 0 || corners == 0)
            return 0;
        if (stream != s_stream || corners != s_corners || was || s_posedLast)
            cp_upload_stream(stream, corners);
        s_posedLast = was;
    }
    return 1;
}

int Pc_CharaPreview_Showing(void)
{
    return s_showing;
}

int Pc_CharaPreview_Pending(void)
{
    return s_pending;
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

    s_showing = 0;
    s_pending = 0;
    if (!Pc_CharaSelect_IsOpen())
    {
        s_lastMs = 0;
        s_opened = 0;
        s_wantKey[0] = '\0';
        cp_anim_stop();
        return;
    }
    if (!s_glReady) cp_gl_init();
    if (s_glReady != 1) return;
    now = SDL_GetTicks();
    cp_anim_tick(s_lastMs != 0 ? (float)(now - s_lastMs) / 1000.0f : 0.0f);
    if (!cp_model_sync())
    {
        s_lastMs = now;
        return;
    }
    s_showing = 1;

    PsyX_GetDisplayViewport(&vp[0], &vp[1], &vp[2], &vp[3]);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;

    /* The mouse on the model: held and moved it turns it by hand, a plain
     * click plays one of its animations. It turns by itself again a moment
     * after being let go. One preview serves every page, so this does too. */
    {
        static int    s_drag, s_wasDown, s_lastX, s_moved;
        static Uint32 s_handMs;
        int           mx = 0, my = 0, ww = 0, wh = 0, px0, px1, py0, py1, inside;
        const int     down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        SDL_Window*   win = SDL_GetMouseFocus();
        const float   m  = g_PsxWideMenu ? PsyX_WideMenuMargin() : 0.0f;
        const float   fw = 320.0f + 2.0f * m;

        Pc_CharaSelect_PreviewRect(&rx0, &ry0, &rx1, &ry1);
        if (win != NULL)
            SDL_GetWindowSize(win, &ww, &wh);
        if (ww > 0 && wh > 0)
        {
            /* Window points to drawable pixels, then the menu's 320x224 field. */
            int dw = ww, dh = wh;

            SDL_GL_GetDrawableSize(win, &dw, &dh);
            mx = mx * dw / ww;
            my = dh - 1 - my * dh / wh;
        }
        px0    = vp[0] + (int)(((float)rx0 + m) / fw * (float)vp[2]);
        px1    = vp[0] + (int)(((float)rx1 + m) / fw * (float)vp[2]);
        py1    = vp[1] + (int)((1.0f - (float)ry0 / 224.0f) * (float)vp[3]);
        py0    = vp[1] + (int)((1.0f - (float)ry1 / 224.0f) * (float)vp[3]);
        inside = win != NULL && mx >= px0 && mx < px1 && my >= py0 && my < py1;
        if (down && !s_wasDown && inside)
        {
            s_drag  = 1;
            s_lastX = mx;
            s_moved = 0;
        }
        if (s_drag && down)
        {
            s_yaw   += (float)(mx - s_lastX) * 6.2831853f / (float)(px1 - px0 > 0 ? (px1 - px0) * 2 : 1);
            s_moved += abs(mx - s_lastX);
            s_lastX  = mx;
            s_handMs = now;
        }
        if (s_drag && !down)
        {
            s_drag = 0;
            if (s_moved < 4 && s_model != NULL)
            {
                const char* file;
                cp_anim_start(Pc_CharaSelect_PreviewTarget(&file));
            }
        }
        s_wasDown = down;
        if (s_lastMs != 0 && !s_drag && now - s_handMs > 1500)
            s_yaw += CP_TURN_RATE * (float)(now - s_lastMs) / 1000.0f;
    }
    s_lastMs = now;
    while (s_yaw > 6.2831853f) s_yaw -= 6.2831853f;
    while (s_yaw < 0.0f) s_yaw += 6.2831853f;

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
     * clear depth is 1.0 either way. A GLES build links glClearDepthf
     * directly and has no glad pointer to test (libgs_stub.c, same split). */
#if defined(RENDERER_OGLES) || defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(SH_IOS)
    glClearDepthf(1.0f);
#else
    if (glad_glClearDepthf != NULL)
        glClearDepthf(1.0f);
#endif
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
