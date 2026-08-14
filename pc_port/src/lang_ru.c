/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_ru.h"

#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "lang_text.h"          /* Pc_LangReadDiscFile */
#include "main/fileinfo.h"      /* g_FileTable */
#include "main/fsqueue.h"       /* FILE_1ST_FONT16_TIM */
#include "sh_log.h"

/* Alphabet order used by both halves of every charset table. Ё/ё sit where a
 * Russian keyboard puts them rather than at their Unicode home, so one index
 * covers all 33 letters. */
#define RU_LETTER_COUNT 33
#define RU_IDX_YO       6
#define RU_IDX_YE       5

/* One patch family's byte encoding. A zero entry means the repainted atlas has
 * no cell for that letter; the encoder falls back (uppercase -> lowercase, Ё ->
 * Е) rather than dropping the character. Values are the byte the DISC's own
 * text uses, so anything the port can already draw from the disc it can also
 * write. */
typedef struct {
    const char*   id;
    unsigned int  fontHash;               /* FNV-1a of that disc's FONT16.TIM */
    unsigned char up[RU_LETTER_COUNT];    /* А Б В Г Д Е Ё Ж ... Я */
    unsigned char lo[RU_LETTER_COUNT];    /* а б в г д е ё ж ... я */
} s_RuCharset;

/* --- ViT Co / Metallist / Team Raccoon (SLUS-00707, 2021-2022) -------------
 * In-place patch of the US disc: BODYPROG still encrypted and US-linked, the
 * 84-cell FONT16 strip repainted so cells 0-83 carry Cyrillic. Byte = cell +
 * GLYPH_TABLE_ASCII_OFFSET throughout, which is why the letters land on
 * apparently random ASCII. The atlas has no cell for uppercase З, Й, Щ, Ъ, Ь
 * or Ё: the translators reuse the digit '3' for З (their own text does this —
 * "3астрял"), and the rest fall back below. Verified by decoding the disc's
 * own item names and descriptions back to fluent Russian. */
static const s_RuCharset s_Charset_ViTCo = {
    "vitco", 0xCA515CABu,
    /* А     Б     В     Г     Д     Е     Ё     Ж     З     И     Й */
    {  0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x00, 0x47, 0x33, 0x49, 0x00,
    /* К     Л     М     Н     О     П     Р     С     Т     У     Ф */
       0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    /* Х     Ц     Ч     Ш     Щ     Ъ     Ы     Ь     Э     Ю     Я */
       0x56, 0x57, 0x58, 0x59, 0x00, 0x00, 0x5B, 0x4A, 0x3C, 0x3D, 0x3E },
    /* а     б     в     г     д     е     ё     ж     з     и     й */
    {  0x27, 0x2A, 0x5E, 0x3B, 0x60, 0x48, 0x5A, 0x61, 0x62, 0x63, 0x64,
    /* к     л     м     н     о     п     р     с     т     у     ф */
       0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    /* х     ц     ч     ш     щ     ъ     ы     ь     э     ю     я */
       0x78, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x70, 0x79, 0x7A }
};

/* --- consolgames.ru v1.1 (SLES-01514, 2011) --------------------------------
 * In-place patch of the PAL disc, which has the 126-cell 21x6 atlas: uppercase
 * Cyrillic replaces the Latin capitals (cells 26-51), Latin lowercase is left
 * alone, and the accent block is repainted with the lowercase alphabet (cells
 * 95-125) plus a handful of duplicates at 90-94. Byte = cell + 0x27 again, so
 * the lowercase run is 0x86-0xA4.
 *
 * Three of those bytes are unusable: Font_MapChar reproduces retail PAL's
 * special cases for 0x96 (en dash), 0x9C (oe) and 0xA1 (inverted !), which
 * would send с, ч and ь to the wrong cells. The patch hits the same wall and
 * solves it the same way — с takes the Latin 'c' cell (identical glyph, same
 * 9px advance) and ч/ь take the duplicate cells 92/93 — so these substitutions
 * are exactly what the disc's own text already uses. */
