#include "lang_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_log.h"

#define PACK_PATH_FMT "gamedata/lang/%s.lang"
#define PACK_MAX_SIZE (4 * 1024 * 1024)
#define COMMON_MSG_COUNT 15 /* map message indices 0-14 come from the shared header */

typedef struct {
    const char* key;
    const char* val;
} s_PackEntry;

static char*        s_Data;
static s_PackEntry* s_Table;
static int          s_Mask;
static int          s_Count;
static int          s_Active;
static char         s_MenuName[32];

/* Map-message keys are named after the map, and e_MapIdx is dense, so a table
 * is both the lookup and the record of which indices actually carry text. */
static const char* const s_MapNames[] = {
    "MAP0_S00", "MAP0_S01", "MAP0_S02",
    "MAP1_S00", "MAP1_S01", "MAP1_S02", "MAP1_S03", "MAP1_S04", "MAP1_S05", "MAP1_S06",
    "MAP2_S00", "MAP2_S01", "MAP2_S02", "MAP2_S03", "MAP2_S04",
    "MAP3_S00", "MAP3_S01", "MAP3_S02", "MAP3_S03", "MAP3_S04", "MAP3_S05", "MAP3_S06",
    "MAP4_S00", "MAP4_S01", "MAP4_S02", "MAP4_S03", "MAP4_S04", "MAP4_S05", "MAP4_S06",
    "MAP5_S00", "MAP5_S01", "MAP5_S02", "MAP5_S03",
    "MAP6_S00", "MAP6_S01", "MAP6_S02", "MAP6_S03", "MAP6_S04", "MAP6_S05",
    "MAP7_S00", "MAP7_S01", "MAP7_S02", "MAP7_S03",
    "MAPT_S00", "MAPX_S00"
};
#define MAP_NAME_COUNT ((int)(sizeof(s_MapNames) / sizeof(s_MapNames[0])))

/* ------------------------------------------------------------------ */
/* UTF-8 -> font bytes                                                 */
/* ------------------------------------------------------------------ */

/* Polish letters get bytes 0xA2..0xB3 -- a window the retail EUR drawer sends
 * to atlas cells past the grid (so it draws nothing) and that no PAL disc
 * string uses. font_region.c maps them back to glyphs. Order must match
 * s_PolishChars there. */
static const unsigned short s_PolishCodepoints[] = {
    0x0105, 0x0107, 0x0119, 0x0142, 0x0144, 0x00F3, 0x015B, 0x017A, 0x017C,
    0x0104, 0x0106, 0x0118, 0x0141, 0x0143, 0x00D3, 0x015A, 0x0179, 0x017B
};
#define POLISH_BYTE_BASE 0xA2

/* Latin letters a pack may carry that this font cannot draw: fall back to the
 * unaccented base rather than dropping the character. */
static unsigned int FallbackAscii(unsigned int cp)
{
    switch (cp)
    {
        case 0x2013: case 0x2014: return '-';  /* en / em dash */
        case 0x2018: case 0x2019: return '\''; /* curly quotes */
        case 0x201C: case 0x201D: return '"';
        case 0x2026: return '.';               /* ellipsis */
        case 0x0104: case 0x0105: return 'a';
        case 0x0106: case 0x0107: return 'c';
        case 0x0118: case 0x0119: return 'e';
        case 0x0141: case 0x0142: return 'l';
        case 0x0143: case 0x0144: return 'n';
        case 0x015A: case 0x015B: return 's';
        case 0x0179: case 0x017A:
        case 0x017B: case 0x017C: return 'z';
        default: return 0;
    }
}

/* Decode one UTF-8 sequence; returns the codepoint and advances *pp. */
static unsigned int Utf8Next(const char** pp)
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

/* Transcode a value in place (output is never longer than the input) and
 * resolve the '\n' / '\t' escapes the pack writer uses to keep one entry per
 * line. Returns the new length. */
