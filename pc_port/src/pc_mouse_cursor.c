/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC port: mouse control for cursor-driven puzzles + a clickable main menu.
 * See pc_mouse_cursor.h for the design. */
#include "game.h"
#include "inline_no_dmpsx.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h"

#include "bodyprog/screen/screen_data.h"

#include <SDL.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h> /* GR_ReadVRAM */
#include <PsyX/PsyX_backend.h>
#include <math.h>
#include "sh_log.h"
#include "pc_confirm_dialog.h"
#include "pc_bind_panel.h"
#include "pc_quick_options.h"
#include <libgs.h>

extern GLuint GR_ScreenFBO(void);

/* Menu rows are authored via Gfx_StringSetPosition(x, y), a center-origin space
 * whose reference is (OFFSET_X, OFFSET_Y). The overlay OT draw origin is the
 * framebuffer centre (gsScreenWidth/2, gsScreenHeight/2). The framebuffer HEIGHT
 * is 448 on the interlaced title/difficulty menus and 224 in-game, so we read it
 * live (g_GameWork.gsScreenHeight) — a fixed 240/120 was the bug that trapped the
 * cursor in the middle band. Width/OFFSET_X are 320/160 in both modes. */
#define MC_OFFSET_X 160 /* SCREEN_POSITION_X(50) — text authoring X reference */
#define MC_OFFSET_Y 112 /* SCREEN_POSITION_Y(47) — text authoring Y reference */

/* Per-frame pointer state, in logical 320x240 coords. */
static float s_gx, s_gy, s_prevGx, s_prevGy;
static float s_vx, s_vy;   /* normalized 0..1 inside the presented picture */
static int   s_inView;
static int   s_leftDown, s_prevLeft, s_leftEdge;
static int   s_rightDown, s_prevRight, s_rightEdge;
static int   s_puzzleFrames; /* >0 while a free-cursor puzzle is on screen */
static int   s_havePrev;
static int   s_moved;        /* mouse moved this frame */
static int   s_prevMx = -1, s_prevMy = -1;

/* The mouse owns the puzzle cursor from a mouse move until a real stick deflects,
 * so an idle mouse never pins the cursor against a pad. */
static int   s_puzzleMouseOwns;
/* Where the override put the cursor this frame (framebuffer centre-origin px),
 * checked against where the puzzle actually drew it after its clamp. */
static float s_puzzleTargetX, s_puzzleTargetY;
static int   s_puzzleOverrode;
static int   s_wheelStep;    /* +1/-1 on a new wheel notch this frame, else 0 */
static int   s_wheelNotches; /* every notch this frame, signed */
static int   s_prevWheelTotal;
static int   s_prevWheelUp, s_prevWheelDown;
static int   s_showReq;     /* a screen asked for the pointer since the last draw */

static GLuint s_glProg, s_glVao, s_glVbo, s_glTex;
static int    s_glState; /* 0 untried, 1 ready, -1 failed */

static int Mc_Enabled(void)
{
    if (!g_PcConfig.mouseCursor)
        return 0;
    /* Only when the OS cursor is free. In TPS/OTS/FPS the pointer is captured
     * for camera look, so its absolute position is meaningless. */
    if (SDL_GetRelativeMouseMode() == SDL_TRUE)
        return 0;
    /* The quake console owns the mouse while open (its own pointer, click-drag
     * selection) — no hover/click may leak into the menu underneath. */
    {
        extern int Pc_ConsoleIsOpen(void);
        if (Pc_ConsoleIsOpen())
            return 0;
    }
    return 1;
}

/* Pulls the OS pointer back onto the puzzle cursor when the puzzle's own clamp
 * held the cursor short of where the override put it. Otherwise the pointer
 * wanders past the puzzle's bounds and has to travel all the way back before
 * the cursor leaves the edge. The puzzle bounds stay the game's; only the
 * pointer is confined to them. */
