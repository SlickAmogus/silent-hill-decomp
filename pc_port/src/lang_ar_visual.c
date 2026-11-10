/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lang_ar_visual.c -- logical Arabic UTF-8 -> the visual line the TrueType
 * overlays can draw.
 *
 * stb_truetype maps codepoints to glyphs and nothing more (no GSUB, no bidi),
 * and the bake loops lay glyphs out left to right. So the joining forms are
 * picked here from Arabic Presentation Forms-B, and the line is reordered the
 * way an RTL paragraph displays it. This is the subset of the Unicode bidi
 * algorithm a single UI line needs: base direction RTL, one level of embedded
 * LTR (Latin, digits), neutrals resolved by their strong neighbours, brackets
 * mirrored in the RTL parts.
 */
#include "lang_quick.h"

#include <string.h>

#define AV_MAX 512

enum { J_NONE = 0, J_RIGHT, J_DUAL, J_CAUSING, J_TRANSPARENT };

/* Presentation Forms-B base (isolated form) for U+0621..U+064A, and how each
 * letter joins. A right-joining letter has only isolated and final forms;
 * alef maksura is dual-joining in Unicode but Forms-B gives it only those two. */
static const struct { unsigned short base; unsigned char join; } s_Forms[0x064A - 0x0621 + 1] = {
    { 0xFE80, J_NONE  }, /* 0621 hamza */
    { 0xFE81, J_RIGHT }, /* 0622 alef madda */
    { 0xFE83, J_RIGHT }, /* 0623 alef hamza above */
    { 0xFE85, J_RIGHT }, /* 0624 waw hamza */
    { 0xFE87, J_RIGHT }, /* 0625 alef hamza below */
    { 0xFE89, J_DUAL  }, /* 0626 yeh hamza */
    { 0xFE8D, J_RIGHT }, /* 0627 alef */
    { 0xFE8F, J_DUAL  }, /* 0628 beh */
    { 0xFE93, J_RIGHT }, /* 0629 teh marbuta */
    { 0xFE95, J_DUAL  }, /* 062A teh */
    { 0xFE99, J_DUAL  }, /* 062B theh */
    { 0xFE9D, J_DUAL  }, /* 062C jeem */
    { 0xFEA1, J_DUAL  }, /* 062D hah */
    { 0xFEA5, J_DUAL  }, /* 062E khah */
    { 0xFEA9, J_RIGHT }, /* 062F dal */
    { 0xFEAB, J_RIGHT }, /* 0630 thal */
    { 0xFEAD, J_RIGHT }, /* 0631 reh */
    { 0xFEAF, J_RIGHT }, /* 0632 zain */
    { 0xFEB1, J_DUAL  }, /* 0633 seen */
    { 0xFEB5, J_DUAL  }, /* 0634 sheen */
    { 0xFEB9, J_DUAL  }, /* 0635 sad */
    { 0xFEBD, J_DUAL  }, /* 0636 dad */
    { 0xFEC1, J_DUAL  }, /* 0637 tah */
    { 0xFEC5, J_DUAL  }, /* 0638 zah */
    { 0xFEC9, J_DUAL  }, /* 0639 ain */
    { 0xFECD, J_DUAL  }, /* 063A ghain */
    { 0, J_NONE }, { 0, J_NONE }, { 0, J_NONE }, { 0, J_NONE }, { 0, J_NONE }, /* 063B-063F */
    { 0, J_CAUSING },    /* 0640 tatweel */
    { 0xFED1, J_DUAL  }, /* 0641 feh */
    { 0xFED5, J_DUAL  }, /* 0642 qaf */
    { 0xFED9, J_DUAL  }, /* 0643 kaf */
    { 0xFEDD, J_DUAL  }, /* 0644 lam */
    { 0xFEE1, J_DUAL  }, /* 0645 meem */
    { 0xFEE5, J_DUAL  }, /* 0646 noon */
    { 0xFEE9, J_DUAL  }, /* 0647 heh */
    { 0xFEED, J_RIGHT }, /* 0648 waw */
    { 0xFEEF, J_RIGHT }, /* 0649 alef maksura */
    { 0xFEF1, J_DUAL  }, /* 064A yeh */
};

