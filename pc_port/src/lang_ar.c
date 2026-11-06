/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lang_ar.c -- Arabic fan-translated discs (see lang_ar.h).
 */
#include "lang_ar.h"
#include "lang_text.h"
#include "sh_log.h"
#include "main/fileinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    AR_FILE_BODYPROG,
    AR_FILE_OPTION,
    AR_FILE_SAVELOAD,
    AR_FILE_COUNT
} e_ArFile;

typedef enum
{
    AR_LOC_DIRECT, /* translation overwrote the English in place */
    AR_LOC_PTR,    /* a pointer word at `loc` names the translation */
    AR_LOC_CODE    /* a lui at `aux` + addiu at `loc` load its address */
} e_ArLoc;

typedef struct
{
    unsigned char disc;
    unsigned char file;
    unsigned char kind;
    unsigned int  loc;
    unsigned int  aux;
    const char*   us;
} s_ArDiscText;

static const s_ArDiscText s_ArDiscTexts[] = {
#include "lang_ar_disc.inc"
};

#define AR_DISC_TEXT_COUNT ((int)(sizeof(s_ArDiscTexts) / sizeof(s_ArDiscTexts[0])))

/* PSX link base of each file, per disc (BODYPROG moved between USA and PAL). */
static const unsigned int s_ArFileBase[3][AR_FILE_COUNT] = {
    { 0, 0, 0 },
    { 0x80024B60u, 0x801E2600u, 0x801E2600u },
    { 0x80025690u, 0x801E2600u, 0x801E2600u },
};

static const char* s_ArDiscTr[AR_DISC_TEXT_COUNT];
static char*       s_ArDiscPool;

typedef struct
{
    const char* us;
    const char* ar; /* logical-order UTF-8 */
} s_ArMenu;

static const s_ArMenu s_ArMenus[] = {
#include "lang_ar_menu.inc"
};

#define AR_MENU_COUNT ((int)(sizeof(s_ArMenus) / sizeof(s_ArMenus[0])))

static const char* s_ArMenuTr[AR_MENU_COUNT];
static char*       s_ArMenuPool;

typedef struct
{
    unsigned int fontHash; /* FNV-1a of the disc's whole FONT16.TIM */
    e_ArDisc     disc;
    const char*  region;
    const char*  group;
} s_ArDiscId;

static const s_ArDiscId s_ArDiscs[] = {
    { 0x5D67A2B1u, AR_DISC_USA_MANSOUR, "USA", "Mansour (ETRD)" },
    { 0xF4A11673u, AR_DISC_PAL_AHMED,   "PAL", "Ahmed Khaled, M2D, r.h.h (Obeid), RITO" },
};

static const s_ArDiscId* s_Active;

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

/* The game's own Fs_DecryptOverlay LCG; 1ST/BODYPROG is obfuscated on disc. */
static void ArDecrypt(unsigned char* data, unsigned int size)
{
    unsigned int seed = 0;
    unsigned int i;

    for (i = 0; i + 4 <= size; i += 4)
    {
        unsigned int w;

        seed = (seed + 0x01309125u) * 0x03A452F7u;
        w    = (unsigned int)data[i] | ((unsigned int)data[i + 1] << 8) |
               ((unsigned int)data[i + 2] << 16) | ((unsigned int)data[i + 3] << 24);
        w   ^= seed;
        data[i]     = (unsigned char)w;
        data[i + 1] = (unsigned char)(w >> 8);
        data[i + 2] = (unsigned char)(w >> 16);
        data[i + 3] = (unsigned char)(w >> 24);
    }
}

static unsigned int ArWord(const unsigned char* d, unsigned int off)
{
    return (unsigned int)d[off] | ((unsigned int)d[off + 1] << 8) |
           ((unsigned int)d[off + 2] << 16) | ((unsigned int)d[off + 3] << 24);
}

/* Offset of the translation the entry names, or -1. The disc is untrusted:
 * every read is bounds-checked and the string must end inside the file. */
static int ArResolve(const s_ArDiscText* t, const unsigned char* d, unsigned int size, unsigned int base)
{
    unsigned int addr;
    unsigned int off;

    if (t->kind == AR_LOC_DIRECT)
    {
        off = t->loc;
    }
    else
    {
        if (t->loc + 4 > size || (t->kind == AR_LOC_CODE && t->aux + 4 > size))
            return -1;
        if (t->kind == AR_LOC_PTR)
        {
            addr = ArWord(d, t->loc);
        }
        else
        {
            unsigned int hi = (ArWord(d, t->aux) & 0xFFFFu) << 16;
            int          lo = (int)(short)(ArWord(d, t->loc) & 0xFFFFu);

            addr = hi + (unsigned int)lo;
        }
        if (addr < base)
            return -1;
        off = addr - base;
    }

    if (off >= size || memchr(d + off, 0, size - off) == NULL)
        return -1;
    return (int)off;
}

