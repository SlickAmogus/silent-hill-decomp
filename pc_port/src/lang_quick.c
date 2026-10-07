/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_quick.h"

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "lang_pack.h"
#include "lang_ru.h"
#include "lang_text.h"
#include "pc_config.h"

typedef struct {
    const char* en;
    const char* tr[4]; /* de, fr, es, it -- UTF-8 */
} s_QuickTranslation;

/* PAL columns for the QUICK. strings. The PC Options rows these overlays also
 * show come from lang_menu.c's s_MenuTr instead, so the two menus agree. */
static const s_QuickTranslation s_QuickTr[] = {
#include "lang_quick_pal.inc"
};

enum { QL_ENGLISH = 0, QL_RUSSIAN, QL_PACK, QL_PAL };

/* Mirrors Pc_LangMenuText's order, so an overlay never disagrees with the
 * menu drawn behind it. */
static int QuickLang(void)
{
    int lang = g_PcConfig.language;

    if (g_GameRegion == Region_JPN)
        return QL_ENGLISH;
    if (Pc_RuActive())
        return QL_RUSSIAN;
    if (Pc_LangPackActive())
        return QL_PACK;
    if ((g_GameRegion == Region_EUR || (g_GameRegion == Region_USA && Pc_FanTextActive())) &&
        lang >= 1 && lang <= 4)
    {
        return QL_PAL;
    }
    return QL_ENGLISH;
}

#define RING_COUNT 8
#define RING_SIZE  512

static char* RingNext(void)
{
    static char s_Ring[RING_COUNT][RING_SIZE];
    static int  s_Slot;

    s_Slot = (s_Slot + 1) % RING_COUNT;
    return s_Ring[s_Slot];
}

unsigned int Pc_LangUtf8Next(const char** pp)
{
    const unsigned char* p = (const unsigned char*)*pp;
    unsigned int         c = *p++;

    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    }
    else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
    {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    }
    else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80)
    {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    }

    *pp = (const char*)p;
    return c;
}

static int Utf8Put(unsigned int cp, char* out, int room)
{
    if (cp < 0x80 && room >= 1)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800 && room >= 2)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room >= 3)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

/* Menu-dialect text -> what the overlay draws: '_' is the menu font's space,
 * and the PAL table is Latin-1. */
static const char* FromMenu(const char* s, int latin1)
{
    char* out = RingNext();
    int   n   = 0;

    while (*s != '\0' && n < RING_SIZE - 4)
    {
        unsigned int cp;

        if (*s == '_')
        {
            out[n++] = ' ';
            s++;
            continue;
        }
        if (*s == '\x01')
        {
            s++;
            continue;
        }
        if (latin1)
            cp = (unsigned char)*s++;
        else
            cp = Pc_LangUtf8Next(&s);
        n += Utf8Put(cp, out + n, RING_SIZE - 1 - n);
    }
    out[n] = '\0';
    return out;
}

const char* Pc_LangQuickMenu(const char* us)
{
    const char* tr = NULL;
    int         latin1 = 0;

    if (us == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_RUSSIAN: tr = Pc_RuMenuUtf8(us); break;
        case QL_PACK:    tr = Pc_LangPackMenuUtf8(us); break;
        case QL_PAL:     tr = Pc_LangMenuPal(us, g_PcConfig.language); latin1 = 1; break;
        default:         break;
    }
    return FromMenu(tr ? tr : us, tr ? latin1 : 0);
}

const char* Pc_LangQuick(const char* en)
{
    int i;

    if (en == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_PACK:
        {
            char key[160];
            int  n = snprintf(key, sizeof(key), "QUICK.");
            const char* tr;

            for (i = 0; en[i] != '\0' && n < (int)sizeof(key) - 1; i++)
                key[n++] = (en[i] == ' ') ? '_' : (en[i] == '=') ? '-' : en[i];
            key[n] = '\0';
            tr = Pc_LangPackUtf8(key);
            return (tr && tr[0]) ? tr : en;
        }
        case QL_PAL:
        {
            int lang = g_PcConfig.language;

            for (i = 0; i < (int)(sizeof(s_QuickTr) / sizeof(s_QuickTr[0])); i++)
            {
                if (s_QuickTr[i].en[0] == en[0] && strcmp(s_QuickTr[i].en, en) == 0)
                    return s_QuickTr[i].tr[lang - 1] ? s_QuickTr[i].tr[lang - 1] : en;
            }
            return en;
        }
        default:
            return en;
    }
}

const char* Pc_LangQuickFill(const char* en, const char* placeholder, const char* value)
{
    const char* tmpl = Pc_LangQuick(en);
    const char* at   = strstr(tmpl, placeholder);
    char*       out  = RingNext();
    size_t      phLen = strlen(placeholder);

    if (at == NULL)
    {
        tmpl = en;
        at   = strstr(tmpl, placeholder);
        if (at == NULL)
        {
            snprintf(out, RING_SIZE, "%s", en);
            return out;
        }
    }
    snprintf(out, RING_SIZE, "%.*s%s%s", (int)(at - tmpl), tmpl, value ? value : "", at + phLen);
    return out;
}

static unsigned int UpperCp(unsigned int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 0x20;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 0x20;
    /* Latin Extended-A pairs the cases in two parities around U+0138/U+0149. */
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : c - 1;
    if (c >= 0x430 && c <= 0x44F)
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45F)
        return c - 0x50;
    return c;
}

void Pc_LangUtf8Upper(const char* in, char* out, int outSize)
{
    int n = 0;

    if (outSize <= 0)
        return;
    while (in != NULL && *in != '\0' && n < outSize - 4)
        n += Utf8Put(UpperCp(Pc_LangUtf8Next(&in)), out + n, outSize - 1 - n);
    out[n] = '\0';
}
