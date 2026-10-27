/*
 * split_menu_xbox.c - the title screen's Multiplayer menu: split screen.
 *
 * Opened by the MULTIPLAYER row (title.c) in place, like the achievement list:
 * the pad belongs to it while it is up, and it draws with the game's own font
 * into OT2 (a darkened panel in layer 7 under the glyphs Gfx_StringDraw puts in
 * layer 6), rendered from DbgOverlay_XboxRender.
 *
 * The title screen is interlaced: its text space runs y -112 (top) to 336
 * (bottom), which is where the menu is laid out (the stock rows sit at y164+).
 *
 * New Game / Continue / Load Game arm split screen (split_xbox.c) and hand the
 * start back to the title screen's own entries, so every stock initialisation
 * runs exactly as it does for one player. Continue and Load Game are offered
 * when the title itself would offer them. The settings rows write the same
 * g_PcConfig fields and config keys as the Xbox Options page.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "pc_config.h"
#include "pc_playas.h"
#include "split_xbox.h"

#include <stdio.h>
#include <string.h>

extern int Pad_XboxSparePorts(int* ports, int max);   /* pad_xbox.c */

enum
{
    ROW_NEW,
    ROW_CONTINUE,
    ROW_LOAD,
    ROW_PAD,
    ROW_CHARA,
    ROW_LAYOUT,
    ROW_SOUND,
    ROW_RESPAWN,
    ROW_BACK,
    ROW_COUNT
};

#define SM_X       44
#define SM_TITLE_Y (-54)
#define SM_ROW_Y0  (-14)
#define SM_ROW_DY  20
#define SM_NOTE_Y  178

static int s_open;
static int s_sel;
static int s_canContinue;
static int s_canLoad;

static const char* const LBL_LAYOUT[SPLIT_LAYOUT_COUNT] = {
    "Top and Bottom", "Side by Side", "Top and Bottom (whole)", "Side by Side (whole)"
};
static const char* const LBL_SOUND[2] = { "Nearest Player", "P1 Left / P2 Right" };
static const int         RESPAWN_SECS[] = { 3, 5, 7, 10, 15 };

/* What the 12x16 font atlas can draw (ra_browser_xbox.c's rule): space -> '_'
 * (the drawer's space advance), the ASCII glyph range and '!' '&' kept, anything
 * else -> '?'. */
static void Sm_Sanitize(char* dst, const char* src, int cap)
{
    int i;

    for (i = 0; i < cap - 1 && src[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)src[i];

        if (c == ' ')
            dst[i] = '_';
        else if (c == '!' || c == '&' || (c >= 0x27 && c <= 0x7A))
            dst[i] = (char)c;
        else
            dst[i] = '?';
    }
    dst[i] = '\0';
}

static void Sm_Line(int x, int y, s16 color, const char* text)
{
    char buf[64];

    Sm_Sanitize(buf, text, (int)sizeof(buf));
    Gfx_StringSetColor(color);
    Gfx_StringSetPosition(x, y);
    Gfx_StringDraw(buf, 48);
}

static void Sm_SaveInt(const char* key, int v)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", v);
    PcConfig_SaveKeyValue(key, buf);
}

/* ---- Player 2's character: a play-as token, like player_character ---- */

int Split_P2CharIndex(void)
{
    int n = Pc_PlayAs_Count();
    int i;

    for (i = 0; i < n; i++)
        if (strcmp(Pc_PlayAs_Name(i), g_PcConfig.splitP2Character) == 0)
            return i;
    return 2; /* Cybil */
}

void Split_P2CharCycle(int dir)
{
    int n = Pc_PlayAs_Count();
    int i = (Split_P2CharIndex() + dir + n) % n;

    snprintf(g_PcConfig.splitP2Character, sizeof(g_PcConfig.splitP2Character), "%s", Pc_PlayAs_Name(i));
    PcConfig_SaveKeyValue("split_p2_character", g_PcConfig.splitP2Character);
}

/* The port Player 2 would get: the saved one when that pad is plugged in, else
 * the first free controller; 0 when there is none. */