static size_t TranscodeValue(char* s)
{
    const char* in  = s;
    char*       out = s;
    int         i;

    while (*in != '\0')
    {
        unsigned int cp;

        if (in[0] == '\\' && (in[1] == 'n' || in[1] == 't'))
        {
            *out++ = (in[1] == 'n') ? '\n' : '\t';
            in += 2;
            continue;
        }

        cp = Utf8Next(&in);

        if (cp < 0x80)
        {
            *out++ = (char)cp;
            continue;
        }

        for (i = 0; i < (int)(sizeof(s_PolishCodepoints) / sizeof(s_PolishCodepoints[0])); i++)
        {
            if (s_PolishCodepoints[i] == cp)
            {
                *out++ = (char)(POLISH_BYTE_BASE + i);
                break;
            }
        }
        if (i < (int)(sizeof(s_PolishCodepoints) / sizeof(s_PolishCodepoints[0])))
            continue;

        /* Latin-1 passes straight through -- the EUR atlas draws those. */
        if (cp >= 0xA0 && cp <= 0xFF)
        {
            *out++ = (char)cp;
            continue;
        }

        cp = FallbackAscii(cp);
        if (cp != 0)
            *out++ = (char)cp;
    }

    *out = '\0';
    return (size_t)(out - s);
}

/* ------------------------------------------------------------------ */
/* Hash table                                                          */
/* ------------------------------------------------------------------ */