static void Mc_PinPointerToPuzzleCursor(float tipX, float tipY)
{
    extern SDL_Window* g_window;
    float              ex = tipX - s_puzzleTargetX;
    float              ey = tipY - s_puzzleTargetY;
    float              fx0, fy0, fx1, fy1;

    if (ex > -1.0f && ex < 1.0f && ey > -1.0f && ey < 1.0f)
        return;
    if (g_window == NULL || SDL_GetKeyboardFocus() != g_window)
        return;

    /* Window px per framebuffer px, sampled from the forward mapping so the
     * pillarbox / internal-resolution handling there needs no inverse here. */
    PsyX_MapWindowToViewport(s_prevMx, s_prevMy, &fx0, &fy0);
    PsyX_MapWindowToViewport(s_prevMx + 1024, s_prevMy + 1024, &fx1, &fy1);
    if (fx1 <= fx0 || fy1 <= fy0)
        return;

    SDL_WarpMouseInWindow(g_window,
                          s_prevMx + (int)(ex / (float)g_GameWork.gsScreenWidth * 1024.0f / (fx1 - fx0)),
                          s_prevMy + (int)(ey / (float)g_GameWork.gsScreenHeight * 1024.0f / (fy1 - fy0)));
}

void Pc_MouseCursor_NoteCursorDrawn(int curX, int curY)
{
    s_puzzleFrames = 3;

    if (s_puzzleOverrode)
    {
        s_puzzleOverrode = 0;
        /* The arrow is drawn centred 8 px right/down of its tip. */
        Mc_PinPointerToPuzzleCursor((float)(curX - 8), (float)(curY - 8));
    }
}

void Pc_MouseCursor_PuzzleOverride(int* cursorX, int* cursorY)
{
    if (!Mc_Enabled() || !s_puzzleMouseOwns)
        return;

    /* s_gx/s_gy are text-authoring coords; minus the OFFSET reference they are
     * framebuffer centre-origin px, the space the puzzles keep their cursor in
     * (the arrow tip sits exactly on FP_FROM(cursor)). Off-picture positions
     * are passed through so the puzzle's own clamp pins the cursor to its edge. */
    s_puzzleTargetX  = s_gx - (float)MC_OFFSET_X;
    s_puzzleTargetY  = s_gy - (float)MC_OFFSET_Y;
    s_puzzleOverrode = 1;
    *cursorX = (int)(s_puzzleTargetX * 4096.0f);
    *cursorY = (int)(s_puzzleTargetY * 4096.0f);
}

int Pc_MouseCursor_Moved(void)
{
    return s_moved;
}

int Pc_MouseCursor_UiPos(int* outX, int* outY)
{
    if (!Mc_Enabled() || !s_inView)
        return 0;
    if (outX != NULL)
        *outX = (int)s_gx;
    if (outY != NULL)
        *outY = (int)s_gy;
    return 1;
}

/* Normalized position inside the presented picture. Anything drawing in the
 * post-capture GL hook wants THIS, not UiPos: the authoring space UiPos reports
 * in is the live framebuffer (448 tall for the interlaced menus, 224 in game,
 * and different again on PAL), so reconstructing viewport pixels from it needs
 * knowledge this side does not have. */
int Pc_MouseCursor_ViewportPos(float* outX, float* outY)
{
    if (!Mc_Enabled() || !s_inView)
        return 0;
    if (outX != NULL) *outX = s_vx;
    if (outY != NULL) *outY = s_vy;
    return 1;
}

int Pc_MouseCursor_LeftClicked(void)
{
    return Mc_Enabled() && s_inView && s_leftEdge;
}

int Pc_MouseCursor_LeftHeld(void)
{
    return Mc_Enabled() && s_inView && s_leftDown;
}

int Pc_MouseCursor_RightClicked(void)
{
    return Mc_Enabled() && s_inView && s_rightEdge;
}

int Pc_MouseCursor_WheelStep(void)
{
    return Mc_Enabled() ? s_wheelStep : 0;
}

int Pc_MouseCursor_WheelNotches(void)
{
    return Mc_Enabled() ? s_wheelNotches : 0;
}

