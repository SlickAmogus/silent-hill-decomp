#include "game.h"

#ifdef SH_PC_PORT

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <SDL_scancode.h>
#include <PsyX/common/glad.h>
#include "bodyprog/bodyprog.h"
#include "debug_console.h"

/* ========================================
 * EXTERNS
 * ======================================== */

extern const unsigned char* g_sdlKeyboardState;
extern const s_MapInfo MAP_INFOS[MapType_Count];
extern void GameBoot_MapLoad(s32 mapIdx);

/* ========================================
 * EMBEDDED 5x7 FONT (ASCII 32-95)
 * Each char is 5 columns × 7 rows, packed as 7 bytes (1 bit per column).
 * ======================================== */

static const unsigned char FONT_5X7[64][7] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    /* 34 '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00},
    /* 36 '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* 37 '%' */ {0x19,0x1A,0x02,0x04,0x0B,0x13,0x00},
    /* 38 '&' */ {0x08,0x14,0x08,0x15,0x12,0x0D,0x00},
    /* 39 ''' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
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

static GLuint g_fontTexture = 0;

static void con_init_font_texture(void)
{
    /* Create a 512x8 RGBA texture: 64 chars × 8px wide = 512, 8 rows (7+1 pad) */
    unsigned char pixels[512 * 8 * 4];
    int ch, row, col, px;

    memset(pixels, 0, sizeof(pixels));

    for (ch = 0; ch < 64; ch++) {
        for (row = 0; row < 7; row++) {
            unsigned char bits = FONT_5X7[ch][row];
            for (col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col))) {
                    px = ((row * 512) + (ch * 8) + col) * 4;
                    pixels[px + 0] = 255;
                    pixels[px + 1] = 255;
                    pixels[px + 2] = 255;
                    pixels[px + 3] = 255;
                }
            }
        }
    }

    glGenTextures(1, &g_fontTexture);
    glBindTexture(GL_TEXTURE_2D, g_fontTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* ========================================
 * CONSOLE STATE
 * ======================================== */

#define CON_MAX_INPUT    64
#define CON_MAX_OUTPUT   16
#define CON_MAX_LINE     64
#define CON_CHAR_W       7
#define CON_CHAR_H       10
#define CON_PAD_X        6
#define CON_PAD_Y        4

static int  con_open = 0;
static int  con_toggle_prev = 0;
static char con_input[CON_MAX_INPUT];
static int  con_input_len = 0;
static char con_output[CON_MAX_OUTPUT][CON_MAX_LINE];
static int  con_output_count = 0;
static int  con_cursor_blink = 0;

/* Previous key states for edge detection */
static unsigned char con_prev_keys[512];

/* Map overlay names for "map list" / "map <name>" */
static const struct {
    const char* name;
    int         overlayId;
} CON_MAP_TABLE[] = {
    { "THR",  0 },   /* MapOverlayId_MAP0_S00 = streets */
    { "SC",   3 },   /* MapOverlayId_MAP1_S00 = school interior */
    { "SU",  10 },   /* MapOverlayId_MAP2_S00 = school upper */
    { "SPR", 15 },   /* MapOverlayId_MAP3_S00 */
    { "SPU", 22 },   /* MapOverlayId_MAP4_S00 */
    { "RSR", 29 },   /* MapOverlayId_MAP5_S00 */
    { "RSU", 33 },   /* MapOverlayId_MAP6_S00 */
    { "APR", 39 },   /* MapOverlayId_MAP7_S00 */
    { NULL,   0 }
};

/* ========================================
 * OUTPUT HELPERS
 * ======================================== */

static void con_print(const char* msg)
{
    if (con_output_count < CON_MAX_OUTPUT) {
        strncpy(con_output[con_output_count], msg, CON_MAX_LINE - 1);
        con_output[con_output_count][CON_MAX_LINE - 1] = '\0';
        con_output_count++;
    } else {
        int i;
        for (i = 0; i < CON_MAX_OUTPUT - 1; i++) {
            memcpy(con_output[i], con_output[i + 1], CON_MAX_LINE);
        }
        strncpy(con_output[CON_MAX_OUTPUT - 1], msg, CON_MAX_LINE - 1);
        con_output[CON_MAX_OUTPUT - 1][CON_MAX_LINE - 1] = '\0';
    }
}

/* ========================================
 * COMMAND EXECUTION
 * ======================================== */

static void str_to_upper(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void con_execute(const char* cmd)
{
    char upper[CON_MAX_INPUT];
    char echo[CON_MAX_LINE];

    str_to_upper(upper, cmd, CON_MAX_INPUT);

    snprintf(echo, CON_MAX_LINE, "> %s", upper);
    con_print(echo);

    if (strcmp(upper, "HELP") == 0) {
        con_print("COMMANDS:");
        con_print("  MAP LIST  - LIST AVAILABLE MAPS");
        con_print("  MAP <TAG> - LOAD MAP BY TAG");
        con_print("  HELP      - SHOW THIS");
        con_print("  CLEAR     - CLEAR OUTPUT");
    }
    else if (strcmp(upper, "CLEAR") == 0) {
        con_output_count = 0;
    }
    else if (strcmp(upper, "MAP LIST") == 0 || strcmp(upper, "MAP") == 0) {
        int i;
        char line[CON_MAX_LINE];
        const char* cur_tag = "???";

        if (g_WorldGfx.mapInfo_0) {
            cur_tag = g_WorldGfx.mapInfo_0->tag_2;
        }

        snprintf(line, CON_MAX_LINE, "CURRENT MAP: %s", cur_tag);
        con_print(line);
        con_print("AVAILABLE MAPS:");

        for (i = 0; i < MapType_Count; i++) {
            const char* type = (MAP_INFOS[i].flags_6 & MapFlag_Interior) ? "INT" : "EXT";
            snprintf(line, CON_MAX_LINE, "  %s [%s]", MAP_INFOS[i].tag_2, type);
            con_print(line);
        }
    }
    else if (strncmp(upper, "MAP ", 4) == 0) {
        const char* tag = upper + 4;
        int i, found = -1;

        while (*tag == ' ') tag++;

        for (i = 0; CON_MAP_TABLE[i].name != NULL; i++) {
            if (strcmp(tag, CON_MAP_TABLE[i].name) == 0) {
                found = i;
                break;
            }
        }

        if (found >= 0) {
            char line[CON_MAX_LINE];
            snprintf(line, CON_MAX_LINE, "LOADING MAP: %s (OVL %d)",
                     CON_MAP_TABLE[found].name, CON_MAP_TABLE[found].overlayId);
            con_print(line);
            con_print("NOTE: ONLY MAP0_S00 OVERLAY LINKED");
            fprintf(stderr, "[CONSOLE] Loading map %s overlay=%d\n",
                    CON_MAP_TABLE[found].name, CON_MAP_TABLE[found].overlayId);
            fflush(stderr);

            g_SavegamePtr->mapOverlayId_A4 = CON_MAP_TABLE[found].overlayId;
            g_SysWork.processFlags_2298 = SysWorkProcessFlag_OverlayTransition;
            GameBoot_MapLoad(g_SavegamePtr->mapOverlayId_A4);
        } else {
            char line[CON_MAX_LINE];
            snprintf(line, CON_MAX_LINE, "UNKNOWN MAP: %s", tag);
            con_print(line);
            con_print("USE 'MAP LIST' TO SEE AVAILABLE");
        }
    }
    else if (upper[0] != '\0') {
        con_print("UNKNOWN COMMAND. TYPE 'HELP'");
    }
}

/* ========================================
 * KEY HANDLING
 * ======================================== */

static int key_edge(int scancode)
{
    int cur = g_sdlKeyboardState[scancode];
    int prev = con_prev_keys[scancode];
    con_prev_keys[scancode] = (unsigned char)cur;
    return cur && !prev;
}

static char scancode_to_char(int sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (char)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (char)(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc == SDL_SCANCODE_SPACE) return ' ';
    if (sc == SDL_SCANCODE_MINUS) return '-';
    if (sc == SDL_SCANCODE_PERIOD) return '.';
    if (sc == SDL_SCANCODE_COMMA) return ',';
    if (sc == SDL_SCANCODE_SLASH) return '/';
    if (sc == SDL_SCANCODE_SEMICOLON) return ':';
    return 0;
}

/* ========================================
 * UPDATE
 * ======================================== */

void DebugConsole_Update(void)
{
    int sc;

    if (!g_sdlKeyboardState) return;

    /* Toggle with ~ (grave/tilde key) */
    {
        int cur = g_sdlKeyboardState[SDL_SCANCODE_GRAVE];
        if (cur && !con_toggle_prev) {
            con_open = !con_open;
            if (con_open) {
                con_input_len = 0;
                con_input[0] = '\0';
                memset(con_prev_keys, 0, sizeof(con_prev_keys));
                fprintf(stderr, "[CONSOLE] Opened\n"); fflush(stderr);
            } else {
                fprintf(stderr, "[CONSOLE] Closed\n"); fflush(stderr);
            }
        }
        con_toggle_prev = cur;
    }

    if (!con_open) return;

    con_cursor_blink++;

    if (key_edge(SDL_SCANCODE_RETURN) || key_edge(SDL_SCANCODE_KP_ENTER)) {
        if (con_input_len > 0) {
            con_execute(con_input);
            con_input_len = 0;
            con_input[0] = '\0';
        }
        return;
    }

    if (key_edge(SDL_SCANCODE_BACKSPACE)) {
        if (con_input_len > 0) {
            con_input[--con_input_len] = '\0';
        }
        return;
    }

    if (key_edge(SDL_SCANCODE_ESCAPE)) {
        con_open = 0;
        return;
    }

    /* Character input */
    for (sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_Z; sc++) {
        if (key_edge(sc)) {
            char ch = scancode_to_char(sc);
            if (ch && con_input_len < CON_MAX_INPUT - 1) {
                con_input[con_input_len++] = ch;
                con_input[con_input_len] = '\0';
            }
        }
    }
    /* Numbers and punctuation */
    for (sc = SDL_SCANCODE_1; sc <= SDL_SCANCODE_0; sc++) {
        if (key_edge(sc)) {
            char ch = scancode_to_char(sc);
            if (ch && con_input_len < CON_MAX_INPUT - 1) {
                con_input[con_input_len++] = ch;
                con_input[con_input_len] = '\0';
            }
        }
    }
    {
        int punct[] = {SDL_SCANCODE_SPACE, SDL_SCANCODE_MINUS, SDL_SCANCODE_PERIOD,
                       SDL_SCANCODE_COMMA, SDL_SCANCODE_SLASH, SDL_SCANCODE_SEMICOLON, -1};
        int i;
        for (i = 0; punct[i] >= 0; i++) {
            if (key_edge(punct[i])) {
                char ch = scancode_to_char(punct[i]);
                if (ch && con_input_len < CON_MAX_INPUT - 1) {
                    con_input[con_input_len++] = ch;
                    con_input[con_input_len] = '\0';
                }
            }
        }
    }
}

/* ========================================
 * GL TEXT RENDERING
 * ======================================== */

static void gl_draw_char(float x, float y, float scale, char ch)
{
    int idx;
    float u0, u1, v0, v1;

    if (ch < 32 || ch > 95) ch = '?';
    idx = ch - 32;

    /* UV coords into the 512x8 font texture */
    u0 = (float)(idx * 8) / 512.0f;
    u1 = (float)(idx * 8 + 5) / 512.0f;
    v0 = 0.0f;
    v1 = 7.0f / 8.0f;

    glTexCoord2f(u0, v0); glVertex2f(x,             y);
    glTexCoord2f(u1, v0); glVertex2f(x + 5 * scale, y);
    glTexCoord2f(u1, v1); glVertex2f(x + 5 * scale, y + 7 * scale);
    glTexCoord2f(u0, v1); glVertex2f(x,             y + 7 * scale);
}

static void gl_draw_string(float x, float y, float scale, const char* str)
{
    while (*str) {
        if (*str >= 32) {
            gl_draw_char(x, y, scale, *str);
            x += CON_CHAR_W * scale;
        }
        str++;
    }
}

/* ========================================
 * RENDER (call before PsyX_EndScene)
 * ======================================== */

void DebugConsole_Render(void)
{
    int i;
    float y;
    float scale = 2.0f;
    int totalLines;
    float conHeight;
    char line[CON_MAX_LINE + 4];
    GLint viewport[4];

    if (!con_open) return;

    /* Lazy-init font texture */
    if (g_fontTexture == 0) {
        con_init_font_texture();
    }

    /* Get actual window dimensions */
    glGetIntegerv(GL_VIEWPORT, viewport);

    /* Save GL state */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, viewport[2], viewport[3], 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Draw console background */
    totalLines = con_output_count + 2; /* title + input */
    conHeight = (float)(CON_PAD_Y * 2 + totalLines * CON_CHAR_H * scale);

    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(0.0f, 0.0f, 0.0f, 0.80f);
    glVertex2f(0,            0);
    glVertex2f((float)viewport[2], 0);
    glVertex2f((float)viewport[2], conHeight);
    glVertex2f(0,            conHeight);
    glEnd();

    /* Draw separator line */
    glBegin(GL_QUADS);
    glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
    glVertex2f(0,            conHeight);
    glVertex2f((float)viewport[2], conHeight);
    glVertex2f((float)viewport[2], conHeight + 2);
    glVertex2f(0,            conHeight + 2);
    glEnd();

    /* Draw text */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fontTexture);

    y = (float)CON_PAD_Y;

    /* Title */
    glBegin(GL_QUADS);
    glColor4f(0.6f, 0.8f, 1.0f, 1.0f);
    gl_draw_string((float)CON_PAD_X, y, scale, "SILENT HILL DEBUG CONSOLE");
    y += CON_CHAR_H * scale;

    /* Output lines */
    for (i = 0; i < con_output_count; i++) {
        if (con_output[i][0] == '>') {
            glColor4f(1.0f, 1.0f, 0.5f, 1.0f);
        } else {
            glColor4f(0.9f, 0.9f, 0.9f, 1.0f);
        }
        gl_draw_string((float)CON_PAD_X, y, scale, con_output[i]);
        y += CON_CHAR_H * scale;
    }

    /* Input line */
    glColor4f(0.5f, 1.0f, 0.5f, 1.0f);
    if ((con_cursor_blink / 15) & 1) {
        snprintf(line, sizeof(line), "> %s_", con_input);
    } else {
        snprintf(line, sizeof(line), "> %s", con_input);
    }
    gl_draw_string((float)CON_PAD_X, y, scale, line);

    glEnd(); /* End GL_QUADS for all text */

    /* Restore GL state */
    glBindTexture(GL_TEXTURE_2D, 0);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
}

int DebugConsole_IsOpen(void)
{
    return con_open;
}

#endif /* SH_PC_PORT */