static const s_RuCharset s_Charset_ConsolGames = {
    "consolgames", 0x7D67AF89u,
    /* А     Б     В     Г     Д     Е     Ё     Ж     З     И     Й */
    {  0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x00, 0x47, 0x48, 0x49, 0x4A,
    /* К     Л     М     Н     О     П     Р     С     Т     У     Ф */
       0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    /* Х     Ц     Ч     Ш     Щ     Ъ     Ы     Ь     Э     Ю     Я */
       0x56, 0x57, 0x58, 0x59, 0x5A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80 },
    /* а     б     в     г     д     е     ё     ж     з     и     й */
    {  0x86, 0x87, 0x88, 0x89, 0x8A, 0x65, 0x00, 0x8B, 0x8C, 0x8D, 0x8E,
    /* к     л     м     н     о     п     р     с     т     у     ф */
       0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x63, 0x97, 0x98, 0x99,
    /* х     ц     ч     ш     щ     ъ     ы     ь     э     ю     я */
       0x9A, 0x9B, 0x83, 0x9D, 0x9E, 0x9F, 0xA0, 0x84, 0xA2, 0xA3, 0xA4 }
};

/* --- KUDOS (SLUS-00707, jewel-case era) ------------------------------------
 * Also an in-place US patch, but an UPPERCASE-ONLY translation: it repaints
 * only the Latin capitals (plus four digits pressed into service as О З Ч Б,
 * whose glyphs already pass for those letters) and leaves the lowercase half
 * of the atlas alone. So both halves of this table are the same bytes and
 * mixed-case menu text renders in caps, exactly as the disc's own text does.
 * Й, Щ, Ъ and Ё have no cell; the translators substitute И, Ш and Ь for them
 * throughout ("НАИДЕН", "ОБЬЕКТ"), which is what the fallbacks below do. */
#define RU_KUDOS_BYTES \
    /* А     Б     В     Г     Д     Е     Ё     Ж     З     И     Й */ \
    {  0x41, 0x36, 0x42, 0x5A, 0x44, 0x45, 0x00, 0x57, 0x33, 0x4E, 0x00, \
    /* К     Л     М     Н     О     П     Р     С     Т     У     Ф */ \
       0x4B, 0x4C, 0x4D, 0x48, 0x30, 0x4A, 0x50, 0x43, 0x54, 0x59, 0x4F, \
    /* Х     Ц     Ч     Ш     Щ     Ъ     Ы     Ь     Э     Ю     Я */ \
       0x58, 0x56, 0x34, 0x47, 0x00, 0x00, 0x46, 0x53, 0x51, 0x55, 0x52 }

static const s_RuCharset s_Charset_Kudos = {
    "kudos", 0x11552019u, RU_KUDOS_BYTES, RU_KUDOS_BYTES
};

/* --- ViToTiV / VovaMal (SLES-01514, 2006) ----------------------------------
 * The "old text" PAL patch. It never touched BODYPROG — kerning and the menu
 * strings are stock retail — and translated only the VIN overlays, repainting
 * the PAL atlas's Latin cells and accent block for the job. Its own menus
 * therefore stayed English, which on a Cyrillic atlas is unreadable; the table
 * below gives that disc Russian menus its authors never wrote.
 *
 * Щ lands on 0xC7 deliberately: retail PAL's drawer special-cases that byte to
 * the C-cedilla cell, which this patch repainted, and Font_MapChar reproduces
 * the same special case — so the port already resolves it correctly. Uppercase
 * Й Ъ Ы Ь have no cell of their own and fall back to their lowercase forms,
 * matching the patch's own habit of writing "НАИДЕН" for "НАЙДЕН". */
