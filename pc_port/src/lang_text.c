#include "lang_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/map/map.h" /* s_MapOverlayHdr, g_pMapOverlayHeader, e_MapIdx */
#include "main/fileinfo.h"    /* g_GameRegion, Fs_EurFileLookup */
#include "pc_config.h"
#include "sh_log.h"

extern const char* PcPort_GetGameDiscPath(void);

/* Disc layout facts (all probe-verified against SLES-01514):
 * ITEM_<lang>.BIN: 4-byte zero prefix + 195 {namePtr,descPtr} pairs + string
 * pool, pointers linked for base 0x800C8B68 (fileOffset = ptr - base).
 * Map overlays: u32 at FILE offset 0x34 points at the message pointer array;
 * EUR overlays are linked for base 0x800CB370 (fileOffset = ptr - base). */
#define ITEM_TEXT_BASE  0x800C8B68u
#define ITEM_TEXT_COUNT 195
#define EUR_OVL_BASE    0x800CB370u
#define MSG_COUNT_MAX   96
#define MSG_LINES_MAX   9 /* FONT_12X16_LINE_COUNT_MAX — the renderer clips past it */

#define RAW_SECTOR_SIZE 2352
#define SECTOR_DATA_OFF 24
#define SECTOR_DATA_LEN 2048

static const char* s_ItemBinNames[5] = { NULL, "ITEM_GER", "ITEM_FRN", "ITEM_SPN", "ITEM_ITL" };

static char*       s_ItemPool;
static const char* s_ItemNames[ITEM_TEXT_COUNT];
static const char* s_ItemDescs[ITEM_TEXT_COUNT];

static char*           s_MsgPool;
static const char*     s_MsgPtrs[MSG_COUNT_MAX];
static s_MapOverlayHdr s_LangMapHeader;

/* PAL split-page exceptions: a few messages one US entry wide are split into
 * several sequential pages in some languages (probe-verified indices; US
 * index space, langs 1=de 2=fr 3=es 4=it). Parts are joined with ~N; joins
 * over 9 lines lose their tail to the renderer's line cap (FR/IT piano poem
 * — known limitation, candidates for hand-condensed overrides later). */
typedef struct {
    unsigned char mapIdx;
    unsigned char lang;
    unsigned char usIdx;
    unsigned char parts;
} s_LangMsgSplit;

static const s_LangMsgSplit s_MsgSplits[] = {
    { MapIdx_MAP1_S01, 1, 23, 2 }, /* DE: piano poem title + page 1 */
    { MapIdx_MAP1_S01, 2, 23, 2 }, /* FR: same */
    { MapIdx_MAP1_S01, 4, 23, 3 }, /* IT: poem page 1 in three parts */
    { MapIdx_MAP1_S01, 4, 24, 2 }, /* IT: poem page 2 */
    { MapIdx_MAP1_S01, 4, 25, 2 }, /* IT: poem page 3 */
    { MapIdx_MAP1_S03, 3, 22, 2 }, /* ES: poltergeist note */
    { MapIdx_MAP5_S02, 4, 43, 2 }, /* IT: Norman's delivery note */
};

int Pc_LangActive(void)
{
    return g_GameRegion == Region_EUR && g_PcConfig.language >= 1 && g_PcConfig.language <= 4;
}

/* Read a whole file out of the raw-sector disc image. Caller frees. */
static unsigned char* ReadDiscFile(unsigned int sector, unsigned int size)
{
    const char*    path = PcPort_GetGameDiscPath();
    FILE*          f;
    unsigned char* buf;
    unsigned int   done;

    if (!path || !path[0])
        return NULL;

    f = fopen(path, "rb");
    if (!f)
        return NULL;

    buf = (unsigned char*)malloc(size + SECTOR_DATA_LEN);
    for (done = 0; done < size; done += SECTOR_DATA_LEN, sector++)
    {
        fseek(f, (long)sector * RAW_SECTOR_SIZE + SECTOR_DATA_OFF, SEEK_SET);
        if (fread(buf + done, 1, SECTOR_DATA_LEN, f) != SECTOR_DATA_LEN)
        {
            fclose(f);
            free(buf);
            return NULL;
        }
    }
    fclose(f);
    return buf;
}

/* Item text uses the US dialect except that PAL localizations write real
 * spaces; the stock renderer only knows '_'. Accents/newlines pass through
 * (text_draw handles both). */
static char* TranslateItemText(char* dst, const unsigned char* src)
{
    for (; *src != '\0'; src++)
    {
        *dst++ = (*src == ' ') ? '_' : (char)*src;
    }
    *dst++ = '\0';
    return dst;
}

