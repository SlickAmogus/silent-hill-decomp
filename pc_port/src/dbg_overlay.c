#include "game.h"

#ifdef SH_PC_PORT

#include <stdio.h>
#include <string.h>
#include <SDL_scancode.h>
#include <SDL_keyboard.h>
#include <PsyX/common/glad.h>
#include "bodyprog/bodyprog.h"
#include "sh_log.h"
#include "dbg_overlay.h"

extern void vcGetNowCamPos(VECTOR3* cam_pos);

/* ========================================
 * 5x7 BITMAP FONT (ASCII 32–95)
 * ======================================== */

static const unsigned char FONT_5X7[64][7] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    /* 34 '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00},
    /* 36 '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* 37 '%' */ {0x19,0x1A,0x02,0x04,0x0B,0x13,0x00},
    /* 38 '&' */ {0x08,0x14,0x08,0x15,0x12,0x0D,0x00},
    /* 39 '\'' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x02,0x04,0x04,0x04,0x04,0x02,0x00},
    /* 41 ')' */ {0x08,0x04,0x04,0x04,0x04,0x08,0x00},
    /* 42 '*' */ {0x04,0x15,0x0E,0x15,0x04,0x00,0x00},
    /* 43 '+' */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x04,0x04,0x08},
    /* 45 '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x04,0x00},
    /* 47 '/' */ {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    /* 48 '0' */ {0x0E,0x11,0x13,0x15,0x19,0x0E,0x00},
    /* 49 '1' */ {0x04,0x0C,0x04,0x04,0x04,0x0E,0x00},
    /* 50 '2' */ {0x0E,0x11,0x01,0x06,0x08,0x1F,0x00},
    /* 51 '3' */ {0x0E,0x11,0x02,0x01,0x11,0x0E,0x00},
    /* 52 '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x00},
    /* 53 '5' */ {0x1F,0x10,0x1E,0x01,0x11,0x0E,0x00},
    /* 54 '6' */ {0x06,0x08,0x1E,0x11,0x11,0x0E,0x00},
    /* 55 '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x00},
    /* 56 '8' */ {0x0E,0x11,0x0E,0x11,0x11,0x0E,0x00},
    /* 57 '9' */ {0x0E,0x11,0x11,0x0F,0x02,0x0C,0x00},
    /* 58 ':' */ {0x00,0x04,0x00,0x00,0x04,0x00,0x00},
    /* 59 ';' */ {0x00,0x04,0x00,0x00,0x04,0x04,0x08},
    /* 60 '<' */ {0x02,0x04,0x08,0x04,0x02,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* 62 '>' */ {0x08,0x04,0x02,0x04,0x08,0x00,0x00},
    /* 63 '?' */ {0x0E,0x11,0x02,0x04,0x00,0x04,0x00},
    /* 64 '@' */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    /* 65 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x00},
    /* 66 'B' */ {0x1E,0x11,0x1E,0x11,0x11,0x1E,0x00},
    /* 67 'C' */ {0x0E,0x11,0x10,0x10,0x11,0x0E,0x00},
    /* 68 'D' */ {0x1E,0x11,0x11,0x11,0x11,0x1E,0x00},
    /* 69 'E' */ {0x1F,0x10,0x1E,0x10,0x10,0x1F,0x00},
    /* 70 'F' */ {0x1F,0x10,0x1E,0x10,0x10,0x10,0x00},
    /* 71 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x0F,0x00},
    /* 72 'H' */ {0x11,0x11,0x1F,0x11,0x11,0x11,0x00},
    /* 73 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x0E,0x00},
    /* 74 'J' */ {0x07,0x02,0x02,0x02,0x12,0x0C,0x00},
    /* 75 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 76 'L' */ {0x10,0x10,0x10,0x10,0x10,0x1F,0x00},
    /* 77 'M' */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x00},
    /* 78 'N' */ {0x11,0x19,0x15,0x13,0x11,0x11,0x00},
    /* 79 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x0E,0x00},
    /* 80 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x00},
    /* 81 'Q' */ {0x0E,0x11,0x11,0x15,0x12,0x0D,0x00},
    /* 82 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x00},
    /* 83 'S' */ {0x0E,0x11,0x08,0x06,0x11,0x0E,0x00},
    /* 84 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x00},
    /* 85 'U' */ {0x11,0x11,0x11,0x11,0x11,0x0E,0x00},
    /* 86 'V' */ {0x11,0x11,0x11,0x0A,0x0A,0x04,0x00},
    /* 87 'W' */ {0x11,0x11,0x11,0x15,0x15,0x0A,0x00},
    /* 88 'X' */ {0x11,0x0A,0x04,0x04,0x0A,0x11,0x00},
    /* 89 'Y' */ {0x11,0x0A,0x04,0x04,0x04,0x04,0x00},
    /* 90 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x1F,0x00},
    /* 91 '[' */ {0x0E,0x08,0x08,0x08,0x08,0x0E,0x00},
    /* 92 '\' */ {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    /* 93 ']' */ {0x0E,0x02,0x02,0x02,0x02,0x0E,0x00},
    /* 94 '^' */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x1F,0x00},
};