int Split_P2PortPick(void)
{
    int ports[4];
    int n = Pad_XboxSparePorts(ports, 4);
    int i;

    for (i = 0; i < n; i++)
        if (ports[i] == g_PcConfig.splitP2Port)
            return ports[i];
    return n > 0 ? ports[0] : 0;
}

static int Sm_RowShown(int row)
{
    if (row == ROW_CONTINUE)
        return s_canContinue;
    if (row == ROW_LOAD)
        return s_canLoad;
    return 1;
}

void SplitMenu_SetAvailable(int canContinue, int canLoad)
{
    s_canContinue = canContinue;
    s_canLoad     = canLoad;
    if (s_open && !Sm_RowShown(s_sel))
        s_sel = ROW_NEW;
}

void SplitMenu_OpenAt(int preferContinue)
{
    s_open = 1;
    s_sel  = (preferContinue && s_canContinue) ? ROW_CONTINUE : ROW_NEW;
    if (g_PcConfig.splitLayout < 0 || g_PcConfig.splitLayout >= SPLIT_LAYOUT_COUNT)
        g_PcConfig.splitLayout = 0;
    if (g_PcConfig.splitAudio < 0 || g_PcConfig.splitAudio > 1)
        g_PcConfig.splitAudio = 0;
}

void SplitMenu_Open(void)
{
    SplitMenu_OpenAt(0);
}

void SplitMenu_Close(void)
{
    s_open = 0;
}

int SplitMenu_IsOpen(void)
{
    return s_open;
}

static void Sm_Cycle(int row, int dir)
{
    switch (row)
    {
        case ROW_PAD:
        {
            int ports[4];
            int n   = Pad_XboxSparePorts(ports, 4);
            int cur = Split_P2PortPick();
            int i, k = 0;

            if (n <= 1)
            {
                Sd_PlaySfx(Sfx_MenuError, 0, 64);
                return;
            }
            for (i = 0; i < n; i++)
                if (ports[i] == cur)
                    k = i;
            k = (k + dir + n) % n;
            g_PcConfig.splitP2Port = ports[k];
            Sm_SaveInt("split_p2_port", g_PcConfig.splitP2Port);
            break;
        }

        case ROW_CHARA:
            Split_P2CharCycle(dir);
            break;

        case ROW_LAYOUT:
            g_PcConfig.splitLayout = (g_PcConfig.splitLayout + dir + SPLIT_LAYOUT_COUNT) % SPLIT_LAYOUT_COUNT;
            Sm_SaveInt("split_layout", g_PcConfig.splitLayout);
            break;

        case ROW_SOUND:
            g_PcConfig.splitAudio = !g_PcConfig.splitAudio;
            Sm_SaveInt("split_audio", g_PcConfig.splitAudio);
            break;

        case ROW_RESPAWN:
        {
            int n = (int)(sizeof(RESPAWN_SECS) / sizeof(RESPAWN_SECS[0]));
            int i, k = 2;

            for (i = 0; i < n; i++)
                if (RESPAWN_SECS[i] == g_PcConfig.splitRespawnSec)
                    k = i;
            k = (k + dir + n) % n;
            g_PcConfig.splitRespawnSec = RESPAWN_SECS[k];
            Sm_SaveInt("split_respawn_s", g_PcConfig.splitRespawnSec);
            break;
        }

        default:
            return;
    }
    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
}

static void Sm_Step(int dir)
{
    int i;

    for (i = 0; i < ROW_COUNT; i++)
    {
        s_sel = (s_sel + dir + ROW_COUNT) % ROW_COUNT;
        if (Sm_RowShown(s_sel))
            break;
    }
    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
}