unsigned int Pc_MouseCursor_BoundPadBits(void)
{
    extern unsigned short g_cfg_mouseButtonMask[8];
    extern int            g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
    const Uint32          mb   = SDL_GetMouseState(NULL, NULL);
    unsigned int          bits = 0;
    int                   b;

    for (b = 1; b <= 5; b++)
    {
        if (mb & SDL_BUTTON(b))
            bits |= g_cfg_mouseButtonMask[b];
    }
    if (g_PsyX_WheelUpFrames > 0)   bits |= g_cfg_mouseButtonMask[6];
    if (g_PsyX_WheelDownFrames > 0) bits |= g_cfg_mouseButtonMask[7];
    if (bits & ControllerFlag_DpadUp)    bits |= ControllerFlag_LStickUp;
    if (bits & ControllerFlag_DpadDown)  bits |= ControllerFlag_LStickDown;
    if (bits & ControllerFlag_DpadLeft)  bits |= ControllerFlag_LStickLeft;
    if (bits & ControllerFlag_DpadRight) bits |= ControllerFlag_LStickRight;
    return bits;
}

int Pc_MouseCursor_PuzzleActive(void)
{
    return s_puzzleFrames > 0;
}

void Pc_MouseCursor_FrameUpdate(void)
{
    int    mx, my;
    Uint32 btn;
    float  fx, fy;

    s_leftEdge = s_rightEdge = 0;
    s_puzzleOverrode = 0;

    if (!Mc_Enabled())
    {
        s_prevLeft = s_prevRight = 0;
        s_havePrev  = 0;
        s_wheelStep = 0;
        {
            /* Notches spun while the cursor was off must not land as one
             * burst when it comes back. */
            extern int g_PsyX_WheelTotal;
            s_wheelNotches   = 0;
            s_prevWheelTotal = g_PsyX_WheelTotal;
        }
        if (s_puzzleFrames > 0)
            s_puzzleFrames--;
        return;
    }

    /* Wheel: PsyX latches each notch as a 2-frame countdown; a fresh notch is a
     * rise back to 2 (prev<2). Two notches in one frame collapse to one step. */
    {
        extern int g_PsyX_WheelUpFrames, g_PsyX_WheelDownFrames;
        extern int g_PsyX_WheelTotal;

        s_wheelNotches   = g_PsyX_WheelTotal - s_prevWheelTotal;
        s_prevWheelTotal = g_PsyX_WheelTotal;
        s_wheelStep = 0;
        if (g_PsyX_WheelUpFrames == 2 && s_prevWheelUp < 2)
            s_wheelStep = 1;
        else if (g_PsyX_WheelDownFrames == 2 && s_prevWheelDown < 2)
            s_wheelStep = -1;
        s_prevWheelUp   = g_PsyX_WheelUpFrames;
        s_prevWheelDown = g_PsyX_WheelDownFrames;
    }

    btn         = SDL_GetMouseState(&mx, &my);
    s_moved     = (mx != s_prevMx || my != s_prevMy);
    s_prevMx    = mx;
    s_prevMy    = my;
    s_leftDown  = (btn & SDL_BUTTON(SDL_BUTTON_LEFT))  != 0;
    s_rightDown = (btn & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    s_leftEdge  = (s_leftDown  && !s_prevLeft);
    s_rightEdge = (s_rightDown && !s_prevRight);
    s_prevLeft  = s_leftDown;
    s_prevRight = s_rightDown;

    s_prevGx = s_gx;
    s_prevGy = s_gy;
    s_inView = PsyX_MapWindowToViewport(mx, my, &fx, &fy);
    s_vx = fx;
    s_vy = fy;
    /* Mouse -> menu text-authoring space. Height differs by framebuffer mode:
     * 448 (interlaced menus) / 224 (progressive in-game), read live. */
    s_gx = fx * (float)g_GameWork.gsScreenWidth;
    s_gy = fy * (float)g_GameWork.gsScreenHeight
           - (float)(g_GameWork.gsScreenHeight / 2)
           + (float)MC_OFFSET_Y;
    if (!s_havePrev)
    {
        s_prevGx  = s_gx;
        s_prevGy  = s_gy;
        s_havePrev = 1;
    }

    /* ---- Free-cursor puzzle: ownership + clicks (the cursor itself is placed
     * by Pc_MouseCursor_PuzzleOverride from inside each puzzle) ---- */
    if (s_puzzleFrames > 0)
    {
        if (g_Controller0 != NULL)
        {
            s8* lx = &g_Controller0->sticks_24.sticks_0.leftX;
            s8* ly = &g_Controller0->sticks_24.sticks_0.leftY;

            if (*lx <= -STICK_THRESHOLD || *lx >= STICK_THRESHOLD || *ly <= -STICK_THRESHOLD || *ly >= STICK_THRESHOLD)
                s_puzzleMouseOwns = 0;
            else if (s_moved)
                s_puzzleMouseOwns = 1;

            /* The puzzle still adds the stick after the override has placed the
             * cursor, so a resting pad's drift (under the hand-back threshold)
             * would push it off the pointer. Harry is control-frozen here, so
             * zeroing it can't move him. */
            if (s_puzzleMouseOwns)
                *lx = *ly = 0;

            if (s_leftEdge)
                g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.enter;
            if (s_rightEdge)
                g_Controller0->clickedBtnFlags |= g_GameWorkPtr->config.controllerConfig.cancel;
        }
        s_puzzleFrames--;
    }
    else
    {
        s_puzzleMouseOwns = 0;
    }
}

int Pc_MouseCursor_MenuRowHover(int baseY, int stepY, int count, unsigned int visibleFlags, int* outClicked)
{
    int i, hovered = -1;

    if (outClicked != NULL)
        *outClicked = 0;

    if (!Mc_Enabled() || !s_inView || stepY <= 0)
        return -1;

    for (i = 0; i < count; i++)
    {
        int top, bot;

        if ((visibleFlags & (1u << i)) == 0)
            continue;

        top = baseY + (i * stepY) - 3; /* small lead-in above the glyph top */
        bot = top + stepY;
        if (s_gy >= (float)top && s_gy < (float)bot)
        {
            hovered = i;
            break;
        }
    }

    if (hovered >= 0 && outClicked != NULL)
        *outClicked = s_leftEdge;

    return hovered;
}

/* Only the picture hides: hover and clicks stay live, and any real mouse use
 * clears the flag before they could matter. */
static int Mc_HiddenForPad(void)
{
    extern int g_PsyX_PadInputLast;
    return g_PsyX_PadInputLast;
}

void Pc_MouseCursor_Show(void)
{
    s_showReq = 1;
}

/* The game's own arrow pointer -- the 32x32 sprite every cursor puzzle passes
 * to Gfx_CursorDraw: BG_ETC.TIM atlas UV(0,64), CLUT (192,0), 4bpp tpage at
 * VRAM (768,0). BG_ETC is uploaded during the boot logos (b_konami.c, or the
 * skip_intros replication in game_main.c) and no region's title art overlaps
 * it (US/JPN title lives at (848,0), PAL at (896,0); PAL's resliced BG_ETC
 * keeps the top half, so the same UVs hold). The arrow tip sits at the
 * sprite's top-left, so no hotspot offset. */
static int Mc_SpriteRgba(unsigned char* out)
{
    /* UV (0,64) -> halfword column 768, row 64, 8 halfwords per row; 16-entry
     * CLUT at halfword (192,0). Texel 0x0000 is transparent on the PSX. */
    unsigned short px[32 * 8];
    unsigned short clut[16];
    int x, y;

    GR_ReadVRAM(px, 768, 64, 8, 32);
    GR_ReadVRAM(clut, 192, 0, 16, 1);

    /* BG_ETC not uploaded yet: an all-zero CLUT would decode to nothing. */
    for (x = 0; x < 16; x++)
        if (clut[x] != 0)
            break;
    if (x == 16)
        return 0;

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            unsigned short w = px[y * 8 + (x >> 2)];
            unsigned short c = clut[(w >> ((x & 3) * 4)) & 0xF];
            unsigned char* o = out + (y * 32 + x) * 4;
            o[0] = (unsigned char)((c & 0x1F) << 3);
            o[1] = (unsigned char)(((c >> 5) & 0x1F) << 3);
            o[2] = (unsigned char)(((c >> 10) & 0x1F) << 3);
            o[3] = (c == 0) ? 0 : 255;
        }
    }
    return 1;
}