#define CHAR_W   7   /* pixels per character (5 glyph + 1 gap + 1 pad) */
#define CHAR_H   10  /* pixels per row (7 glyph + 3 pad) */
#define SCALE    2.0f
#define PAD      6

static GLuint s_fontTex = 0;

static void init_font(void)
{
    unsigned char pixels[512 * 8 * 4];
    int ch, row, col, px;
    memset(pixels, 0, sizeof(pixels));
    for (ch = 0; ch < 64; ch++) {
        for (row = 0; row < 7; row++) {
            unsigned char bits = FONT_5X7[ch][row];
            for (col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col))) {
                    px = ((row * 512) + (ch * 8) + col) * 4;
                    pixels[px+0] = pixels[px+1] = pixels[px+2] = pixels[px+3] = 255;
                }
            }
        }
    }
    glGenTextures(1, &s_fontTex);
    glBindTexture(GL_TEXTURE_2D, s_fontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Draw a string starting at (x,y) in window pixels. Must be called inside
 * glBegin(GL_QUADS). Returns x after the last character. */
static float draw_str(float x, float y, const char* str)
{
    int idx;
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c < 32 || c > 95) c = 32;
        idx = c - 32;
        float u0 = (idx * 8.0f) / 512.0f;
        float u1 = (idx * 8.0f + 5.0f) / 512.0f;
        glTexCoord2f(u0, 0.0f); glVertex2f(x,              y);
        glTexCoord2f(u1, 0.0f); glVertex2f(x + 5.0f*SCALE, y);
        glTexCoord2f(u1, 1.0f); glVertex2f(x + 5.0f*SCALE, y + 7.0f*SCALE);
        glTexCoord2f(u0, 1.0f); glVertex2f(x,              y + 7.0f*SCALE);
        x += CHAR_W * SCALE;
    }
    return x;
}

/* String width in window pixels (for right-alignment). */
static float str_width(const char* str)
{
    int n = 0;
    while (*str++) n++;
    return (float)(n * CHAR_W) * SCALE;
}

/* ========================================
 * MARKER STATE
 * ======================================== */

#define MAX_MARKS 16
#define LINE_LEN  48

static char s_a_lines[MAX_MARKS][LINE_LEN];
static char s_b_lines[MAX_MARKS][LINE_LEN];
static int  s_a_count = 0;
static int  s_b_count = 0;

static int s_prev_a = 0;
static int s_prev_b = 0;

static void log_mark(char letter, int idx, VECTOR3* hpos, VECTOR3* cpos)
{
    float hx = hpos->vx / 4096.0f;
    float hy = hpos->vy / 4096.0f;
    float hz = hpos->vz / 4096.0f;
    float cx = cpos->vx / 4096.0f;
    float cy = cpos->vy / 4096.0f;
    float cz = cpos->vz / 4096.0f;

    SH_DBG_ECHO("======== MARK-%c%d ========", letter, idx);
    SH_DBG_ECHO("  Harry  : (%.3f, %.3f, %.3f)  raw(%d, %d, %d)",
                hx, hy, hz, (int)hpos->vx, (int)hpos->vy, (int)hpos->vz);
    SH_DBG_ECHO("  Camera : (%.3f, %.3f, %.3f)  raw(%d, %d, %d)",
                cx, cy, cz, (int)cpos->vx, (int)cpos->vy, (int)cpos->vz);
    SH_DBG_ECHO("========================");
}