void SplitMenu_Update(int cancel, int up, int down, int left, int right, int confirm)
{
    if (!s_open)
        return;

    if (cancel)
    {
        s_open = 0;
        Split_MenuClosed();
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        return;
    }
    if (up)
        Sm_Step(-1);
    if (down)
        Sm_Step(1);
    if (s_sel >= ROW_PAD && s_sel <= ROW_RESPAWN && (left || right))
        Sm_Cycle(s_sel, right ? 1 : -1);

    if (!confirm)
        return;

    if (s_sel >= ROW_PAD && s_sel <= ROW_RESPAWN)
    {
        Sm_Cycle(s_sel, 1);
        return;
    }
    if (s_sel == ROW_BACK)
    {
        s_open = 0;
        Split_MenuClosed();
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        return;
    }
    if (Split_P2PortPick() == 0)
    {
        /* Nothing for Player 2 to play with: say so instead of starting. */
        Sd_PlaySfx(Sfx_MenuError, 0, 64);
        return;
    }
    s_open = 0;
    Split_MenuStart(s_sel == ROW_NEW      ? SPLIT_START_NEW
                    : s_sel == ROW_CONTINUE ? SPLIT_START_CONTINUE
                    :                         SPLIT_START_LOAD,
                    Split_P2PortPick());
}

/* Darken the area behind the menu: one subtractive TILE (abr 2, B - F) in OT2
 * layer 7, which the walk draws before the text's layer 6. Prim coordinates are
 * centre-origin; the interlaced title display is 448 tall. */
static void Sm_Panel(void)
{
    static TILE     s_tile[2];
    static DR_TPAGE s_tp[2];
    const int       buf = g_ActiveBufferIdx;

    setTile(&s_tile[buf]);
    setSemiTrans(&s_tile[buf], 1);
    setRGB0(&s_tile[buf], 112, 112, 112);
    setXY0(&s_tile[buf], -146, -184);
    setWH(&s_tile[buf], 292, 392);
    AddPrim(&g_OtTags0[buf][7], &s_tile[buf]);
    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 2, 0, 0));
    AddPrim(&g_OtTags0[buf][7], &s_tp[buf]);
}

void SplitMenu_Render(void)
{
    char line[64];
    int  port, i, y;

    if (!s_open)
        return;

    Sm_Panel();
    Sm_Line(SM_X, SM_TITLE_Y, StringColorId_Gold, "SPLIT SCREEN");

    port = Split_P2PortPick();
    y    = SM_ROW_Y0;
    for (i = 0; i < ROW_COUNT; i++)
    {
        s16 color = (i == s_sel) ? StringColorId_Gold : StringColorId_White;

        if (!Sm_RowShown(i))
            continue;
        switch (i)
        {
            case ROW_NEW:      snprintf(line, sizeof(line), "New Game");  break;
            case ROW_CONTINUE: snprintf(line, sizeof(line), "Continue");  break;
            case ROW_LOAD:     snprintf(line, sizeof(line), "Load Game"); break;
            case ROW_PAD:
                if (port)
                    snprintf(line, sizeof(line), "2P Controller:  Port %d", port);
                else
                    snprintf(line, sizeof(line), "2P Controller:  None");
                break;
            case ROW_CHARA:
                snprintf(line, sizeof(line), "2P Character:  %s", Pc_PlayAs_Label(Split_P2CharIndex()));
                break;
            case ROW_LAYOUT:
                snprintf(line, sizeof(line), "Layout:  %s", LBL_LAYOUT[g_PcConfig.splitLayout % SPLIT_LAYOUT_COUNT]);
                break;
            case ROW_SOUND:
                snprintf(line, sizeof(line), "2P Sound:  %s", LBL_SOUND[g_PcConfig.splitAudio & 1]);
                break;
            case ROW_RESPAWN:
                snprintf(line, sizeof(line), "Respawn:  %d s", g_PcConfig.splitRespawnSec);
                break;
            default:
                snprintf(line, sizeof(line), "Back");
                break;
        }
        if (i <= ROW_LOAD && port == 0)
            color = (i == s_sel) ? StringColorId_Red : StringColorId_DarkGrey;
        if (i == ROW_PAD || i == ROW_BACK)
            y += 8; /* gap after the game rows and before Back */
        Sm_Line(SM_X, y, color, line);
        y += SM_ROW_DY;
    }

    if (port == 0)
        Sm_Line(SM_X, SM_NOTE_Y, StringColorId_LightGrey, "Connect a controller for Player 2");
}