static GLuint Mc_GlShader(GLenum type, const char* src)
{
    GLuint      sh       = glCreateShader(type);
    GLint       ok       = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };

    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[MOUSE] cursor shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Leaves its program, VAO and buffer bound; Pc_MouseCursor_GlDraw restores them. */
static void Mc_GlInit(void)
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
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
    static const float quad[6][4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f }, { -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f,  1.0f, 1.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f }, { -1.0f, -1.0f, 0.0f, 1.0f }, {  1.0f, -1.0f, 1.0f, 1.0f },
    };
    GLuint vs, fs;
    GLint  ok = 0;

    s_glState = -1;
    vs = Mc_GlShader(GL_VERTEX_SHADER, vs_src);
    fs = Mc_GlShader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return;
    }

    s_glProg = glCreateProgram();
    glAttachShader(s_glProg, vs);
    glAttachShader(s_glProg, fs);
    glBindAttribLocation(s_glProg, 0, "a_pos");
    glBindAttribLocation(s_glProg, 1, "a_uv");
    glLinkProgram(s_glProg);
    glGetProgramiv(s_glProg, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_glProg, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[MOUSE] cursor link failed: %s", log);
        glDeleteProgram(s_glProg);
        s_glProg = 0;
        return;
    }
    glUseProgram(s_glProg);
    glUniform1i(glGetUniformLocation(s_glProg, "u_tex"), 0);

    glGenVertexArrays(1, &s_glVao);
    glBindVertexArray(s_glVao);
    glGenBuffers(1, &s_glVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_glVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    s_glState = 1;
}