/* ========================================
 * UPDATE — called once per frame before Joy_ReadP1
 * ======================================== */

void DbgOverlay_Update(void)
{
    VECTOR3 hpos, cpos;
    s_SubCharacter* player;
    int cur_a, cur_b;

    const unsigned char* ks = SDL_GetKeyboardState(NULL);
    if (!ks) return;

    cur_a = ks[SDL_SCANCODE_LEFTBRACKET];
    cur_b = ks[SDL_SCANCODE_RIGHTBRACKET];

    if (cur_a && !s_prev_a && s_a_count < MAX_MARKS) {
        player = &g_SysWork.playerWork.player;
        hpos   = player->position;
        vcGetNowCamPos(&cpos);

        s_a_count++;
        log_mark('A', s_a_count, &hpos, &cpos);
        snprintf(s_a_lines[s_a_count - 1], LINE_LEN,
                 "A%d: H(%.1f,%.1f,%.1f)",
                 s_a_count,
                 hpos.vx / 4096.0f, hpos.vy / 4096.0f, hpos.vz / 4096.0f);
    }

    if (cur_b && !s_prev_b && s_b_count < MAX_MARKS) {
        player = &g_SysWork.playerWork.player;
        hpos   = player->position;
        vcGetNowCamPos(&cpos);

        s_b_count++;
        log_mark('B', s_b_count, &hpos, &cpos);
        snprintf(s_b_lines[s_b_count - 1], LINE_LEN,
                 "B%d: H(%.1f,%.1f,%.1f)",
                 s_b_count,
                 hpos.vx / 4096.0f, hpos.vy / 4096.0f, hpos.vz / 4096.0f);
    }

    s_prev_a = cur_a;
    s_prev_b = cur_b;
}

/* ========================================
 * RENDER — called after OT2 draw, before PsyX_EndScene
 * ======================================== */

void DbgOverlay_Render(void)
{
    GLint vp[4];
    int i;

    if (s_a_count == 0 && s_b_count == 0) return;

    if (s_fontTex == 0) init_font();

    glGetIntegerv(GL_VIEWPORT, vp);
    float winW = (float)vp[2];
    float winH = (float)vp[3];

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, winW, winH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float lineH = CHAR_H * SCALE;

    /* Draw dark backgrounds for all visible lines */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.65f);

    for (i = 0; i < s_a_count; i++) {
        float y = PAD + i * lineH;
        float w = str_width(s_a_lines[i]) + PAD * 2;
        glVertex2f(0,   y - 1);
        glVertex2f(w,   y - 1);
        glVertex2f(w,   y + lineH);
        glVertex2f(0,   y + lineH);
    }
    for (i = 0; i < s_b_count; i++) {
        float y  = PAD + i * lineH;
        float w  = str_width(s_b_lines[i]) + PAD * 2;
        float x0 = winW - w;
        glVertex2f(x0,   y - 1);
        glVertex2f(winW, y - 1);
        glVertex2f(winW, y + lineH);
        glVertex2f(x0,   y + lineH);
    }
    glEnd();

    /* Draw text */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_fontTex);
    glBegin(GL_QUADS);

    glColor4f(0.4f, 1.0f, 0.4f, 1.0f);
    for (i = 0; i < s_a_count; i++) {
        float y = PAD + i * lineH;
        draw_str((float)PAD, y, s_a_lines[i]);
    }

    glColor4f(1.0f, 0.8f, 0.3f, 1.0f);
    for (i = 0; i < s_b_count; i++) {
        float y  = PAD + i * lineH;
        float x  = winW - str_width(s_b_lines[i]) - PAD;
        draw_str(x, y, s_b_lines[i]);
    }

    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
}

#endif /* SH_PC_PORT */
