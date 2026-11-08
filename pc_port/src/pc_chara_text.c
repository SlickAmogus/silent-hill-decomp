/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Character Select / Additional Characters text; see pc_chara_text.h. */
#include <stdio.h>
#include <string.h>

#include "lang_quick.h"
#include "pc_chara_text.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"
#include "pc_ttf_text.h"

#define CT_LINE 512

static int ct_translated(const char* en, const char* tr)
{
    return tr != en && strcmp(tr, en) != 0;
}

const char* Ct_Tr(const char* en)
{
    return Pc_LangQuick(en);
}

const char* Ct_Fill(const char* en, const char* placeholder, const char* value)
{
    return Pc_LangQuickFill(en, placeholder, value);
}

/* Whether this frame's text is in a translation: the language has one for
 * the screen's first heading. Keeps a whole screen in one face. */
static int ct_lang_active(void)
{
    const char* probe = "Character Select";

    return ct_translated(probe, Pc_LangQuick(probe));
}

/* Upper-cased (the game font is capitals only, and the screens keep that
 * look in TrueType) and laid out left to right for the TrueType queue. */
static const char* ct_ttf_line(const char* utf8, char* buf, int bufSize, int upper)
{
    char up[CT_LINE];

    if (upper)
    {
        Pc_LangUtf8Upper(utf8, up, (int)sizeof(up));
        utf8 = up;
    }
    else
    {
        snprintf(up, sizeof(up), "%s", utf8);
        utf8 = up;
    }
    {
        const char* vis = Pc_LangVisual(utf8, buf, bufSize);

        if (vis != buf)
            snprintf(buf, (size_t)bufSize, "%s", vis);
    }
    return buf;
}

int Ct_DrawText(int x, int y, int tall, int color, const char* utf8, int maxW, int pan)
{
    char clean[CT_LINE];
    int  size = tall ? NM_TEXT_TALL : NM_TEXT_SMALL;

    if (utf8 == NULL || utf8[0] == '\0')
        return 0;
    if (ct_lang_active())
    {
        char line[CT_LINE * 2];
        int  w;

        ct_ttf_line(utf8, line, (int)sizeof(line), 1);
        w = Pc_TtfText_Width(line, tall);
        Pc_TtfText_Queue(x, y, tall, color, line, maxW);
        return (maxW > 0 && w > maxW) ? maxW : w;
    }
    Nm_SanitizeGlyphs(utf8, clean, sizeof(clean), 1);
    return Nm_TextDrawLatin(x, y, size, color, clean, maxW, pan);
}

int Ct_TextWidth(const char* utf8, int tall)
{
    char clean[CT_LINE];

    if (utf8 == NULL || utf8[0] == '\0')
        return 0;
    if (ct_lang_active())
    {
        char line[CT_LINE * 2];

        ct_ttf_line(utf8, line, (int)sizeof(line), 1);
        return Pc_TtfText_Width(line, tall);
    }
    Nm_SanitizeGlyphs(utf8, clean, sizeof(clean), 1);
    return Nm_TextWidthLatin(clean, tall ? NM_TEXT_TALL : NM_TEXT_SMALL);
}

int Ct_Draw(int x, int y, int tall, int color, const char* en, int maxW, int pan)
{
    return Ct_DrawText(x, y, tall, color, Ct_Tr(en), maxW, pan);
}

int Ct_Width(const char* en, int tall)
{
    return Ct_TextWidth(Ct_Tr(en), tall);
}

void Ct_DrawCentered(int cx, int y, int tall, int color, const char* en)
{
    const char* tr = Ct_Tr(en);
    char        copy[CT_LINE];

    /* Ct_Tr's buffer is a short ring: hold it across the measure. */
    snprintf(copy, sizeof(copy), "%s", tr);
    Ct_DrawText(cx - Ct_TextWidth(copy, tall) / 2, y, tall, color, copy, 0, 0);
}

const char* Ct_Name(const char* label)
{
    char key[64];
    int  i, start = 1;

    if (label == NULL)
        return "";
    for (i = 0; label[i] != '\0' && i < (int)sizeof(key) - 1; i++)
    {
        char c = label[i];

        if (c >= 'A' && c <= 'Z' && !start)
            c = (char)(c + 32);
        else if (c >= 'a' && c <= 'z' && start)
            c = (char)(c - 32);
        key[i] = c;
        start  = (c == ' ' || c == '-');
    }
    key[i] = '\0';
    return Ct_Tr(key);
}

void Ct_Small(int x, int y, int color, const char* utf8, int maxW)
{
    char line[CT_LINE * 2];

    if (utf8 == NULL || utf8[0] == '\0')
        return;
    ct_ttf_line(utf8, line, (int)sizeof(line), 0);
    Pc_TtfText_Queue(x, y, 0, color, line, maxW);
}