static void ArLoadDiscText(void)
{
    static const int s_FileIdx[AR_FILE_COUNT] = {
        FILE_1ST_BODYPROG_BIN, FILE_VIN_OPTION_BIN, FILE_VIN_SAVELOAD_BIN
    };
    unsigned char* data[AR_FILE_COUNT] = { NULL, NULL, NULL };
    unsigned int   size[AR_FILE_COUNT] = { 0, 0, 0 };
    int            off[AR_DISC_TEXT_COUNT];
    unsigned int   poolBytes = 0;
    int            loaded    = 0;
    int            f;
    int            i;
    char*          out;

    for (f = 0; f < AR_FILE_COUNT; f++)
    {
        size[f] = (unsigned int)g_FileTable[s_FileIdx[f]].blockCount << 8;
        if (size[f] != 0)
            data[f] = Pc_LangReadDiscFile(g_FileTable[s_FileIdx[f]].startSector, size[f]);
        if (data[f] != NULL && f == AR_FILE_BODYPROG)
            ArDecrypt(data[f], size[f]);
    }

    for (i = 0; i < AR_DISC_TEXT_COUNT; i++)
    {
        const s_ArDiscText* t = &s_ArDiscTexts[i];

        off[i] = -1;
        if (t->disc != s_Active->disc || data[t->file] == NULL)
            continue;
        off[i] = ArResolve(t, data[t->file], size[t->file], s_ArFileBase[t->disc][t->file]);
        if (off[i] >= 0)
            poolBytes += (unsigned int)strlen((const char*)data[t->file] + off[i]) + 1;
    }

    s_ArDiscPool = (char*)malloc(poolBytes ? poolBytes : 1);
    if (s_ArDiscPool != NULL)
    {
        out = s_ArDiscPool;
        for (i = 0; i < AR_DISC_TEXT_COUNT; i++)
        {
            size_t n;

            if (off[i] < 0)
                continue;
            n = strlen((const char*)data[s_ArDiscTexts[i].file] + off[i]) + 1;
            memcpy(out, data[s_ArDiscTexts[i].file] + off[i], n);
            s_ArDiscTr[i] = out;
            out += n;
            loaded++;
        }
    }

    for (f = 0; f < AR_FILE_COUNT; f++)
        free(data[f]);

    SH_LOG("[LANG-AR] %d UI strings read from the disc", loaded);
}

/* The port's own rows, spelled in this disc's cells. A shaped string is never
 * longer than its UTF-8 (2 bytes per letter in, at most 2 cells out). */
static void ArEncodeMenus(void)
{
    unsigned int bytes = 0;
    char*        out;
    int          i;

    for (i = 0; i < AR_MENU_COUNT; i++)
        bytes += (unsigned int)strlen(s_ArMenus[i].ar) + 1;

    s_ArMenuPool = (char*)malloc(bytes);
    if (s_ArMenuPool == NULL)
        return;

    out = s_ArMenuPool;
    for (i = 0; i < AR_MENU_COUNT; i++)
    {
        int cap = (int)strlen(s_ArMenus[i].ar) + 1;

        s_ArMenuTr[i] = out;
        out += Pc_ArEncode(s_ArMenus[i].ar, (int)s_Active->disc, out, cap) + 1;
    }
}

/* Equal as UI text: PAL literals indent continuation lines with tabs and use a
 * real space where the USA build writes '_'; the drawer treats both alike. */
static int ArKeyEqual(const char* a, const char* b)
{
    for (;;)
    {
        while (*a == '\t')
            a++;
        while (*b == '\t')
            b++;
        if (*a == '\0' || *b == '\0')
            return *a == *b;
        if (*a != *b && !((*a == ' ' || *a == '_') && (*b == ' ' || *b == '_')))
            return 0;
        a++;
        b++;
    }
}