static const s_RuCharset s_Charset_ViToTiV = {
    "vitotiv", 0x1E0942BAu,
    /* А     Б     В     Г     Д     Е     Ё     Ж     З     И     Й */
    {  0x41, 0x46, 0x42, 0xF6, 0xF9, 0x45, 0x00, 0x56, 0x33, 0x51, 0x00,
    /* К     Л     М     Н     О     П     Р     С     Т     У     Ф */
       0x4B, 0x7A, 0x4D, 0x48, 0x4F, 0x5A, 0x50, 0x43, 0x54, 0xE0, 0xF3,
    /* Х     Ц     Ч     Ш     Щ     Ъ     Ы     Ь     Э     Ю     Я */
       0x58, 0xFC, 0xFA, 0xDF, 0xC7, 0x00, 0x00, 0x00, 0xFB, 0x57, 0x47 },
    /* а     б     в     г     д     е     ё     ж     з     и     й */
    {  0x61, 0xEB, 0xEA, 0xE9, 0xE2, 0x65, 0x00, 0x77, 0x4A, 0x75, 0xF4,
    /* к     л     м     н     о     п     р     с     т     у     ф */
       0xF2, 0xE7, 0x9D, 0xF1, 0x6F, 0x76, 0x70, 0x63, 0xE4, 0x79, 0x64,
    /* х     ц     ч     ш     щ     ъ     ы     ь     э     ю     я */
       0x78, 0x67, 0x68, 0xE1, 0x6B, 0x5E, 0x71, 0xE8, 0x72, 0x6D, 0x62 }
};

static const s_RuCharset* const s_Charsets[] = {
    &s_Charset_ViTCo,
    &s_Charset_ConsolGames,
    &s_Charset_Kudos,
    &s_Charset_ViToTiV
};

static const s_RuCharset* s_Active;
static char*              s_Arena;

typedef struct {
    const char* us;
    const char* ru;
} s_RuMenuEntry;

/* UTF-8 source strings, encoded into s_Arena at init. */
static const s_RuMenuEntry s_RuMenu[] = {
#include "lang_ru_menu.inc"
};
#define RU_MENU_COUNT ((int)(sizeof(s_RuMenu) / sizeof(s_RuMenu[0])))

static const char* s_RuMenuEncoded[RU_MENU_COUNT];

/* ------------------------------------------------------------------ */

static unsigned int Fnv1a(const unsigned char* p, unsigned int n)
{
    unsigned int h = 2166136261u;
    unsigned int i;

    for (i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Decode one UTF-8 sequence; returns the codepoint and advances *pp. */
static unsigned int Utf8Next(const char** pp)
{
    const unsigned char* p = (const unsigned char*)*pp;
    unsigned int         c = *p++;

    if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
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

/* Cyrillic codepoint -> index into the 33-letter table, or -1. `*isUpper` is
 * only meaningful on a hit. */
static int RuLetterIndex(unsigned int cp, int* isUpper)
{
    if (cp == 0x401) { *isUpper = 1; return RU_IDX_YO; }
    if (cp == 0x451) { *isUpper = 0; return RU_IDX_YO; }

    if (cp >= 0x410 && cp <= 0x42F)
    {
        *isUpper = 1;
        cp -= 0x410;
    }
    else if (cp >= 0x430 && cp <= 0x44F)
    {
        *isUpper = 0;
        cp -= 0x430;
    }
    else
    {
        return -1;
    }

    /* Unicode runs А..Е Ж..Я with no Ё; the table reserves slot 6 for it. */
    return (cp <= 5) ? (int)cp : (int)cp + 1;
}

/* Substitute letter for one an atlas may not have painted at all, chosen the
 * way these translators themselves handle the gap: Ё writes as Е, Й as И
 * ("НАИДЕН"), Щ as Ш, and Ъ as Ь ("ОБЬЕКТ"). -1 = no substitute. */
static const signed char s_RuSubstitute[RU_LETTER_COUNT] = {
    /* А  Б  В  Г  Д  Е  Ё  Ж  З  И  Й  К  Л  М  Н  О  П */
       -1,-1,-1,-1,-1,-1, 5,-1,-1,-1, 9,-1,-1,-1,-1,-1,-1,
    /* Р  С  Т  У  Ф  Х  Ц  Ч  Ш  Щ  Ъ  Ы  Ь  Э  Ю  Я */
       -1,-1,-1,-1,-1,-1,-1,-1,-1,25,29,-1,-1,-1,-1,-1
};

/* One Cyrillic letter -> font byte, applying the fallback chain. 0 = drop. */
static unsigned char RuEncodeLetter(int idx, int isUpper)
{
    int depth;

    /* At most two hops (Ъ -> Ь -> its lowercase form). */
    for (depth = 0; depth < 3 && idx >= 0; depth++)
    {
        unsigned char b = isUpper ? s_Active->up[idx] : s_Active->lo[idx];

        if (b == 0 && isUpper)
            b = s_Active->lo[idx]; /* atlas painted only the lowercase form */
        if (b != 0)
            return b;

        idx = s_RuSubstitute[idx];
    }
    return 0;
}

/* Transcode a UTF-8 menu string into the active patch's font bytes. ASCII
 * passes through untouched: '_' is the renderer's space, '\x01'/'\x07' are its
 * kerning and colour codes, and digits/punctuation still occupy their own
 * atlas cells on every one of these patches. Latin LETTERS do not — those
 * cells now hold Cyrillic — so the translations never contain any.
 * Returns the byte length written (excluding the NUL). */
static unsigned int RuEncode(const char* utf8, char* out)
{
    char* o = out;

    while (*utf8 != '\0')
    {
        unsigned int cp = Utf8Next(&utf8);
        int          idx;
        int          isUpper;

        if (cp < 0x80)
        {
            *o++ = (char)cp;
            continue;
        }

        idx = RuLetterIndex(cp, &isUpper);
        if (idx >= 0)
        {
            unsigned char b = RuEncodeLetter(idx, isUpper);
            if (b != 0)
                *o++ = (char)b;
            continue;
        }

        /* Typographic characters a translator may paste in. */
        switch (cp)
        {
            case 0x2013: case 0x2014: *o++ = '-'; break;
            case 0x2018: case 0x2019: *o++ = '\''; break;
            case 0x2026: *o++ = '.'; break;
            case 0x00A0: *o++ = '_'; break;
            default: break;
        }
    }

    *o = '\0';
    return (unsigned int)(o - out);
}

static const s_RuCharset* FindCharset(unsigned int hash)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_Charsets) / sizeof(s_Charsets[0])); i++)
    {
        if (s_Charsets[i]->fontHash == hash)
            return s_Charsets[i];
    }
    return NULL;
}

