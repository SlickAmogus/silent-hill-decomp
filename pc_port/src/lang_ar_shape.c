/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lang_ar_shape.c -- Arabic text into an Arabic disc's FONT16 cells.
 *
 * Neither disc stores Unicode. Each repaints the Latin cells with pre-shaped
 * letter forms, so a string has to be shaped (each letter's joining form
 * picked from its neighbours) before it can be spelled in that disc's bytes.
 *
 *   USA (Mansour):  logical order -- its drawer walks right to left. A
 *                   dual-joining letter with no final cell is written as its
 *                   connecting form plus the tail stroke 0x3D; hamza and
 *                   madda are separate zero-advance mark cells after the alef.
 *   PAL (Ahmed):    visual order -- its drawer is the stock left-to-right one,
 *                   so the shaped run is reversed. Every form has one cell.
 *
 * In both, a digit run keeps left-to-right reading order on screen, which in
 * the USA disc's logical storage means writing it reversed (the disc stores
 * "Room 302" as "203"), and brackets take their mirrored glyph.
 */
#include "lang_ar.h"

#include <string.h>

enum { F_ISO, F_FIN, F_INI, F_MED };

typedef struct
{
    unsigned short cp;
    unsigned char  dual;     /* joins to the following letter too */
    const char*    usa[4];
    const char*    pal[4];
} s_ArLetter;

/* Forms: isolated, final, initial, medial. */
static const s_ArLetter s_ArLetters[] = {
    { 0x0621, 0, { "\x3C", "\x3C", "\x3C", "\x3C" },             { "\x72", "\x72", "\x72", "\x72" } }, /* hamza */
    { 0x0622, 0, { "\x41\x3B", "\x79\x3B", "\x41\x3B", "\x79\x3B" }, { "\x61", "\x61", "\x61", "\x61" } }, /* alef madda */
    { 0x0623, 0, { "\x41\x3E", "\x79\x3E", "\x41\x3E", "\x79\x3E" }, { "\x61", "\x61", "\x61", "\x61" } }, /* alef hamza above */
    { 0x0624, 0, { "\x46\x27", "\x46\x27", "\x46\x27", "\x46\x27" }, { "\x71", "\x71", "\x71", "\x71" } }, /* waw hamza */
    { 0x0625, 0, { "\x41\x2A", "\x79\x2A", "\x41\x2A", "\x79\x2A" }, { "\x7D", "\x7D", "\x7D", "\x7D" } }, /* alef hamza below */
    { 0x0626, 1, { "\x4A\x3E", "\x4A\x3E", "\x77", "\x77" },     { "\x87", "\x87", "\x86", "\x86" } }, /* yeh hamza */
    { 0x0627, 0, { "\x41", "\x79", "\x41", "\x79" },             { "\x41", "\x41", "\x41", "\x41" } }, /* alef */
    { 0x0628, 1, { "\x42\x3D", "\x42\x3D", "\x42", "\x42" },     { "\x62", "\x62", "\x42", "\x42" } }, /* beh */
    { 0x0629, 0, { "\x75", "\x75", "\x75", "\x75" },             { "\x73", "\x73", "\x73", "\x73" } }, /* teh marbuta */
    { 0x062A, 1, { "\x4B\x3D", "\x4B\x3D", "\x4B", "\x4B" },     { "\x63", "\x63", "\x43", "\x43" } }, /* teh */
    { 0x062B, 1, { "\x4C\x3D", "\x4C\x3D", "\x4C", "\x4C" },     { "\x64", "\x64", "\x44", "\x44" } }, /* theh */
    { 0x062C, 1, { "\x4E", "\x4E", "\x43", "\x43" },             { "\x65", "\x65", "\x45", "\x45" } }, /* jeem */
    { 0x062D, 1, { "\x4F", "\x4F", "\x48", "\x48" },             { "\x66", "\x66", "\x46", "\x46" } }, /* hah */
    { 0x062E, 1, { "\x50", "\x50", "\x4D", "\x4D" },             { "\x67", "\x67", "\x47", "\x47" } }, /* khah */
    { 0x062F, 0, { "\x44", "\x44", "\x44", "\x44" },             { "\x48", "\x48", "\x48", "\x48" } }, /* dal */
    { 0x0630, 0, { "\x51", "\x51", "\x51", "\x51" },             { "\x49", "\x49", "\x49", "\x49" } }, /* thal */
    { 0x0631, 0, { "\x52", "\x52", "\x52", "\x52" },             { "\x4A", "\x4A", "\x4A", "\x4A" } }, /* reh */
    { 0x0632, 0, { "\x47", "\x47", "\x47", "\x47" },             { "\x4B", "\x4B", "\x4B", "\x4B" } }, /* zain */
    { 0x0633, 1, { "\x54", "\x54", "\x53", "\x53" },             { "\x6C", "\x6C", "\x4C", "\x4C" } }, /* seen */
    { 0x0634, 1, { "\x56", "\x56", "\x55", "\x55" },             { "\x6D", "\x6D", "\x4D", "\x4D" } }, /* sheen */
    { 0x0635, 1, { "\x58", "\x58", "\x57", "\x57" },             { "\x6E", "\x6E", "\x4E", "\x4E" } }, /* sad */
    { 0x0636, 1, { "\x5A", "\x5A", "\x59", "\x59" },             { "\x6F", "\x6F", "\x4F", "\x4F" } }, /* dad */
    { 0x0637, 1, { "\x49", "\x49", "\x49", "\x49" },             { "\x50", "\x50", "\x50", "\x50" } }, /* tah */
    { 0x0638, 1, { "\x61", "\x61", "\x61", "\x61" },             { "\x51", "\x51", "\x51", "\x51" } }, /* zah */
    { 0x0639, 1, { "\x65", "\x64", "\x62", "\x63" },             { "\x6A", "\x6A", "\x52", "\x52" } }, /* ain */
    { 0x063A, 1, { "\x69", "\x68", "\x66", "\x67" },             { "\x6B", "\x6B", "\x53", "\x53" } }, /* ghain */
    { 0x0641, 1, { "\x6A\x3D", "\x6A\x3D", "\x6A", "\x6A" },     { "\x74", "\x74", "\x54", "\x54" } }, /* feh */
    { 0x0642, 1, { "\x6C", "\x6C", "\x6B", "\x6B" },             { "\x75", "\x75", "\x55", "\x55" } }, /* qaf */
    { 0x0643, 1, { "\x6D", "\x6D", "\x5E", "\x5E" },             { "\x76", "\x76", "\x56", "\x56" } }, /* kaf */
    { 0x0644, 1, { "\x6F", "\x6F", "\x6E", "\x6E" },             { "\x77", "\x77", "\x57", "\x57" } }, /* lam */
    { 0x0645, 1, { "\x71", "\x71", "\x70", "\x70" },             { "\x78", "\x78", "\x58", "\x58" } }, /* meem */
    { 0x0646, 1, { "\x73", "\x73", "\x72", "\x72" },             { "\x79", "\x79", "\x59", "\x59" } }, /* noon */
    { 0x0647, 1, { "\x74", "\x74", "\x45", "\x45" },             { "\x7A", "\x7A", "\x5A", "\x5A" } }, /* heh */
    { 0x0648, 0, { "\x46", "\x46", "\x46", "\x46" },             { "\x70", "\x70", "\x70", "\x70" } }, /* waw */
    { 0x0649, 1, { "\x78", "\x78", "\x76", "\x76" },             { "\xE9", "\xE9", "\x68", "\x68" } }, /* alef maksura */
    { 0x064A, 1, { "\x4A", "\x4A", "\x76", "\x76" },             { "\xE8", "\xE8", "\x68", "\x68" } }, /* yeh */
};

