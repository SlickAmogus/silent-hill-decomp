/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PC-port block appended to the staff roll (src/screens/credits/credits.c),
 * and the text the ABOUT console command prints.
 *
 * The roll is a flat array of encoded lines walked at a fixed rate: the scroll
 * distance comes from the credits XA track's length and is divided by
 * D_801E5C20 to get the per-line step, so the roll always ends on the music and
 * the KCET logo. Appending lines therefore means raising D_801E5C20 by the same
 * amount, which tightens the step slightly rather than running past the track.
 *
 * Lines are built here instead of being written as escaped literals so the
 * content table below stays plain text.
 */
#include "game.h"
#include "sh_log.h"
#include "screens/credits/credits.h"

#include "pc_config.h"
#include "pc_credits.h"

#include <string.h>

/* stringtable.h, compiled into credits.c. */
extern char* g_CreditList[];

/* Roll control codes, read by func_801E4394 / func_801E4C1C. 0xD0 opens a span
 * whose rendered width is measured up to the closing 0xD8; 0xF0 then steps back
 * half that width (centre the span on the cursor) and 0xF8 the full width
 * (right-align it). 0x13/0x14/0x15 step right 8/16/32px, 0x1D steps left 32px.
 * These are the exact sequences vanilla's own header and voice-cast lines use. */
#define RC_SPAN_OPEN  0xD0
#define RC_SPAN_CLOSE 0xD8
#define RC_BACK_HALF  0xF0
#define RC_BACK_FULL  0xF8
#define RC_RIGHT_8    0x13
#define RC_RIGHT_16   0x14
#define RC_RIGHT_32   0x15
#define RC_LEFT_32    0x1D

/* ------------------------------------------------------------------------- */
/* Content. PcCreditRow_Header = centered section title, PcCreditRow_Name =    */
/* centered single line, PcCreditRow_Pair = "left  ....  right" two-column,    */
/* PcCreditRow_Blank = one empty scroll row. Vanilla spacing is 2 blanks under */
/* a header and 6 between sections; match it.                                  */
/* ------------------------------------------------------------------------- */
static const s_PcCreditRow s_Rows[] = {
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Header, "PC Port Credits", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Website", "sh1pc.com" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Name,   "github.com/SlickAmogus", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Header, "Developer", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Chris Hardin", "KushAstronaut" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Header, "Contributors", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Ric Lewis", "keylimesoda" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Sergio Manzur", "sergiomanzur" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Miau", "WhoisMiau0x1" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Header, "Special Thanks", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "Svperstar", "Supporter / Tester" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "luminati5983", "Discord Mod" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "eugene_lychany", "Discord Mod" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "frazzle1", "Supporter & Mod" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "ivanproff", "Playtesting & Mods" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "jojo670", "QoL Mods" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Pair,   "cristinathegamer", "Supporter" },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Header, "Original Silent Hill PSX Decomp", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Name,   "github.com/shdecompilations", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Name,   "Above not affiliated with this port", NULL },
    { PcCreditRow_Blank,  NULL, NULL },
    { PcCreditRow_Blank,  NULL, NULL },
};

#define PC_CREDITS_ROW_COUNT ((int)(sizeof(s_Rows) / sizeof(s_Rows[0])))

/* Headroom over the vanilla 399 lines plus whatever s_Rows grows to. */
#define PC_CREDITS_LINE_MAX 768
#define PC_CREDITS_POOL     8192

static char   s_pool[PC_CREDITS_POOL];
static int    s_poolUsed;
static char*  s_lines[PC_CREDITS_LINE_MAX];
static char   s_blank[] = "";
static char** s_list;

/* The roll font atlas only covers 0x21..0x84; a byte outside that range would
 * be read as a control code and wreck the rest of the line. */
static char Sanitize(char c)
{
    unsigned char u = (unsigned char)c;
    return (u == ' ' || (u >= 0x21 && u < 0x7F)) ? (char)u : '?';
}

static char* EmitCentered(const char* text)
{
    char* start;

    if (text == NULL)
        return s_blank;
    if (s_poolUsed + (int)strlen(text) + 4 > PC_CREDITS_POOL)
        return s_blank;

    start                = &s_pool[s_poolUsed];
    s_pool[s_poolUsed++] = (char)RC_SPAN_OPEN;
    s_pool[s_poolUsed++] = (char)RC_BACK_HALF;
    while (*text != 0)
        s_pool[s_poolUsed++] = Sanitize(*text++);
    s_pool[s_poolUsed++] = (char)RC_SPAN_CLOSE;
    s_pool[s_poolUsed++] = 0;

    return start;
}

static char* EmitPair(const char* left, const char* right)
{
    char* start;

    if (left == NULL || right == NULL)
        return s_blank;
    if (s_poolUsed + (int)strlen(left) + (int)strlen(right) + 8 > PC_CREDITS_POOL)
        return s_blank;

    start                = &s_pool[s_poolUsed];
    s_pool[s_poolUsed++] = (char)RC_LEFT_32;
    s_pool[s_poolUsed++] = (char)RC_SPAN_OPEN;
    s_pool[s_poolUsed++] = (char)RC_BACK_FULL;
    while (*left != 0)
        s_pool[s_poolUsed++] = Sanitize(*left++);
    s_pool[s_poolUsed++] = (char)RC_RIGHT_8;
    s_pool[s_poolUsed++] = (char)RC_RIGHT_16;
    s_pool[s_poolUsed++] = (char)RC_SPAN_CLOSE;
    s_pool[s_poolUsed++] = (char)RC_RIGHT_32;
    while (*right != 0)
        s_pool[s_poolUsed++] = Sanitize(*right++);
    s_pool[s_poolUsed++] = 0;

    return start;
}

void PcCredits_Begin(void)
{
    int vanilla = 0;
    int n       = 0;
    int i;

    while (g_CreditList[vanilla] != NULL)
        vanilla++;

    if (!g_PcConfig.pcPortCredits)
    {
        s_list     = g_CreditList;
        D_801E5C20 = vanilla + 1; /* vanilla scrolls one row past the last line */
        return;
    }

    s_poolUsed = 0;

    for (i = 0; i < vanilla && n < PC_CREDITS_LINE_MAX - 1; i++)
        s_lines[n++] = g_CreditList[i];

    for (i = 0; i < PC_CREDITS_ROW_COUNT && n < PC_CREDITS_LINE_MAX - 1; i++)
    {
        switch (s_Rows[i].kind)
        {
            case PcCreditRow_Header:
            case PcCreditRow_Name:
                s_lines[n++] = EmitCentered(s_Rows[i].left);
                break;

            case PcCreditRow_Pair:
                s_lines[n++] = EmitPair(s_Rows[i].left, s_Rows[i].right);
                break;

            default:
                s_lines[n++] = s_blank;
                break;
        }
    }

    s_lines[n++] = NULL;
    s_list       = s_lines;
    D_801E5C20   = n;

    SH_DBG("[CREDITS] PC port block on: %d vanilla + %d = %d rows", vanilla, n - 1 - vanilla, n);
}

char** PcCredits_List(void)
{
    if (s_list == NULL)
        PcCredits_Begin();

    return s_list;
}

int PcCredits_RowCount(void)
{
    return PC_CREDITS_ROW_COUNT;
}

const s_PcCreditRow* PcCredits_Row(int idx)
{
    if (idx < 0 || idx >= PC_CREDITS_ROW_COUNT)
        return NULL;

    return &s_Rows[idx];
}