void Pc_RuInit(void)
{
    unsigned int   size;
    unsigned char* font;
    unsigned int   hash;
    unsigned int   bytes = 0;
    char*          out;
    int            i;

    /* Re-entrant: the title-screen Language row re-runs Pc_LangInit. */
    free(s_Arena);
    s_Arena  = NULL;
    s_Active = NULL;

    size = (unsigned int)g_FileTable[FILE_1ST_FONT16_TIM].blockCount << 8;
    if (size == 0)
        return;

    font = Pc_LangReadDiscFile(g_FileTable[FILE_1ST_FONT16_TIM].startSector, size);
    if (font == NULL)
        return;

    hash = Fnv1a(font, size);
    free(font);

    s_Active = FindCharset(hash);
    if (s_Active == NULL)
    {
        /* Every retail disc lands here too, so this is log-file only. It is the
         * one thing needed to add a new patch: hash in, charset table out. */
        SH_DBG("[LANG-RU] FONT16 hash %08X is not a known Russian repaint — menus stay English", hash);
        return;
    }

    /* Worst case one byte per UTF-8 byte (Cyrillic shrinks 2:1). */
    for (i = 0; i < RU_MENU_COUNT; i++)
        bytes += (unsigned int)strlen(s_RuMenu[i].ru) + 1;

    s_Arena = (char*)malloc(bytes);
    if (s_Arena == NULL)
    {
        s_Active = NULL;
        return;
    }

    out = s_Arena;
    for (i = 0; i < RU_MENU_COUNT; i++)
    {
        s_RuMenuEncoded[i] = out;
        out += RuEncode(s_RuMenu[i].ru, out) + 1;
    }

    SH_LOG("[LANG-RU] '%s' Russian patch detected (FONT16 %08X) — %d menu strings encoded",
           s_Active->id, hash, RU_MENU_COUNT);
}

int Pc_RuActive(void)
{
    return s_Active != NULL;
}

const char* Pc_RuMenuText(const char* us)
{
    int i;

    if (s_Active == NULL || us == NULL)
        return NULL;

    for (i = 0; i < RU_MENU_COUNT; i++)
    {
        if (s_RuMenu[i].us[0] == us[0] && strcmp(s_RuMenu[i].us, us) == 0)
            return s_RuMenuEncoded[i];
    }
    return NULL;
}
