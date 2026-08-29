/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_jpn.h"

#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "lang_text.h"      /* Pc_LangReadDiscFile */
#include "main/fileinfo.h"  /* g_FileTable */
#include "main/fsqueue.h"   /* FILE_VIN_OPTION_BIN, FILE_VIN_SAVELOAD_BIN */
#include "pc_kanji.h"       /* Pc_KanjiIsLead (SJIS pair walk) */
#include "lang_zh.h"
#include "sh_log.h"

/* NTSC-J menu text comes off the disc rather than out of the binary.
 *
 * Retail NTSC-J draws its option and save/load strings from VIN/OPTION.BIN and
 * VIN/SAVELOAD.BIN, in Japanese. The port compiles the decomp's US branch, so
 * it drew the compiled English literals there instead — wrong for a Japanese
 * disc, and doubly wrong for the Chinese fan translation, whose whole point is
 * that the disc's text is Chinese. Reading the strings back fixes both at once
 * and needs no translation of our own: the Chinese patch ships as a PPF, which
 * can only replace bytes in place, so its strings sit at exactly the offsets
 * the Japanese ones do. */

typedef struct {
    const char*  us;   /* the compiled literal the port would have drawn */
    unsigned char file; /* 0 = VIN/OPTION.BIN, 1 = VIN/SAVELOAD.BIN */
    unsigned short off;
    unsigned char len; /* 0 = NUL-terminated */
} s_JpnMenuEntry;

static const s_JpnMenuEntry s_JpnMenu[] = {
#include "lang_jpn_menu.inc"
};
#define JPN_MENU_COUNT ((int)(sizeof(s_JpnMenu) / sizeof(s_JpnMenu[0])))

/* Rows the PORT added, which no disc can supply. Written rather than read
 * back, and Japanese only -- see the header of lang_jpn_pcopt.inc for why the
 * Chinese glyph set cannot borrow them. */
typedef struct {
    const char* us;
    const char* jp; /* Shift-JIS */
} s_JpnPcOptEntry;

static const s_JpnPcOptEntry s_JpnPcOpt[] = {
#include "lang_jpn_pcopt.inc"
};
#define JPN_PCOPT_COUNT ((int)(sizeof(s_JpnPcOpt) / sizeof(s_JpnPcOpt[0])))

static char*       s_Arena;
static const char* s_Text[JPN_MENU_COUNT];
static int         s_Active;

/* SJIS ideographic space: the overlays pad their fixed-width value fields with
 * it, so the shorter of two languages still lines up with the longer. */
#define SJIS_SPACE_HI 0x81
#define SJIS_SPACE_LO 0x40

/* Copy one entry's bytes out of the overlay, dropping the trailing padding and
 * converting the overlay's "~Cn" colour markup to the \x0n the port's string
 * drawer uses. Returns the advanced write pointer.
 *
 * The walk is SJIS-pair-aware, and has to be: a trail byte may legally be 0x7E
 * ('~') or 0x81, so scanning bytes one at a time would let the second half of a
 * kanji masquerade as markup or as padding. Two of the Chinese location names
 * do contain a 0x7E trail byte. */
static char* CopyEntry(char* out, const unsigned char* src, unsigned int avail,
                       unsigned int off, unsigned int len)
{
    unsigned int end;
    unsigned int i;
    char*        keep;

    if (off >= avail)
    {
        *out++ = '\0';
        return out;
    }

    if (len != 0)
    {
        end = off + len;
        if (end > avail)
            end = avail;
    }
    else
    {
        end = off;
        while (end < avail && src[end] != '\0')
            end++;
    }

    /* `keep` trails the last token that was not padding, so trailing padding is
     * dropped without a backwards scan that could land mid-pair. */
    keep = out;
    i    = off;
    while (i < end)
    {
        if (Pc_KanjiIsLead(src[i]) && i + 1 < end)
        {
            int pad = (src[i] == SJIS_SPACE_HI && src[i + 1] == SJIS_SPACE_LO);

            *out++ = (char)src[i];
            *out++ = (char)src[i + 1];
            i += 2;
            if (!pad)
                keep = out;
            continue;
        }

        if (src[i] == '~' && i + 2 < end && src[i + 1] == 'C' &&
            src[i + 2] >= '0' && src[i + 2] <= '7')
        {
            *out++ = (char)(src[i + 2] - '0');
            i += 3;
            keep = out;
            continue;
        }

        *out++ = (char)src[i];
        i++;
        if (src[i - 1] != ' ')
            keep = out;
    }

    out    = keep;
    *out++ = '\0';
    return out;
}