void Pc_MouseCursor_GlDraw(void)
{
    GLint     pic[4], prevVp[4];
    GLint     prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint     prevDrawFb = 0, prevReadFb = 0;
    GLint     prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint     prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull, prevScissor;
    int       show, scale, size, left, top;

    show      = s_showReq || Pc_QuickOptions_IsOpen() || Pc_ConfirmDialog_IsOpen() || Pc_BindPanel_IsOpen();
    s_showReq = 0;
    if (!show || !Mc_Enabled() || !s_inView || Mc_HiddenForPad() || s_glState < 0)
        return;

    PsyX_GetDisplayViewport(&pic[0], &pic[1], &pic[2], &pic[3]);
    if (pic[2] <= 0 || pic[3] <= 0)
        return;

    /* Whole screen pixels per texel, from the picture height alone: the sprite
     * stays square and evenly sampled in every display mode. Half the PSX's
     * ~1/15 of the picture height, which read as a 96px arrow at 1440p. */
    scale = (int)floor((double)pic[3] / 960.0 + 0.5);
    if (scale < 1)
        scale = 1;
    size = 32 * scale;
    left = (int)floorf((float)pic[0] + s_vx * (float)pic[2]);
    top  = (int)floorf((float)pic[1] + (1.0f - s_vy) * (float)pic[3]);

    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    if (s_glState == 0)
        Mc_GlInit();
    if (s_glState != 1)
        goto restore;

    /* BG_ETC reaches VRAM during the boot logos, after the first frames. */
    if (!s_glTex)
    {
        unsigned char rgba[32 * 32 * 4];

        if (!Mc_SpriteRgba(rgba))
            goto restore;
        glGenTextures(1, &s_glTex);
        glBindTexture(GL_TEXTURE_2D, s_glTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    /* Same target as the debug overlay: in borderless, framebuffer 0 is
     * painted over by the scene target's blit at present. */
    glBindFramebuffer(GL_FRAMEBUFFER, GR_ScreenFBO());
    /* The viewport is the sprite rect itself, so the pillarbox never clips it
     * and every texel covers exactly scale x scale pixels. */
    glViewport(left, top - size, size, size);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_glProg);
    glBindVertexArray(s_glVao);
    glBindTexture(GL_TEXTURE_2D, s_glTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

restore:
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDrawFb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFb);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend)  glDisable(GL_BLEND);
    if (prevDepth)   glEnable(GL_DEPTH_TEST);
    if (prevCull)    glEnable(GL_CULL_FACE);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}