/* Lam followed by an alef is one ligature cell (plus the alef's mark on USA). */
typedef struct
{
    unsigned short alef;
    const char*    usa[2]; /* isolated, final */
    const char*    pal;
} s_ArLamAlef;

static const s_ArLamAlef s_ArLamAlefs[] = {
    { 0x0627, { "\x60", "\x7A" },         "\xE7" },
    { 0x0623, { "\x60\x3E", "\x7A\x3E" }, "\x89" },
    { 0x0625, { "\x60\x2A", "\x7A\x2A" }, "\x8A" },
    { 0x0622, { "\x60\x3B", "\x7A\x3B" }, "\x88" },
};

static const s_ArLetter* ArLetter(unsigned int cp)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_ArLetters) / sizeof(s_ArLetters[0])); i++)
    {
        if (s_ArLetters[i].cp == cp)
            return &s_ArLetters[i];
    }
    return NULL;
}

static const s_ArLamAlef* ArLamAlef(unsigned int cp)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_ArLamAlefs) / sizeof(s_ArLamAlefs[0])); i++)
    {
        if (s_ArLamAlefs[i].alef == cp)
            return &s_ArLamAlefs[i];
    }
    return NULL;
}

static unsigned int ArUtf8Next(const unsigned char** pp)
{
    const unsigned char* p = *pp;
    unsigned int         cp;

    if (p[0] < 0x80)
    {
        cp = p[0];
        p += 1;
    }
    else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
    {
        cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        p += 2;
    }
    else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        p += 3;
    }
    else
    {
        cp = 0xFFFD;
        p += 1;
    }
    *pp = p;
    return cp;
}