void Pc_JpnMenuInit(void)
{
    unsigned char* ovl[2] = { NULL, NULL };
    unsigned int   size[2];
    char*          out;
    unsigned int   bytes = 0;
    int            i;
    int            packHits = 0;

    free(s_Arena);
    s_Arena = NULL;
    s_Active = 0;

    if (g_GameRegion != Region_JPN)
        return;

    for (i = 0; i < 2; i++)
    {
        int idx = (i == 0) ? FILE_VIN_OPTION_BIN : FILE_VIN_SAVELOAD_BIN;

        size[i] = (unsigned int)g_FileTable[idx].blockCount << 8;
        ovl[i]  = Pc_LangReadDiscFile(g_FileTable[idx].startSector, size[i]);
        if (ovl[i] == NULL)
        {
            free(ovl[0]);
            free(ovl[1]);
            SH_WARN("[LANG-JP] could not read the menu overlays — keeping compiled text");
            return;
        }
    }

    /* An entry can only shrink (padding dropped, ~Cn folded to one byte). */
    for (i = 0; i < JPN_MENU_COUNT; i++)
        bytes += (s_JpnMenu[i].len ? s_JpnMenu[i].len : 128) + 1;

    s_Arena = (char*)malloc(bytes);
    if (s_Arena == NULL)
    {
        free(ovl[0]);
        free(ovl[1]);
        return;
    }

    out = s_Arena;
    for (i = 0; i < JPN_MENU_COUNT; i++)
    {
        const s_JpnMenuEntry* e = &s_JpnMenu[i];

        const unsigned char* packed;
        int                  packedLen = 0;

        s_Text[i] = out;
        /* zh.pack first when it is driving. The overlays read above are the
         * RETAIL disc's, so they are Japanese, and with the Chinese glyph set
         * active they would draw as unrelated characters — which is what left
         * the options screen in Japanese after switching. The pack stores the
         * same fixed-width field the overlay holds, so CopyEntry does the same
         * padding trim and ~Cn fold either way. */
        packed = Pc_LangZhPackActive() ? Pc_LangZhMenuEntry(i, &packedLen) : NULL;
        if (packed != NULL)
        {
            packHits++;
            out = CopyEntry(out, packed, (unsigned int)packedLen, 0, e->len);
        }
        else if (Pc_LangZhPackActive())
        {
            /* Chinese is active but the pack has nothing for this one. The
             * disc's string is Japanese and would draw as unrelated characters
             * through the Chinese glyphs, so drop to the port's own English
             * literal — NULL means exactly that to Pc_JpnMenuText. Readable
             * beats wrong. */
            s_Text[i] = NULL;
            continue;
        }
        else
        {
            out = CopyEntry(out, ovl[e->file], size[e->file], e->off, e->len);
        }
        /* An offset that landed on nothing keeps the compiled literal. */
        if (s_Text[i][0] == '\0')
            s_Text[i] = NULL;
    }

    free(ovl[0]);
    free(ovl[1]);
    s_Active = 1;
    /* Naming the shortfall matters: the pack is read BY INDEX and a short one
     * simply runs out, so rows the game has and the pack does not fall back to
     * English silently. New rows are always APPENDED to lang_jpn_menu.inc for
     * that reason -- inserting one in the middle would shift every later index
     * and draw the wrong string instead of none. Regenerate with
     * pc_port/tools/gen_zh_pack.py after any change to that table. */
    if (Pc_LangZhPackActive() && packHits < JPN_MENU_COUNT)
    {
        SH_WARN("[LANG-JP] zh.pack supplied %d of %d menu strings — the rest stay "
                "English; regenerate it with tools/gen_zh_pack.py",
                packHits, JPN_MENU_COUNT);
    }
    SH_LOG("[LANG-JP] %d menu strings installed (%s)", JPN_MENU_COUNT,
           Pc_LangZhPackActive() ? "zh.pack" : "the disc's own overlays");
}

const char* Pc_JpnMenuText(const char* us)
{
    int i;

    if (us == NULL)
        return NULL;

    /* The port's own rows first. Not gated on s_Active -- these never came off
     * a disc, so they stand whether or not the overlays read. Gated on the
     * glyph set instead: under the Chinese font every kanji here would draw as
     * an unrelated character, and English beats that.
     *
     * Ahead of the disc so this table can also OVERRIDE a disc string whose
     * retail wording does not fit the port's layout (the BGM/SE volume rows,
     * whose full sentence ran through the volume bar). Chinese still gets the
     * disc string, which is what should happen: it is shorter and it is real. */
    if (!Pc_KanjiChineseActive())
    {
        for (i = 0; i < JPN_PCOPT_COUNT; i++)
        {
            if (s_JpnPcOpt[i].us[0] == us[0] && strcmp(s_JpnPcOpt[i].us, us) == 0)
                return s_JpnPcOpt[i].jp;
        }
    }

    if (s_Active)
    {
        for (i = 0; i < JPN_MENU_COUNT; i++)
        {
            if (s_JpnMenu[i].us[0] == us[0] && strcmp(s_JpnMenu[i].us, us) == 0)
                return s_Text[i];
        }
    }
    return NULL;
}