static unsigned int HashKey(const char* s)
{
    unsigned int h = 2166136261u;

    for (; *s != '\0'; s++)
    {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void TableInsert(const char* key, const char* val)
{
    unsigned int i = HashKey(key) & (unsigned int)s_Mask;

    while (s_Table[i].key != NULL)
    {
        if (strcmp(s_Table[i].key, key) == 0)
        {
            s_Table[i].val = val; /* last definition wins */
            return;
        }
        i = (i + 1) & (unsigned int)s_Mask;
    }
    s_Table[i].key = key;
    s_Table[i].val = val;
    s_Count++;
}

const char* Pc_LangPackGet(const char* key)
{
    unsigned int i;

    if (!s_Active || key == NULL)
        return NULL;

    i = HashKey(key) & (unsigned int)s_Mask;
    while (s_Table[i].key != NULL)
    {
        if (strcmp(s_Table[i].key, key) == 0)
            return s_Table[i].val;
        i = (i + 1) & (unsigned int)s_Mask;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

void Pc_LangPackFree(void)
{
    free(s_Data);
    free(s_Table);
    s_Data     = NULL;
    s_Table    = NULL;
    s_Count    = 0;
    s_Mask     = 0;
    s_Active   = 0;
    s_MenuName[0] = '\0';
}

int Pc_LangPackLoad(const char* code)
{
    char   path[128];
    FILE*  f;
    long   size;
    char*  p;
    int    cap;
    int    lines = 0;

    Pc_LangPackFree();

    if (code == NULL || code[0] == '\0')
        return 0;

    snprintf(path, sizeof(path), PACK_PATH_FMT, code);
    f = fopen(path, "rb");
    if (f == NULL)
    {
        SH_WARN("[LANGPACK] %s not found", path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > PACK_MAX_SIZE)
    {
        SH_WARN("[LANGPACK] %s has an implausible size (%ld bytes)", path, size);
        fclose(f);
        return 0;
    }

    s_Data = (char*)malloc((size_t)size + 1);
    if (s_Data == NULL || fread(s_Data, 1, (size_t)size, f) != (size_t)size)
    {
        SH_WARN("[LANGPACK] failed to read %s", path);
        fclose(f);
        Pc_LangPackFree();
        return 0;
    }
    fclose(f);
    s_Data[size] = '\0';

    /* Power-of-two table with generous headroom keeps the linear probe short. */
    for (cap = 256; cap < (int)(size / 24) * 2; cap <<= 1)
    {
    }
    s_Mask  = cap - 1;
    s_Table = (s_PackEntry*)calloc((size_t)cap, sizeof(s_PackEntry));
    if (s_Table == NULL)
    {
        Pc_LangPackFree();
        return 0;
    }

    p = s_Data;
    while (*p != '\0')
    {
        char* line = p;
        char* eq;

        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            *p++ = '\0';
        if (line[0] != '\0' && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = '\0';

        lines++;
        if (line[0] == '\0' || line[0] == '#')
            continue;

        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        eq++;

        if (line[0] == '!')
        {
            /* Pack metadata: !code, !name, !menu. */
            if (strcmp(line, "!menu") == 0)
            {
                TranscodeValue(eq);
                snprintf(s_MenuName, sizeof(s_MenuName), "%s", eq);
            }
            continue;
        }

        TranscodeValue(eq);
        TableInsert(line, eq);
    }

    if (s_Count == 0)
    {
        SH_WARN("[LANGPACK] %s parsed to zero entries (%d lines)", path, lines);
        Pc_LangPackFree();
        return 0;
    }

    if (s_MenuName[0] == '\0')
        snprintf(s_MenuName, sizeof(s_MenuName), "%s", code);

    s_Active = 1;
    SH_LOG("[LANGPACK] %s loaded: %d entries, menu label '%s'", path, s_Count, s_MenuName);
    return 1;
}

int Pc_LangPackActive(void)
{
    return s_Active;
}

const char* Pc_LangPackName(void)
{
    return s_MenuName;
}

/* ------------------------------------------------------------------ */
/* Typed lookups                                                       */
/* ------------------------------------------------------------------ */

const char* Pc_LangPackMapMsg(int mapIdx, int msgIdx)
{
    char key[32];

    if (!s_Active || msgIdx < 0)
        return NULL;

    /* Indices 0-14 are the shared map_msg_common.h block, extracted once. */
    if (msgIdx < COMMON_MSG_COUNT)
    {
        snprintf(key, sizeof(key), "COMMON.%d", msgIdx);
        return Pc_LangPackGet(key);
    }

    if (mapIdx < 0 || mapIdx >= MAP_NAME_COUNT)
        return NULL;

    snprintf(key, sizeof(key), "%s.%d", s_MapNames[mapIdx], msgIdx);
    return Pc_LangPackGet(key);
}

const char* Pc_LangPackItemName(int itemIdx)
{
    char key[32];

    if (!s_Active || itemIdx < 0)
        return NULL;

    snprintf(key, sizeof(key), "ITEM_NAME.%d", itemIdx);
    return Pc_LangPackGet(key);
}

const char* Pc_LangPackItemDesc(int itemIdx)
{
    char key[32];

    if (!s_Active || itemIdx < 0)
        return NULL;

    snprintf(key, sizeof(key), "ITEM_DESC.%d", itemIdx);
    return Pc_LangPackGet(key);
}

const char* Pc_LangPackMenu(const char* us)
{
    char   key[160];
    size_t n = 0;
    size_t i;

    if (!s_Active || us == NULL)
        return NULL;

    /* The extractor built these keys by dropping the \x01 kerning bytes and
     * trimming the outer padding; \x07 colour prefixes stay part of the key. */
    key[n++] = 'M';
    key[n++] = 'E';
    key[n++] = 'N';
    key[n++] = 'U';
    key[n++] = '.';

    i = 0;
    while (us[i] == '_' || us[i] == ' ' || us[i] == '\t' || us[i] == '\n' || us[i] == '\r')
        i++;

    for (; us[i] != '\0' && n < sizeof(key) - 1; i++)
    {
        if (us[i] != '\x01')
            key[n++] = us[i];
    }
    while (n > 5 && (key[n - 1] == '_' || key[n - 1] == ' ' ||
                     key[n - 1] == '\t' || key[n - 1] == '\n' || key[n - 1] == '\r'))
    {
        n--;
    }
    key[n] = '\0';

    return Pc_LangPackGet(key);
}