void Pc_ArInit(void)
{
    unsigned int   size;
    unsigned char* font;
    unsigned int   hash;
    int            i;

    s_Active = NULL;
    free(s_ArDiscPool);
    s_ArDiscPool = NULL;
    free(s_ArMenuPool);
    s_ArMenuPool = NULL;
    memset((void*)s_ArMenuTr, 0, sizeof(s_ArMenuTr));
    memset((void*)s_ArDiscTr, 0, sizeof(s_ArDiscTr));

    size = (unsigned int)g_FileTable[FILE_1ST_FONT16_TIM].blockCount << 8;
    if (size == 0)
        return;

    font = Pc_LangReadDiscFile(g_FileTable[FILE_1ST_FONT16_TIM].startSector, size);
    if (font == NULL)
        return;

    hash = Fnv1a(font, size);
    free(font);

    for (i = 0; i < (int)(sizeof(s_ArDiscs) / sizeof(s_ArDiscs[0])); i++)
    {
        if (s_ArDiscs[i].fontHash == hash)
        {
            s_Active = &s_ArDiscs[i];
            break;
        }
    }

    if (s_Active == NULL)
        return;

    SH_LOG("[LANG-AR] Arabic %s disc detected (%s), FONT16 %08X", s_Active->region, s_Active->group, hash);
    ArLoadDiscText();
    ArEncodeMenus();
}

int Pc_ArDisc(void)
{
    return s_Active ? (int)s_Active->disc : AR_DISC_NONE;
}

int Pc_ArActive(void)
{
    return s_Active != NULL;
}

int Pc_ArRtl(void)
{
    return s_Active != NULL && s_Active->disc == AR_DISC_USA_MANSOUR;
}

int Pc_ArRtlMark(unsigned int charCode)
{
    return charCode == 0x27 || charCode == 0x2A || charCode == 0x3B || charCode == 0x3E;
}

/* Strings the port formats at run time from numbers alone ("26/66", a
 * resolution, "100%"): no table holds them, and the disc atlases have no 'x'
 * or '%' cell (those bytes are Arabic letters there). The USA drawer walks
 * right to left, so the whole run is reversed to read left to right on
 * screen; every character here is direction-neutral, so that is all it takes.
 * Anything with a letter is left alone. Ring of buffers: callers use the
 * result at once (draw or measure), never keep it. */
static const char* ArNumericText(const char* s)
{
    static char  ring[16][64];
    static int   next;
    char         tmp[64];
    char*        out;
    int          n = 0;
    int          digits = 0;
    const char*  p;
    int          i;
    int          area, scene;
    char         mapId[16];

    /* The New Game start map (PC Options) shows its id, "map0_s00": the
     * letters have no cells, the numbers do. */
    if (sscanf(s, "map%d_s%d", &area, &scene) == 2)
    {
        snprintf(mapId, sizeof(mapId), "%d-%02d", area, scene);
        s = mapId;
    }

    for (p = s; *p != '\0'; p++)
    {
        char c = *p;

        if (c >= '0' && c <= '9')
            digits++;
        else if (!(c == '/' || c == ':' || c == '.' || c == '_' || c == ' ' || c == '-' || c == '+' ||
                   c == '%' || c == 'x' || c == '(' || c == ')' || (unsigned char)c < 0x08))
            return NULL;
    }
    if (digits == 0)
        return NULL;

    for (p = s; *p != '\0' && n < (int)sizeof(tmp) - 3; p++)
    {
        if (*p == '%')
            continue;
        if (*p == 'x')
        {
            tmp[n++] = '_';
            tmp[n++] = '-';
            tmp[n++] = '_';
            continue;
        }
        tmp[n++] = *p;
    }

    out  = ring[next];
    next = (next + 1) % 16;
    if (Pc_ArRtl())
    {
        int lead = 0;

        /* Colour codes stay in front; only the visible run turns around. */
        while (lead < n && (unsigned char)tmp[lead] < 0x08)
        {
            out[lead] = tmp[lead];
            lead++;
        }
        for (i = lead; i < n; i++)
            out[i] = tmp[n - 1 - (i - lead)];
    }
    else
    {
        memcpy(out, tmp, (size_t)n);
    }
    out[n] = '\0';
    return out;
}

const char* Pc_ArMenuText(const char* us)
{
    int i;

    if (s_Active == NULL || us == NULL || us[0] == '\0')
        return NULL;

    for (i = 0; i < AR_DISC_TEXT_COUNT; i++)
    {
        if (s_ArDiscTr[i] != NULL && ArKeyEqual(s_ArDiscTexts[i].us, us))
            return s_ArDiscTr[i];
    }
    for (i = 0; i < AR_MENU_COUNT; i++)
    {
        if (s_ArMenuTr[i] != NULL && ArKeyEqual(s_ArMenus[i].us, us))
            return s_ArMenuTr[i];
    }
    return ArNumericText(us);
}

const char* Pc_ArDiscRegionName(void)
{
    return s_Active ? s_Active->region : "";
}

const char* Pc_ArDiscGroupName(void)
{
    return s_Active ? s_Active->group : "";
}