static int IsArabic(unsigned int c)
{
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) ||
           (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

/* Harakat. The overlays place every glyph at its own advance, so a combining
 * mark would land beside its letter instead of on it; UI text reads fine
 * without them, so they are dropped. */
static int IsMark(unsigned int c)
{
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

static int JoinType(unsigned int c)
{
    if (IsMark(c))
        return J_TRANSPARENT;
    if (c >= 0x0621 && c <= 0x064A)
        return s_Forms[c - 0x0621].join;
    return J_NONE;
}

/* Lam-alef ligature (isolated form) for the alef that follows a lam, or 0. */
static unsigned int LamAlef(unsigned int alef)
{
    switch (alef)
    {
        case 0x0622: return 0xFEF5;
        case 0x0623: return 0xFEF7;
        case 0x0625: return 0xFEF9;
        case 0x0627: return 0xFEFB;
        default:     return 0;
    }
}

static int Shape(const unsigned int* in, int n, unsigned int* out)
{
    int o = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        unsigned int c = in[i];
        int          t = JoinType(c);
        int          prev, next, joinPrev, joinNext;

        if (t == J_TRANSPARENT)
            continue;
        if (t == J_NONE || t == J_CAUSING || s_Forms[c - 0x0621].base == 0)
        {
            out[o++] = c;
            continue;
        }

        for (prev = i - 1; prev >= 0 && JoinType(in[prev]) == J_TRANSPARENT; prev--)
            ;
        for (next = i + 1; next < n && JoinType(in[next]) == J_TRANSPARENT; next++)
            ;
        joinPrev = prev >= 0 &&
                   (JoinType(in[prev]) == J_DUAL || JoinType(in[prev]) == J_CAUSING);

        if (c == 0x0644 && next < n && LamAlef(in[next]) != 0)
        {
            out[o++] = LamAlef(in[next]) + (joinPrev ? 1 : 0);
            i        = next;
            continue;
        }

        joinNext = (t == J_DUAL) && next < n && JoinType(in[next]) != J_NONE;
        if (joinPrev && joinNext)
            out[o++] = s_Forms[c - 0x0621].base + 3u;
        else if (joinNext)
            out[o++] = s_Forms[c - 0x0621].base + 2u;
        else if (joinPrev)
            out[o++] = s_Forms[c - 0x0621].base + 1u;
        else
            out[o++] = s_Forms[c - 0x0621].base;
    }
    return o;
}

static int IsDigit(unsigned int c)
{
    return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9);
}

static int IsStrongLtr(unsigned int c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return 1;
    return c >= 0xC0 && !IsArabic(c) && c != 0xD7 && c != 0xF7 &&
           !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) &&
           !(c >= 0xFF00 && c <= 0xFF0F);
}

static unsigned int Mirror(unsigned int c)
{
    switch (c)
    {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        default:  return c;
    }
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

const char* Pc_LangVisual(const char* in, char* buf, int bufSize)
{
    unsigned int  logical[AV_MAX];
    unsigned int  cps[AV_MAX];
    unsigned char ltr[AV_MAX];
    const char*   p   = in;
    int           n   = 0;
    int           any = 0;
    int           i, j, o;

    if (in == NULL || buf == NULL || bufSize <= 0)
        return in;

    while (*p != '\0' && n < AV_MAX)
    {
        logical[n] = Pc_LangUtf8Next(&p);
        any |= IsArabic(logical[n]);
        n++;
    }
    if (!any)
        return in;

    n = Shape(logical, n, cps);

    /* Signs and separators that belong to a number travel with it, so "+15",
     * "50%" and "1.5" stay whole instead of splitting around the RTL text. */
    for (i = 0; i < n; i++)
    {
        unsigned int c = cps[i];
        int          prevDigit = i > 0 && IsDigit(cps[i - 1]);
        int          nextDigit = i + 1 < n && IsDigit(cps[i + 1]);

        ltr[i] = IsStrongLtr(c) || IsDigit(c) ||
                 ((c == '+' || c == '-') && nextDigit) || (c == '%' && prevDigit) ||
                 ((c == '.' || c == ',' || c == ':' || c == '/') && prevDigit && nextDigit);
    }

    /* A neutral between two LTR characters is part of their run; any other
     * takes the line's RTL direction. */
    for (i = 0; i < n; i++)
    {
        int prevL, nextL;

        if (ltr[i] || IsArabic(cps[i]))
            continue;
        for (j = i - 1; j >= 0 && !ltr[j] && !IsArabic(cps[j]); j--)
            ;
        prevL = j >= 0 && ltr[j];
        for (j = i + 1; j < n && !ltr[j] && !IsArabic(cps[j]); j++)
            ;
        nextL = j < n && ltr[j];
        if (prevL && nextL)
            ltr[i] = 2;
    }

    o = 0;
    for (i = n - 1; i >= 0 && o < bufSize - 4; i--)
    {
        if (ltr[i])
        {
            int start = i;

            while (start > 0 && ltr[start - 1])
                start--;
            for (j = start; j <= i; j++)
                o += Utf8Put(cps[j], buf + o, bufSize - 1 - o);
            i = start;
        }
        else
        {
            o += Utf8Put(Mirror(cps[i]), buf + o, bufSize - 1 - o);
        }
    }
    buf[o] = '\0';
    return buf;
}