void Pc_LangInit(void)
{
    unsigned int   sector;
    unsigned int   blocks;
    unsigned int   size;
    unsigned char* bin;
    char*          out;
    int            i;

    if (!Pc_LangActive())
        return;

    /* pathIdx 9 = VIN, type 2 = .BIN in the EUR table. */
    if (!Fs_EurFileLookup(s_ItemBinNames[g_PcConfig.language], 9, 2, &sector, &blocks))
    {
        SH_WARN("[LANG] %s not found in the EUR file table", s_ItemBinNames[g_PcConfig.language]);
        return;
    }

    size = blocks << 8;
    bin  = ReadDiscFile(sector, size);
    if (bin == NULL)
    {
        SH_WARN("[LANG] failed to read %s from the disc image", s_ItemBinNames[g_PcConfig.language]);
        return;
    }

    /* Translated strings can only grow by the NUL per entry; 2x is plenty. */
    s_ItemPool = (char*)malloc(size * 2);
    out        = s_ItemPool;

    for (i = 0; i < ITEM_TEXT_COUNT; i++)
    {
        unsigned int namePtr = *(unsigned int*)(bin + 4 + (i * 8));
        unsigned int descPtr = *(unsigned int*)(bin + 8 + (i * 8));

        s_ItemNames[i] = NULL;
        s_ItemDescs[i] = NULL;

        if (namePtr > ITEM_TEXT_BASE && namePtr - ITEM_TEXT_BASE < size)
        {
            s_ItemNames[i] = out;
            out            = TranslateItemText(out, bin + (namePtr - ITEM_TEXT_BASE));
        }
        if (descPtr > ITEM_TEXT_BASE && descPtr - ITEM_TEXT_BASE < size)
        {
            s_ItemDescs[i] = out;
            out            = TranslateItemText(out, bin + (descPtr - ITEM_TEXT_BASE));
        }
    }

    free(bin);
    SH_LOG("[LANG] item text loaded: %s", s_ItemBinNames[g_PcConfig.language]);
}

const char* Pc_LangItemName(int itemIdx)
{
    if (s_ItemPool == NULL || itemIdx < 0 || itemIdx >= ITEM_TEXT_COUNT)
        return NULL;
    /* Empty strings mean "untranslated" on the disc — fall back to English. */
    return (s_ItemNames[itemIdx] && s_ItemNames[itemIdx][0]) ? s_ItemNames[itemIdx] : NULL;
}

const char* Pc_LangItemDesc(int itemIdx)
{
    if (s_ItemPool == NULL || itemIdx < 0 || itemIdx >= ITEM_TEXT_COUNT)
        return NULL;
    return (s_ItemDescs[itemIdx] && s_ItemDescs[itemIdx][0]) ? s_ItemDescs[itemIdx] : NULL;
}

/* ------------------------------------------------------------------ */
/* Map messages                                                        */
/* ------------------------------------------------------------------ */

/* Translate one PAL map message to the US dialect:
 *   {X...}  -> ~X...  (single-letter codes E/D/H/M get a ' ' pad — the US
 *                      parser always consumes one arg byte after the letter)
 *   '\n'    -> "~N "  (US newline code; PAL uses the literal byte)
 *   '\t'    -> dropped (both parsers skip tabs; PAL uses them as indent art)
 *   ' '     -> '_'    (US space stand-in)
 *   >=0x80  -> raw    (Latin-1 accents; resolved by the region font layout)
 * Returns the advanced dst (past the NUL). */
static char* TranslateMapMsg(char* dst, const unsigned char* src, int isFinalPart)
{
    for (; *src != '\0'; src++)
    {
        unsigned char c = *src;

        if (c == '{')
        {
            int  codeLen   = 0;
            char codeFirst = 0;

            *dst++ = '~';
            for (src++; *src != '\0' && *src != '}'; src++)
            {
                if (codeLen == 0)
                    codeFirst = (char)*src;
                *dst++ = (char)*src;
                codeLen++;
            }
            if (*src == '\0')
                break;
            if (codeLen == 1)
                *dst++ = ' ';
            /* The US ~J parsers scan FORWARD for a space/tab after the
             * closing ')' (both the width pre-pass and the already-timed
             * draw path) — without one they run off the string. US data
             * always has a tab there; emit one (tabs are skipped everywhere
             * else). */
            if (codeFirst == 'J')
                *dst++ = '\t';
        }
        else if (c == '\n')
        {
            *dst++ = '~';
            *dst++ = 'N';
            *dst++ = ' ';
        }
        else if (c == '\t')
        {
            /* skip */
        }
        else if (c == ' ')
        {
            *dst++ = '_';
        }
        else
        {
            *dst++ = (char)c;
        }
    }

    (void)isFinalPart;
    *dst++ = '\0';
    return dst;
}

/* Count rendered lines (1 + ~N codes). */
static int MsgLineCount(const char* msg)
{
    int lines = 1;
    for (; *msg; msg++)
    {
        if (msg[0] == '~' && msg[1] == 'N')
            lines++;
    }
    return lines;
}