#define AR_TOKENS_MAX 256

typedef struct
{
    char          bytes[4];
    unsigned char len;
    unsigned char digit; /* part of a number run, which keeps LTR reading order */
} s_ArToken;

static const char* ArPunct(unsigned int cp)
{
    switch (cp)
    {
        case '_':
        case ' ':    return "_";
        case 0x061F:
        case '?':    return "\x3F";
        case 0x060C:
        case ',':    return "\x2C";
        case '.':    return "\x2E";
        case ':':    return "\x3A";
        case '!':    return "\x5C";
        case '/':    return "\x2F";
        case '+':    return "\x2B";
        case '-':    return "\x2D";
        case '(':    return "\x29";
        case ')':    return "\x28";
        default:     return NULL;
    }
}

int Pc_ArEncode(const char* utf8, int disc, char* out, int outCap)
{
    unsigned int         cps[AR_TOKENS_MAX];
    s_ArToken            tok[AR_TOKENS_MAX];
    const unsigned char* p = (const unsigned char*)utf8;
    int                  n = 0;
    int                  t = 0;
    int                  lead = 0;
    int                  i;
    int                  o = 0;
    int                  usa = (disc == AR_DISC_USA_MANSOUR);

    if (outCap <= 0)
        return 0;

    /* Colour/control bytes that open a literal stay in front of it. */
    while (*p != 0 && *p < 0x20 && lead < outCap - 1)
        out[lead++] = (char)*p++;
    o = lead;

    while (*p != 0 && n < AR_TOKENS_MAX)
        cps[n++] = ArUtf8Next(&p);

    for (i = 0; i < n && t < AR_TOKENS_MAX; i++)
    {
        const s_ArLetter* L = ArLetter(cps[i]);
        const char*       s = NULL;
        int               prevJoins;
        int               nextJoins;

        prevJoins = (i > 0) && ArLetter(cps[i - 1]) != NULL && ArLetter(cps[i - 1])->dual;

        if (L != NULL && cps[i] == 0x0644 && i + 1 < n && ArLamAlef(cps[i + 1]) != NULL)
        {
            const s_ArLamAlef* la = ArLamAlef(cps[i + 1]);

            s = usa ? la->usa[prevJoins ? 1 : 0] : la->pal;
            i++;
        }
        else if (L != NULL)
        {
            int form;

            nextJoins = L->dual && (i + 1 < n) && ArLetter(cps[i + 1]) != NULL;
            if (prevJoins)
                form = nextJoins ? F_MED : F_FIN;
            else
                form = nextJoins ? F_INI : F_ISO;
            s = usa ? L->usa[form] : L->pal[form];
        }
        else if (cps[i] >= '0' && cps[i] <= '9')
        {
            tok[t].bytes[0] = (char)cps[i];
            tok[t].len      = 1;
            tok[t].digit    = 1;
            t++;
            continue;
        }
        else
        {
            s = ArPunct(cps[i]);
        }

        if (s == NULL)
            continue;

        tok[t].len   = (unsigned char)strlen(s);
        tok[t].digit = 0;
        memcpy(tok[t].bytes, s, tok[t].len);
        t++;
    }

    /* USA: logical order, digit runs reversed. PAL: everything reversed, digit
     * runs then turned back, so both read numbers left to right on screen. */
    if (usa)
    {
        i = 0;
        while (i < t)
        {
            int j = i;

            if (tok[i].digit)
            {
                int k;

                while (j < t && tok[j].digit)
                    j++;
                for (k = j - 1; k >= i && o < outCap - 1; k--)
                    out[o++] = tok[k].bytes[0];
                i = j;
                continue;
            }
            if (o + tok[i].len < outCap)
            {
                memcpy(out + o, tok[i].bytes, tok[i].len);
                o += tok[i].len;
            }
            i++;
        }
    }
    else
    {
        i = t - 1;
        while (i >= 0)
        {
            if (tok[i].digit)
            {
                int j = i;
                int k;

                while (j >= 0 && tok[j].digit)
                    j--;
                for (k = j + 1; k <= i && o < outCap - 1; k++)
                    out[o++] = tok[k].bytes[0];
                i = j;
                continue;
            }
            if (o + tok[i].len < outCap)
            {
                memcpy(out + o, tok[i].bytes, tok[i].len);
                o += tok[i].len;
            }
            i--;
        }
    }

    out[o] = '\0';
    return o;
}