/* Collapse one blank line ("~N " directly followed by another "~N") in
 * place; returns 1 if one was removed. Used to squeeze joined split-pages
 * under the renderer's 9-line cap before clipping loses real text. */
static int MsgCollapseOneBlankLine(char* msg)
{
    char* p;
    for (p = msg; *p; p++)
    {
        if (p[0] == '~' && p[1] == 'N')
        {
            char* q = p + 2;
            while (*q == ' ' || *q == '_')
                q++;
            if (q[0] == '~' && q[1] == 'N')
            {
                memmove(p, q, strlen(q) + 1);
                return 1;
            }
        }
    }
    return 0;
}

static int SplitPartsFor(int mapIdx, int usIdx)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_MsgSplits) / sizeof(s_MsgSplits[0])); i++)
    {
        if (s_MsgSplits[i].mapIdx == mapIdx && s_MsgSplits[i].lang == g_PcConfig.language &&
            s_MsgSplits[i].usIdx == usIdx)
        {
            return s_MsgSplits[i].parts;
        }
    }
    return 1;
}

void Pc_LangPatchMapMessages(int mapIdx, void* ovl, unsigned int ovlSize)
{
    const unsigned char* bytes = (const unsigned char*)ovl;
    unsigned int         tablePsx;
    unsigned int         tableOff;
    unsigned int         eurPtrs[MSG_COUNT_MAX + 8];
    int                  eurCount;
    int                  usIdx;
    int                  srcIdx;
    char*                out;
    extern s_MapOverlayHdr* g_pMapOverlayHeader;

    if (!Pc_LangActive() || ovl == NULL || g_pMapOverlayHeader == NULL || ovlSize < 0x40)
        return;

    tablePsx = *(const unsigned int*)(bytes + 0x34);
    if (tablePsx < EUR_OVL_BASE || tablePsx - EUR_OVL_BASE >= ovlSize)
    {
        SH_WARN("[LANG] map %d: overlay message table pointer out of range (0x%08X)", mapIdx, tablePsx);
        return;
    }
    tableOff = tablePsx - EUR_OVL_BASE;

    /* Walk the pointer array until a word stops being a valid in-overlay
     * string pointer (matches the table's on-disc terminator). */
    for (eurCount = 0; eurCount < (int)(sizeof(eurPtrs) / sizeof(eurPtrs[0])); eurCount++)
    {
        unsigned int ptr;

        if (tableOff + (eurCount + 1) * 4 > ovlSize)
            break;

        ptr = *(const unsigned int*)(bytes + tableOff + eurCount * 4);
        if (ptr <= EUR_OVL_BASE || ptr - EUR_OVL_BASE >= ovlSize)
            break;

        eurPtrs[eurCount] = ptr - EUR_OVL_BASE;
    }

    if (eurCount < 4)
    {
        SH_WARN("[LANG] map %d: implausible overlay message count %d — keeping English", mapIdx, eurCount);
        return;
    }

    free(s_MsgPool);
    s_MsgPool = (char*)malloc(ovlSize * 2 + 4096);
    out       = s_MsgPool;

    /* EUR index k maps to US index k for k<3; the entry at EUR index 3 is the
     * second half of the split intro message (a "{E}" stub in EN/FR) — join
     * it into US index 2 — and everything after shifts by one. The
     * s_MsgSplits table handles the handful of additional per-language page
     * splits the same way. */
    srcIdx = 0;
    for (usIdx = 0; usIdx < MSG_COUNT_MAX && srcIdx < eurCount; usIdx++)
    {
        int   parts = (usIdx == 2) ? 2 : SplitPartsFor(mapIdx, usIdx);
        int   p;
        char* start = out;

        s_MsgPtrs[usIdx] = start;

        for (p = 0; p < parts && srcIdx < eurCount; p++, srcIdx++)
        {
            if (p > 0)
            {
                /* Re-open after the previous part's NUL with a line break. */
                out    = out - 1;
                *out++ = '~';
                *out++ = 'N';
                *out++ = ' ';
            }
            out = TranslateMapMsg(out, bytes + eurPtrs[srcIdx], p == parts - 1);
        }

        if (parts > 1)
        {
            while (MsgLineCount(start) > MSG_LINES_MAX && MsgCollapseOneBlankLine(start))
            {
            }
            out = start + strlen(start) + 1;
        }
    }

    for (; usIdx < MSG_COUNT_MAX; usIdx++)
    {
        s_MsgPtrs[usIdx] = "";
    }

    s_LangMapHeader             = *g_pMapOverlayHeader;
    s_LangMapHeader.mapMessages = s_MsgPtrs;
    g_pMapOverlayHeader         = &s_LangMapHeader;

    SH_LOG("[LANG] map %d: %d localized messages installed (lang %d)", mapIdx, eurCount, g_PcConfig.language);
}
