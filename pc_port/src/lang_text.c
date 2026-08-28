/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_text.h"
#include "lang_zh.h"        /* Chinese text pack for NTSC-J */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#define _strdup(s) strdup(s)
#endif

#include "game.h"
#include "bodyprog/bodyprog.h" /* g_Font16AtlasImg (FONT16 re-queue on live switch) */
#include "bodyprog/map/map.h" /* s_MapOverlayHdr, g_pMapOverlayHeader, e_MapIdx */
#include "bodyprog/text/text_draw.h" /* MAP_MSG_CODE_PAGE */
#include "font_region.h"      /* g_FontLayout, Font_SetGlyphWidths (fan-patch kerning) */
#include "lang_pack.h"        /* PC-side language packs (gamedata/lang) */
#include "lang_ru.h"          /* Russian fan-patch detection + menu text */
#include "pc_kanji.h"         /* Pc_KanjiSetChinese (NTSC-J glyph set) */
#include "lang_jpn.h"         /* NTSC-J menu text from the disc overlays */
#include "main/fsqueue.h"     /* Fs_QueueStartReadTim, FS_BUFFER_1 */
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
#define JPN_OVL_BASE    0x800CBBD0u /* JAP Rev 1/2 map-overlay link base (probe-verified) */
#define USA_OVL_BASE    0x800C9578u /* US map-overlay link base (configs/USA/maps vram) */

/* Fan-translation (modified USA disc) support. Patches like the Spanish
 * fandub edit the US disc in place: story text in the VIN overlays, item
 * names/descriptions + the FONT16 kerning table inside the encrypted
 * BODYPROG.BIN, accent glyphs repainted over rarely-used FONT16 cells. The
 * XA voice dub and every TIM stream from the disc already; text is the only
 * thing the port compiles in, so it is re-read from the disc and installed
 * whenever it differs from the compiled originals (a matching decompile —
 * compiled text == vanilla disc text, so vanilla discs are a guaranteed
 * no-op). Addresses from configs/USA/sym.bodyprog.txt / bodyprog.yaml. */
#define USA_BODY_VRAM      0x80024B60u
#define USA_ITEM_NAME_ADDR 0x800ADB60u /* INVENTORY_ITEM_NAMES, 195 ptrs */
#define USA_ITEM_DESC_ADDR 0x800ADE6Cu /* g_ItemDescriptions, 195 ptrs */
#define USA_WIDTHS_ADDR    0x80025D6Cu /* FONT_12X16_GLYPH_WIDTHS, 84 bytes */

/* Same idea one region over. A PAL fan patch (consolgames.ru) repaints the
 * 126-cell EUR atlas with Cyrillic and retunes the kerning table to match — 52
 * of the 120 advances differ from retail — so rendering it with the compiled
 * retail widths mis-spaces every line of text on the disc, subtitles included.
 * Addresses from configs/EUR/bodyprog.yaml; the widths sit at the same file
 * offset as the US table by coincidence, not by construction. */
#define EUR_BODY_VRAM      0x80025690u
#define EUR_WIDTHS_ADDR    0x8002689Cu /* FONT_12X16_GLYPH_WIDTHS, 120 bytes */

/* And once more for NTSC-J, where the Chinese fan translation lives. Its item
 * names and descriptions are rewritten in BODYPROG exactly like a USA fan
 * patch's are, so the same adopt-when-it-differs read applies — on a stock JP
 * disc the compiled tables and the disc agree (matching decompile) and nothing
 * happens. Addresses from configs/JAP1/sym.bodyprog.txt; the load base is the
 * same 0x80024B60 the US overlay uses. */
#define JPN_BODY_VRAM      0x80024B60u
#define JPN_ITEM_NAME_ADDR 0x800B0044u /* INVENTORY_ITEM_NAMES, 195 ptrs */
#define JPN_ITEM_DESC_ADDR 0x800B0350u /* g_ItemDescriptions, 195 ptrs */
#define JPN_WIDTHS_ADDR    0x80025D64u /* FONT_12X16_GLYPH_WIDTHS, 84 bytes */
/* Must cover the largest per-map message table: MAP7_S02 has 159 US entries.
 * (Was 96 — the replaced pointer array under-covered MAP4_S01/MAP7_S01/
 * MAP7_S02 on PAL, sending reads past it.) */
#define MSG_COUNT_MAX   176
#define MSG_PARTS_MAX   8 /* largest s_MsgSplits `parts` is 3 */

#define RAW_SECTOR_SIZE 2352
#define SECTOR_DATA_OFF 24
#define SECTOR_DATA_LEN 2048

static const char* s_ItemBinNames[5] = { "ITEM_ENG", "ITEM_GER", "ITEM_FRN", "ITEM_SPN", "ITEM_ITL" };

/* The compiled item tables a disc's own text is compared against. */
extern const char* INVENTORY_ITEM_NAMES[];
extern const char* g_ItemDescriptions[];

/* Language ids in options-menu order. 0-4 are the PAL disc's own languages;
 * from LANG_PACK_FIRST on they are PC-side packs (gamedata/lang/<id>.lang),
 * which exist on no disc and so are offered on EUR only -- the US font has no
 * accent cells to build their letters into. */
const char* const s_LangIds[LANG_COUNT] = { "en", "de", "fr", "es", "it", "pl" };

static char*       s_ItemPool;
/* "item text has been installed" sentinel. Was s_ItemPool != NULL, but the
 * NTSC-J path installs pointers to COMPILED tables and allocates no pool, so
 * the pool alone would report the Japanese text as absent. */
static int         s_ItemTextReady;
static const char* s_ItemNames[ITEM_TEXT_COUNT];
static const char* s_ItemDescs[ITEM_TEXT_COUNT];

static char*           s_MsgPool;
static const char*     s_MsgPtrs[MSG_COUNT_MAX];
static s_MapOverlayHdr s_LangMapHeader;

/* PAL split-page exceptions: a few messages one US entry wide are split into
 * several sequential pages in some languages (probe-verified indices; US
 * index space, langs 1=de 2=fr 3=es 4=it). Parts are joined with ~N so the
 * compiled map code's US indices still resolve; a join too tall for one
 * rendered page keeps the disc's own page breaks instead (~P — see the join
 * loop). Before that, an over-cap message could not be dismissed at all. */
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

/* NTSC-J: US->JAP message index mapping for the maps whose tables differ. */
#include "lang_jpn_msgmap.inc"

/* NTSC-J inventory text. The decomp carries the Japanese tables but selects them
 * with VERSION_REGION_IS(NTSCJ) at COMPILE time; the port builds one binary and
 * picks the region at runtime, so they are compiled alongside the English ones
 * and installed for Region_JPN below. */
#include "lang_jpn_items.inc"

int Pc_LangActive(void)
{
    /* Any language INCLUDING English: PAL-EN is its own retranslation
     * ("Take them?" vs the US "Take_it?"), so a PAL disc always shows its
     * own text rather than the compiled US strings. */
    return g_GameRegion == Region_EUR;
}

static int s_FanTextActive;

/* Set once the disc's item text is recognised as the Chinese patch. Until then
 * nothing may claim this disc can render Chinese. */
static int s_JpnDiscIsChinese;

/* Set when zh.pack is supplying the words instead. Read by the map-message
 * installer, which must then take its strings from the pack and not the
 * overlay — the disc is Japanese in that case. */
static int s_ZhPackActive;

int Pc_LangZhPackActive(void)
{
    return s_ZhPackActive;
}

int Pc_LangJpnDiscIsChinese(void)
{
    return s_JpnDiscIsChinese;
}

int Pc_FanTextActive(void)
{
    return s_FanTextActive;
}

/* Read a whole file out of the raw-sector disc image. Caller frees. */
unsigned char* Pc_LangReadDiscFile(unsigned int sector, unsigned int size)
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

/* The game's own Fs_DecryptOverlay LCG (1ST/ overlays are XOR-obfuscated on
 * disc; one continuous keystream over the whole file, per LE u32). */
static void DecryptOverlay(unsigned char* data, unsigned int size)
{
    unsigned int seed = 0;
    unsigned int i;

    for (i = 0; i + 4 <= size; i += 4)
    {
        unsigned int w = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) | ((unsigned int)data[i + 3] << 24);

        seed = (seed + 0x01309125u) * 0x03A452F7u;
        w ^= seed;
        data[i]     = (unsigned char)w;
        data[i + 1] = (unsigned char)(w >> 8);
        data[i + 2] = (unsigned char)(w >> 16);
        data[i + 3] = (unsigned char)(w >> 24);
    }
}

/* Parse the {name,desc} pointer arrays at the given BODYPROG file offsets,
 * resolving pointers against `base`, and install them into s_ItemNames/
 * s_ItemDescs when they differ from the compiled US text. Both arrays must
 * parse cleanly before anything is installed. Returns 1 if adopted, 0 if the
 * text matched the compiled originals (vanilla no-op), -1 on a parse error. */
static int AdoptItemArrays(const unsigned char* bin, unsigned int size,
                           unsigned int base, unsigned int nameOff, unsigned int descOff,
                           const char* const* cmpNames, const char* const* cmpDescs)
{
    const char*  fanNames[ITEM_TEXT_COUNT];
    const char*  fanDescs[ITEM_TEXT_COUNT];
    unsigned int poolBytes = 0;
    int          diff      = 0;
    int          i;
    char*        out;

    if (nameOff + ITEM_TEXT_COUNT * 4 > size || descOff + ITEM_TEXT_COUNT * 4 > size)
        return -1;

    for (i = 0; i < ITEM_TEXT_COUNT; i++)
    {
        unsigned int nameAddr = *(const unsigned int*)(bin + nameOff + i * 4);
        unsigned int descAddr = *(const unsigned int*)(bin + descOff + i * 4);
        const char*  compiled;

        fanNames[i] = NULL;
        fanDescs[i] = NULL;

        if (nameAddr != 0)
        {
            if (nameAddr <= base || nameAddr - base >= size)
                return -1;
            fanNames[i] = (const char*)(bin + (nameAddr - base));
            if (memchr(fanNames[i], '\0', size - (nameAddr - base)) == NULL)
                return -1;
            poolBytes += (unsigned int)strlen(fanNames[i]) + 1;
        }
        if (descAddr != 0)
        {
            if (descAddr <= base || descAddr - base >= size)
                return -1;
            fanDescs[i] = (const char*)(bin + (descAddr - base));
            if (memchr(fanDescs[i], '\0', size - (descAddr - base)) == NULL)
                return -1;
            poolBytes += (unsigned int)strlen(fanDescs[i]) + 1;
        }

        compiled = cmpNames[i];
        if ((compiled == NULL) != (fanNames[i] == NULL) ||
            (compiled != NULL && strcmp(compiled, fanNames[i]) != 0))
        {
            diff = 1;
        }
        compiled = cmpDescs[i];
        if ((compiled == NULL) != (fanDescs[i] == NULL) ||
            (compiled != NULL && strcmp(compiled, fanDescs[i]) != 0))
        {
            diff = 1;
        }
    }

    if (!diff)
        return 0;

    /* 4MB cap: legitimate item text totals ~20KB — anything bigger is a
     * malformed disc aiming every pointer at a huge shared string. */
    if (poolBytes > (1u << 22))
        return -1;
    s_ItemPool = (char*)malloc(poolBytes + 1);
    if (s_ItemPool == NULL)
        return -1;
    out = s_ItemPool;
    s_ItemTextReady = 1;
    for (i = 0; i < ITEM_TEXT_COUNT; i++)
    {
        /* Already US dialect (underscores, \n\t layout) — copy verbatim. */
        if (fanNames[i] != NULL)
        {
            s_ItemNames[i] = out;
            memcpy(out, fanNames[i], strlen(fanNames[i]) + 1);
            out += strlen(fanNames[i]) + 1;
        }
        else
        {
            s_ItemNames[i] = NULL;
        }
        if (fanDescs[i] != NULL)
        {
            s_ItemDescs[i] = out;
            memcpy(out, fanDescs[i], strlen(fanDescs[i]) + 1);
            out += strlen(fanDescs[i]) + 1;
        }
        else
        {
            s_ItemDescs[i] = NULL;
        }
    }
    return 1;
}

/* 1 if bin[so..] is a clean single-line item name: printable ASCII, non-empty,
 * NUL-terminated within maxLen. Says nothing about where the string starts —
 * see IsNameBoundary. */
static int IsPlaintextName(const unsigned char* bin, unsigned int size, unsigned int so, unsigned int maxLen)
{
    unsigned int j;

    if (so >= size)
        return 0;
    for (j = 0; j < maxLen && so + j < size; j++)
    {
        unsigned char c = bin[so + j];

        if (c == 0)
            return j > 0;
        if (c < 0x20 || c > 0x7e)
            return 0;
    }
    return 0;
}

/* 1 if the pointer lands on a string boundary (the byte before it is a NUL).
 * This is what pins the link base: at an off-by-a-few base every name still
 * reads as printable text but lands mid-string. Counted rather than required
 * per pointer, because a patch's own pool can legitimately alias — FireCross
 * dropped the terminator between two item names, so one entry points into the
 * middle of its neighbour and every other entry is clean. */
static int IsNameBoundary(const unsigned char* bin, unsigned int so)
{
    return so == 0 || bin[so - 1] == 0;
}

/* Locate a relinked, UNENCRYPTED item-name pointer array in a rebuilt disc's
 * BODYPROG (Brazilian re-translation). The item set is unchanged from US, so
 * the array's set/empty slot pattern matches the compiled build exactly, and
 * every non-empty pointer must resolve to a clean name; nearly all of them must
 * also start on a string boundary, which is what pins `base` exactly (a base
 * off by a few bytes still yields printable text, but from mid-string, so the
 * boundary count collapses). An encrypted vanilla/in-place disc has no such
 * array in the raw bytes (all of it fails on random data), so this returns 0
 * for them and the caller decrypts as before. On success, writes the detected
 * link base and name-array file offset. */
#define ITEM_BOUNDARY_NUM 15 /* accept at 15/16 clean: see IsNameBoundary */
#define ITEM_BOUNDARY_DEN 16
static int BrazilFindItemArray(const unsigned char* bin, unsigned int size,
                               unsigned int* outBase, unsigned int* outNameOff)
{
    unsigned char sig[ITEM_TEXT_COUNT];
    unsigned int  usNameOff = USA_ITEM_NAME_ADDR - USA_BODY_VRAM;
    unsigned int  offLo     = (usNameOff > 0x800) ? usNameOff - 0x800 : 0;
    unsigned int  offHi     = usNameOff + 0x400;
    unsigned int  base;
    unsigned int  nameOff;
    int           i;

    for (i = 0; i < ITEM_TEXT_COUNT; i++)
        sig[i] = (INVENTORY_ITEM_NAMES[i] != NULL);

    for (base = USA_BODY_VRAM - 0x800; base <= USA_BODY_VRAM + 0x400; base += 4)
    {
        for (nameOff = offLo; nameOff <= offHi && nameOff + ITEM_TEXT_COUNT * 4 <= size; nameOff += 4)
        {
            int ok       = 1;
            int setCount = 0;
            int onBound  = 0;

            for (i = 0; i < ITEM_TEXT_COUNT; i++)
            {
                unsigned int p = *(const unsigned int*)(bin + nameOff + i * 4);

                if (!sig[i])
                {
                    if (p != 0)
                    {
                        ok = 0;
                        break;
                    }
                    continue;
                }
                if (p <= base || p - base >= size || !IsPlaintextName(bin, size, p - base, 48))
                {
                    ok = 0;
                    break;
                }
                setCount++;
                onBound += IsNameBoundary(bin, p - base);
            }
            if (ok && onBound * ITEM_BOUNDARY_DEN < setCount * ITEM_BOUNDARY_NUM)
            {
                ok = 0;
            }
            if (ok)
            {
                *outBase    = base;
                *outNameOff = nameOff;
                return 1;
            }
        }
    }
    return 0;
}

/* Read BODYPROG.BIN off the active USA disc and adopt its kerning table and
 * item name/description arrays when they differ from the compiled originals. */
static void FanTextInit(void)
{
    /* Compiled US kerning table, captured before any override so re-runs
     * (the title-screen Language row re-enters via Pc_LangSetLanguage)
     * keep comparing against the pristine baseline. */
    static const unsigned char* s_compiledWidths;

    unsigned int   size = (unsigned int)g_FileTable[FILE_1ST_BODYPROG_BIN].blockCount << 8;
    unsigned char* bin  = Pc_LangReadDiscFile(g_FileTable[FILE_1ST_BODYPROG_BIN].startSector, size);
    unsigned int   brBase;
    unsigned int   brNameOff;
    int            i;

    if (s_compiledWidths == NULL)
        s_compiledWidths = g_FontLayout->glyphWidths;

    s_FanTextActive = 0;
    if (bin == NULL)
        return;

    /* A rebuilt disc (Brazilian re-translation) does NOT encrypt BODYPROG and
     * relinks the item pointer arrays to a shifted base. Detect + adopt them
     * straight from the raw bytes before any decryption — decrypting the
     * plaintext would corrupt it. Encrypted vanilla/in-place discs have no such
     * array in the raw bytes, so this fails and the original decrypt path below
     * runs unchanged. Their VIN map messages stay US-linked either way and
     * translate through UsaPatchMapMessages. */
    if (BrazilFindItemArray(bin, size, &brBase, &brNameOff))
    {
        int r = AdoptItemArrays(bin, size, brBase, brNameOff, brNameOff + ITEM_TEXT_COUNT * 4,
                                INVENTORY_ITEM_NAMES, g_ItemDescriptions);

        if (r == 1)
        {
            s_FanTextActive = 1;
            SH_LOG("[FANPATCH] rebuilt-disc item text adopted (base 0x%08X, name off 0x%X)", brBase, brNameOff);
        }
        else if (r < 0)
        {
            SH_WARN("[FANPATCH] rebuilt-disc item arrays did not parse — keeping compiled item text");
        }
        free(bin);
        return;
    }

    DecryptOverlay(bin, size);

    /* A fan patch that edits BODYPROG in place (Spanish fandub) keeps the
     * kerning table and item-pointer arrays at their US symbol offsets. A patch
     * that REBUILDS BODYPROG but stays encrypted would relocate them, so those
     * offsets land on unrelated bytes and adopting them explodes every glyph /
     * installs garbage item names. Real FONT_12X16 advances never exceed the
     * 16px cell; if the bytes at the US widths offset do, this disc's BODYPROG
     * is not US-linked — keep every compiled US table. */
    for (i = 0; i < 84; i++)
    {
        if (bin[(USA_WIDTHS_ADDR - USA_BODY_VRAM) + i] > 16)
        {
            SH_WARN("[FANPATCH] BODYPROG not US-linked (rebuilt disc) — keeping compiled font/item text");
            free(bin);
            return;
        }
    }

    if (memcmp(bin + (USA_WIDTHS_ADDR - USA_BODY_VRAM), s_compiledWidths, 84) != 0)
    {
        Font_SetGlyphWidths(bin + (USA_WIDTHS_ADDR - USA_BODY_VRAM), 84);
        s_FanTextActive = 1;
        SH_LOG("[FANPATCH] modified FONT16 kerning table adopted from disc");
    }

    switch (AdoptItemArrays(bin, size, USA_BODY_VRAM,
                            USA_ITEM_NAME_ADDR - USA_BODY_VRAM,
                            USA_ITEM_DESC_ADDR - USA_BODY_VRAM,
                            INVENTORY_ITEM_NAMES, g_ItemDescriptions))
    {
        case 1:
            s_FanTextActive = 1;
            SH_LOG("[FANPATCH] modified item text adopted from disc");
            break;
        case -1:
            SH_WARN("[FANPATCH] BODYPROG item arrays did not parse — keeping compiled item text");
            break;
        default:
            break;
    }

    free(bin);
}

/* Adopt a PAL fan patch's retuned FONT16 kerning table. Mirrors the USA branch
 * of FanTextInit: the widths are only taken when they differ from the compiled
 * retail table (so a stock EUR disc is a guaranteed no-op) and only when they
 * are plausible advances for a 16px cell, which is what separates an in-place
 * patch from a rebuilt BODYPROG whose tables moved. Item and story text need
 * nothing extra here — both already come off the disc on EUR. */
static void EurFanFontInit(void)
{
    static const unsigned char* s_compiledWidths;

    unsigned int   size = (unsigned int)g_FileTable[FILE_1ST_BODYPROG_BIN].blockCount << 8;
    unsigned int   off  = EUR_WIDTHS_ADDR - EUR_BODY_VRAM;
    unsigned char* bin;
    int            count;
    int            i;

    if (s_compiledWidths == NULL)
        s_compiledWidths = g_FontLayout->glyphWidths;

    if (size < off + FONT_ATLAS_CELL_MAX)
        return;

    bin = Pc_LangReadDiscFile(g_FileTable[FILE_1ST_BODYPROG_BIN].startSector, size);
    if (bin == NULL)
        return;

    DecryptOverlay(bin, size);

    /* Real advances never exceed the 16px cell. Garbage here means BODYPROG was
     * rebuilt and its tables moved, so nothing at this offset can be trusted. */
    for (i = 0; i < FONT_ATLAS_CELL_MAX; i++)
    {
        if (bin[off + i] > 16)
        {
            SH_WARN("[FANPATCH] EUR BODYPROG not retail-linked — keeping compiled font widths");
            free(bin);
            return;
        }
    }

    /* A non-zero advance in one of retail's six blank cells is the disc telling
     * us it painted glyphs there (consolgames.ru puts ъ ы ь э ю я in them). */
    count = 120;
    for (i = 120; i < FONT_ATLAS_CELL_MAX; i++)
    {
        if (bin[off + i] != 0)
        {
            count = FONT_ATLAS_CELL_MAX;
            break;
        }
    }

    if (count != g_FontLayout->glyphCount ||
        memcmp(bin + off, s_compiledWidths, 120) != 0)
    {
        Font_SetGlyphWidths(bin + off, count);
        SH_LOG("[FANPATCH] modified EUR FONT16 kerning table adopted from disc (%d glyphs)", count);
    }

    free(bin);
}

/* Adopt a fan-translated NTSC-J disc's item text and kerning. Mirrors the USA
 * branch of FanTextInit against the JP symbol addresses and the compiled JP
 * tables: the Chinese translation rewrites BODYPROG in place (it keeps the
 * game's own kuten codes and only redefines the glyphs drawn at them), so the
 * arrays are still JP-linked and only their strings differ. Map/story text
 * needs nothing here — that already comes off the disc on NTSC-J. */
/* Fingerprint of the adopted NTSC-J item text: FNV-1a over item names 0..7,
 * each NUL-terminated. Measured offline from the discs themselves —
 *
 *   0x5FE7A885  retail SLPM-86192 (栄養剤 / 携帯用救急キット / アンプル)
 *   0xECCF3760  the goro / 十三月 2006 Chinese patch (PPF applied to that disc)
 *
 * The Chinese patch does not write Chinese; it writes JIS kuten codes whose
 * GLYPHS its custom BIOS redefines. Its item names come out as runs like
 * 0x89B3 0x89B4 0x89B5 — sequential nonsense as Japanese, which is exactly what
 * makes them a reliable signature. */
#define JPN_ITEMS_FNV_CHINESE 0xECCF3760u

static unsigned int JpnItemTextFingerprint(void)
{
    unsigned int h = 2166136261u;
    int          i;

    for (i = 0; i < 8; i++)
    {
        const char* p = s_ItemNames[i];

        if (p != NULL)
        {
            for (; *p != '\0'; p++)
            {
                h ^= (unsigned char)*p;
                h *= 16777619u;
            }
        }
        h ^= 0u;
        h *= 16777619u;
    }
    return h;
}

/* Returns 1 only if the disc's own item text replaced the compiled tables. */
static int JpnFanTextInit(void)
{
    static const unsigned char* s_compiledWidths;

    unsigned int   size = (unsigned int)g_FileTable[FILE_1ST_BODYPROG_BIN].blockCount << 8;
    unsigned char* bin;
    int            i;
    int            adopted;

    if (s_compiledWidths == NULL)
        s_compiledWidths = g_FontLayout->glyphWidths;

    if (size < (JPN_ITEM_DESC_ADDR - JPN_BODY_VRAM) + ITEM_TEXT_COUNT * 4)
        return 0;

    bin = Pc_LangReadDiscFile(g_FileTable[FILE_1ST_BODYPROG_BIN].startSector, size);
    if (bin == NULL)
        return 0;

    DecryptOverlay(bin, size);

    /* Real FONT_12X16 advances never exceed the 16px cell; garbage here means
     * this BODYPROG is not JP-linked and nothing at these offsets is safe. */
    for (i = 0; i < 84; i++)
    {
        if (bin[(JPN_WIDTHS_ADDR - JPN_BODY_VRAM) + i] > 16)
        {
            SH_WARN("[FANPATCH] NTSC-J BODYPROG not JP-linked — keeping compiled font/item text");
            free(bin);
            return 0;
        }
    }

    if (memcmp(bin + (JPN_WIDTHS_ADDR - JPN_BODY_VRAM), s_compiledWidths, 84) != 0)
    {
        Font_SetGlyphWidths(bin + (JPN_WIDTHS_ADDR - JPN_BODY_VRAM), 84);
        SH_LOG("[FANPATCH] modified NTSC-J FONT16 kerning table adopted from disc");
    }

    adopted = 0;
    switch (AdoptItemArrays(bin, size, JPN_BODY_VRAM,
                            JPN_ITEM_NAME_ADDR - JPN_BODY_VRAM,
                            JPN_ITEM_DESC_ADDR - JPN_BODY_VRAM,
                            INVENTORY_ITEM_NAMES_JPN, ITEM_DESCRIPTIONS_JPN))
    {
        case 1:
            SH_LOG("[FANPATCH] modified NTSC-J item text adopted from disc");
            adopted = 1;
            if (JpnItemTextFingerprint() == JPN_ITEMS_FNV_CHINESE)
            {
                /* The disc IS the Chinese release, so turn its glyph set on
                 * without making the player find the language row: on this disc
                 * Japanese glyphs would render the same codes as gibberish, so
                 * Chinese is not a preference here, it is the correct reading. */
                s_JpnDiscIsChinese = 1;
                if (!g_PcConfig.jpLanguage)
                {
                    g_PcConfig.jpLanguage = 1;
                    SH_LOG("[LANG] Chinese-patched NTSC-J disc detected — selecting Chinese");
                }
            }
            break;
        case -1:
            SH_WARN("[FANPATCH] NTSC-J BODYPROG item arrays did not parse — keeping compiled item text");
            break;
        default:
            break;
    }

    free(bin);
    return adopted;
}

void Pc_LangInit(void)
{
    unsigned int   sector;
    unsigned int   blocks;
    unsigned int   size;
    unsigned char* bin;
    char*          out;
    int            i;

    /* Re-entrant: the title-screen options menu re-runs this on a language
     * switch. Dropping the pool makes Pc_LangItemName/Desc fall back to the
     * compiled US strings until (unless) a new table loads below. */
    free(s_ItemPool);
    s_ItemPool = NULL;

    /* Clean slate for the layout logic below: a previous run's fan-patch
     * override or Polish layout must not survive into this one (see
     * Font_ResetLayout for the switch-away-and-back fold-up it caused). */
    Font_ResetLayout();

    /* A PAL fan patch's retuned kerning has to be in before anything measures
     * a string. (The USA equivalent rides along with item text in FanTextInit.) */
    if (g_GameRegion == Region_EUR)
    {
        EurFanFontInit();
    }

    /* Russian patches repaint the font's Latin cells with Cyrillic, which turns
     * every compiled English menu string into nonsense. Detect them and swap in
     * the port's own Russian text, encoded in that patch's charset. */
    Pc_RuInit();

    /* A PC-side pack language (no disc carries it) serves item and story text
     * from gamedata/lang instead of the disc. EUR only: the glyphs for its
     * extra letters are built into the EUR FONT16 atlas (font_region.c), and
     * the US atlas has no accent cells to hold them. The per-map English
     * overlay walk (still EUR) supplies the count and untranslated fallback,
     * so the disc path below is left entirely alone. */
    /* A PC-side pack on a Russian disc would fight it: the pack's byte window
     * (0xA2-0xB3) is Cyrillic letters there, and Font_PatchPolishGlyphs paints
     * its letterforms over atlas cells the repaint already uses. The disc wins,
     * so fall back to the disc's own language slot before anything indexes on it. */
    if (Pc_RuActive() && g_PcConfig.language >= LANG_PACK_FIRST)
    {
        g_PcConfig.language = 0;
    }

    if (g_PcConfig.language >= LANG_PACK_FIRST)
    {
        if (g_GameRegion == Region_EUR && Pc_LangPackLoad(s_LangIds[g_PcConfig.language]))
        {
            Font_UsePolishLayout();
            /* Item name/desc come from the pack (Pc_LangItemName checks it
             * first); untranslated entries fall back to the compiled US
             * strings in item_screens_3.c. Story text is overlaid per map on
             * the English EUR overlay walk below. No disc item load needed. */
            return;
        }
        SH_WARN("[LANG] pack '%s' unavailable (region=%d) — English",
                s_LangIds[g_PcConfig.language], (int)g_GameRegion);
        g_PcConfig.language = 0;
    }
    else
    {
        Pc_LangPackFree();
    }

    if (g_GameRegion == Region_USA)
    {
        FanTextInit();
        return;
    }

    /* NTSC-J ships Japanese item names/descriptions; retail selects them at
     * compile time, so install the compiled JP tables here instead. Story text
     * is handled separately by the SJIS map-message path. */
    if (g_GameRegion == Region_JPN)
    {
        int i;

        /* Chinese draws from a second glyph set over the same kuten codes. */
        Pc_KanjiSetChinese(g_PcConfig.jpLanguage);

        for (i = 0; i < ITEM_TEXT_COUNT; i++)
        {
            s_ItemNames[i] = INVENTORY_ITEM_NAMES_JPN[i];
            s_ItemDescs[i] = ITEM_DESCRIPTIONS_JPN[i];
        }
        s_ItemTextReady = 1;
        SH_LOG("[LANG] NTSC-J inventory text installed (%d entries)", ITEM_TEXT_COUNT);

        /* A fan-translated JP disc (the Chinese patch) rewrites those same
         * arrays; adopting them replaces the pointers just installed. */
        JpnFanTextInit();

        /* Chinese wanted, but this disc is not the Chinese release: take the
         * words from zh.pack instead. That is the whole point of the pack —
         * the port already has the glyphs, so with the text supplied a RETAIL
         * Japanese disc can show Chinese and switch to it in game. */
        s_ZhPackActive = 0;
        if (g_PcConfig.jpLanguage && !s_JpnDiscIsChinese && Pc_LangZhAvailable())
        {
            int installed = 0;

            for (i = 0; i < ITEM_TEXT_COUNT; i++)
            {
                const char* nm = Pc_LangZhItemName(i);
                const char* ds = Pc_LangZhItemDesc(i);

                /* Only over entries the pack actually carries: the JP tables
                 * are mostly NULL (78 of 195 are real items) and a NULL there
                 * is meaningful — it is what the inventory tests. */
                if (nm != NULL)
                {
                    s_ItemNames[i] = nm;
                    installed++;
                }
                if (ds != NULL)
                {
                    s_ItemDescs[i] = ds;
                }
            }
            s_ItemTextReady = 1;
            s_ZhPackActive  = 1;
            SH_LOG("[LANG-ZH] Chinese text pack installed (%d inventory entries) — "
                   "no disc patch needed", installed);
        }

        if (g_PcConfig.jpLanguage && !s_JpnDiscIsChinese && !s_ZhPackActive)
        {
            /* Chinese is a GLYPH swap over unchanged kuten codes, so it only
             * spells Chinese if the disc supplied Chinese strings. Left on over
             * the compiled Japanese text every code draws some unrelated
             * character — reported as "text is corrupted when CN is selected",
             * which is what a retail JP disc (or a CN disc whose BODYPROG did
             * not parse) does. Japanese glyphs over Japanese text at least
             * reads correctly. */
            Pc_KanjiSetChinese(0);
            SH_WARN("[LANG] Chinese selected but neither this disc nor gamedata/lang/"
                    "zh.pack carries Chinese text — using Japanese glyphs.");
        }
        Pc_JpnMenuInit();
        return;
    }

    if (!Pc_LangActive())
        return;

    /* pathIdx 9 = VIN, type 2 = .BIN in the EUR table. */
    if (!Fs_EurFileLookup(s_ItemBinNames[g_PcConfig.language], 9, 2, &sector, &blocks))
    {
        SH_WARN("[LANG] %s not found in the EUR file table", s_ItemBinNames[g_PcConfig.language]);
        return;
    }

    size = blocks << 8;
    bin  = Pc_LangReadDiscFile(sector, size);
    if (bin == NULL)
    {
        SH_WARN("[LANG] failed to read %s from the disc image", s_ItemBinNames[g_PcConfig.language]);
        return;
    }

    /* Translated strings can only grow by the NUL per entry; 2x is plenty. */
    s_ItemPool = (char*)malloc(size * 2);
    out        = s_ItemPool;
    s_ItemTextReady = 1;

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

/* Live language switch — title-screen options menu only (no map loaded, so
 * nothing already-extracted goes stale; the next New Game/Load/demo picks
 * everything up through the normal per-map path). Persists the config key,
 * rebinds the file table and reloads item text. */
void Pc_LangSetLanguage(int lang)
{
    if (lang < 0 || lang >= LANG_COUNT)
        lang = 0;

    g_PcConfig.language = lang;
    PcConfig_SaveKeyValue("language", s_LangIds[lang]);
    /* Pack languages fall through to the English disc assets (the redirect
     * only knows the five disc languages), which is what they want: the pack
     * supplies the text, the disc supplies everything else. */
    Fs_ApplyLanguageRedirects();
    Pc_LangInit();

    /* A pack's built letters (ą ę ł ż + caps) live in atlas cells patched onto
     * the FONT16 pixels at upload time (font_region.c). FONT16 was uploaded at
     * boot, before this live switch, so those cells are still blank — re-queue
     * the atlas so Font_PatchPolishGlyphs runs against the now-active pack.
     * (Composed accents like ć/ś/ó reuse vanilla cells and were fine already.)
     * The options menu drains the queue before it redraws, so the glyphs are
     * in VRAM by the next frame. EUR only — pack languages need that atlas. */
    if (g_GameRegion == Region_EUR)
    {
        Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
    }

    SH_LOG("[LANG] language switched to '%s'", s_LangIds[lang]);
}

/* NTSC-J text language: 0 Japanese, 1 Chinese.
 *
 * This used to swap only the rasterizer's glyph set, which was the whole job
 * back when the words could only come from a Chinese-patched disc — the codes
 * were already on the disc and only the glyphs drawn at them changed. With
 * zh.pack supplying the words for an unpatched disc that is now half the job:
 * swapping glyphs alone left every string Japanese, so the switch appeared to
 * do nothing. Reinstall the text too, the same way Pc_LangSetLanguage does for
 * PAL. Pc_LangInit picks the glyph set itself (and stands it back down if
 * neither the disc nor the pack has Chinese), so it is not set here. */
void Pc_LangSetJpLanguage(int lang)
{
    lang = (lang != 0);

    g_PcConfig.jpLanguage = lang;
    PcConfig_SaveKeyValue("jp_language", lang ? "zh" : "ja");
    Pc_LangInit();

    /* Inventory text is reinstalled above, but story text is installed per map
     * at load time, so the map already loaded would keep the old language until
     * the next door. g_OvlDynamic still holds this map's overlay, which is the
     * same pointer game_boot.c passes, so re-running the installer against it
     * refreshes the current map in place. Only in game: at the title there is
     * no map loaded and nothing to re-patch. */
    if (g_GameRegion == Region_JPN && g_GameWork.gameState == GameState_InGame &&
        g_OvlDynamic != NULL && g_SavegamePtr != NULL)
    {
        int mapIdx = (int)g_SavegamePtr->mapIdx;

        if (mapIdx >= 0 && mapIdx < 45)
        {
            Pc_LangPatchMapMessages(
                mapIdx, g_OvlDynamic,
                (unsigned int)g_FileTable[FILE_VIN_MAP0_S00_BIN + mapIdx].blockCount << 8);
        }
    }

    SH_LOG("[LANG] NTSC-J language switched to '%s'", lang ? "zh" : "ja");
}

/* ---- Options-menu Language row -------------------------------------------
 * The row works in SLOTS (0..count-1) so it needs no region knowledge of its
 * own: PAL cycles the disc's five languages plus the PC-side packs, NTSC-J
 * cycles Japanese and Chinese. The two lists share no ids and no config key. */

int Pc_LangSlotCount(void)
{
    if (g_GameRegion == Region_JPN)
        return 2;

    /* PC-side packs need the EUR font atlas, so they are offered on EUR only;
     * elsewhere the row stops at the five disc languages. */
    return (g_GameRegion == Region_EUR) ? LANG_COUNT : LANG_PACK_FIRST;
}

int Pc_LangSlotCurrent(void)
{
    return (g_GameRegion == Region_JPN) ? g_PcConfig.jpLanguage : g_PcConfig.language;
}

void Pc_LangSlotSet(int slot)
{
    if (g_GameRegion == Region_JPN)
    {
        Pc_LangSetJpLanguage(slot);
        return;
    }
    Pc_LangSetLanguage(slot);
}

const char* Pc_LangSlotName(int slot)
{
    /* Names in the retail PAL option-menu order (= config language index).
     * Index LANG_PACK_FIRST and up are PC-side packs, labelled by the pack's
     * own `!menu` field (e.g. "POLISH"). */
    static const char* const s_PalNames[LANG_PACK_FIRST] = {
        "English", "German", "French", "Spanish", "Italian"
    };

    if (g_GameRegion == Region_JPN)
        return slot ? "Chinese" : "Japanese";

    if (slot < 0 || slot >= LANG_PACK_FIRST)
        return Pc_LangPackName();

    return s_PalNames[slot];
}

/* Left edge for the value name, nudged per word length the way the retail
 * On/Off values are (the row draws right-aligned-ish against fixed arrows). */
int Pc_LangSlotNameX(int slot)
{
    static const unsigned char s_PalX[LANG_PACK_FIRST] = { 198, 204, 204, 198, 198 };

    if (g_GameRegion == Region_JPN)
        return slot ? 198 : 192;

    if (slot < 0 || slot >= LANG_PACK_FIRST)
        return 200;

    return s_PalX[slot];
}

/* The options menu shows the Language row only on EUR discs and only when
 * entered from the title screen (mirrors how retail applied language at the
 * front end; in-game the row stays Auto Load). */
int Pc_LangMenuRowActive(void)
{
    /* Also on fan-translated USA discs: there the row only switches the
     * port's menu translations (story/item text stays whatever language the
     * disc patch carries). Not on a Russian patch though — its font has no
     * Latin letters, so every other language on the row is unreadable and the
     * menus are already Russian; the slot goes back to retail's Auto Load. */
    if (Pc_RuActive())
    {
        return 0;
    }

    /* NTSC-J keeps the row in-game as well. The title-screen restriction exists
     * because a PAL switch rebinds the file table and reloads item text, which
     * would strand whatever the current map already extracted; the NTSC-J
     * switch only picks a glyph set, so it is safe at any time. Nothing is lost
     * by taking the slot: Auto Load is read once, at the Konami logo
     * (b_konami.c), so its row does nothing in-game anyway and is still
     * reachable from the title screen where it takes effect. */
    if (g_GameRegion == Region_JPN)
    {
        return 1;
    }

    return (g_GameRegion == Region_EUR ||
            (g_GameRegion == Region_USA && s_FanTextActive)) &&
           g_GameWork.gameStatePrev == GameState_MainMenu;
}

const char* Pc_LangItemName(int itemIdx)
{
    if (Pc_LangPackActive())
        return Pc_LangPackItemName(itemIdx);

    if (!s_ItemTextReady || itemIdx < 0 || itemIdx >= ITEM_TEXT_COUNT)
        return NULL;
    /* Empty strings mean "untranslated" on the disc — fall back to English. */
    return (s_ItemNames[itemIdx] && s_ItemNames[itemIdx][0]) ? s_ItemNames[itemIdx] : NULL;
}

/* "~N" is the MAP-MESSAGE newline, understood by the message drawer. Item text
 * goes through Gfx_StringDraw, which knows only a literal newline -- and drops
 * '~' outright, because 0x7E is past the 'z' its glyph range ends at, leaving
 * the 'N' to draw as a letter. That is the stray N in the Japanese inventory.
 *
 * Normalising HERE rather than in the tables is deliberate: the Japanese text
 * can arrive from three places -- the compiled table, a patched disc adopted by
 * JpnFanTextInit, or zh.pack -- and fixing only the compiled one made it DIFFER
 * from the disc, which is precisely the test JpnFanTextInit uses to decide a
 * disc is patched. It then adopted the disc's own "~N" text and put the N
 * straight back. One conversion at the exit covers every source.
 *
 * The caller (Gfx_Inventory_ItemDescriptionDraw) draws the string immediately
 * and does not retain it, so one rotating buffer is enough for the two calls a
 * frame; text without a "~N" is returned untouched. */
static const char* NormalizeItemText(const char* s)
{
    static char s_buf[2][512];
    static int  s_next;

    const char* p;
    char*       out;
    char*       end;

    if (s == NULL)
        return NULL;

    for (p = s; *p != '\0'; p++)
    {
        if (p[0] == '~' && p[1] == 'N')
            break;
    }

    if (*p == '\0')
        return s;

    out    = s_buf[s_next];
    s_next = (s_next + 1) & 1;
    end    = out + sizeof(s_buf[0]) - 1;

    for (p = s; *p != '\0' && out < end; p++)
    {
        if (p[0] == '~' && p[1] == 'N')
        {
            *out++ = '\n';
            p++;
            continue;
        }
        *out++ = *p;
    }

    *out = '\0';
    return s_buf[(s_next + 1) & 1];
}

const char* Pc_LangItemDesc(int itemIdx)
{
    if (Pc_LangPackActive())
        return NormalizeItemText(Pc_LangPackItemDesc(itemIdx));

    if (!s_ItemTextReady || itemIdx < 0 || itemIdx >= ITEM_TEXT_COUNT)
        return NULL;
    return NormalizeItemText((s_ItemDescs[itemIdx] && s_ItemDescs[itemIdx][0]) ? s_ItemDescs[itemIdx] : NULL);
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

/* Message-table pointers are linked for the overlay's PSX load base. An in-place
 * fan patch (Spanish) keeps the US base USA_OVL_BASE; a REBUILT disc (Brazilian)
 * relinks the overlay to a different base, so both the word@0x34 table pointer
 * and every string pointer are shifted by a constant. The overlay layout is
 * otherwise identical. Detect the base by the value for which word@0x34 resolves
 * to a run of clean text messages (the table always opens with the short Yes/No
 * pair). Returns USA_OVL_BASE for vanilla/in-place discs (their table resolves
 * there on the first try, so they take the fast path and are never rescanned). */
#define MSG_MIN_CLEAN_RUN 8

/* Length of a NUL-terminated message if every byte is engine text (printable
 * ASCII, high-bit accents, or the tab/newline used for layout); -1 otherwise. */
static int MsgCleanLen(const unsigned char* s, const unsigned char* end)
{
    int n = 0;
    for (; s < end && *s != '\0'; s++, n++)
    {
        unsigned char c = *s;
        if (c != '\t' && c != '\n' && c < 0x20)
            return -1;
    }
    return (s < end) ? n : -1; /* must terminate inside the overlay */
}

/* Consecutive clean messages the table at (tablePsx - base) resolves to. */
static int MsgCleanRun(const unsigned char* ovl, unsigned int ovlSize, unsigned int tablePsx, unsigned int base)
{
    unsigned int tableOff;
    int          i;

    if (tablePsx <= base || tablePsx - base >= ovlSize)
        return 0;
    tableOff = tablePsx - base;

    for (i = 0; i < 250; i++)
    {
        unsigned int ptr;
        int          len;

        if (tableOff + (unsigned int)(i + 1) * 4 > ovlSize)
            break;
        ptr = *(const unsigned int*)(ovl + tableOff + i * 4);
        if (ptr <= base || ptr - base >= ovlSize)
            break;
        /* The string must START on a boundary (the byte before it is a NUL
         * terminator). An off-by-a-few-bytes base still yields printable text
         * but lands mid-string; rejecting that pins `base` to the overlay's
         * EXACT link base. Message-only use tolerated a few bytes of slack, but
         * the player map-anim loader reuses this base to rebase field_38 (the
         * per-map Harry anim table) — that needs the exact base or the pointer
         * lands mid-struct. */
        if (ptr - base != 0 && ovl[(ptr - base) - 1] != '\0')
            break;
        len = MsgCleanLen(ovl + (ptr - base), ovl + ovlSize);
        if (len < 1 || len > 600)
            break;
        if (i < 2 && len > 6) /* the first two are always the short Yes/No pair */
            return 0;
    }
    return i;
}

/* A dub-only release (ViT Co / Metallist) strips the words out of every voiced
 * line and leaves the "~J0(seconds)" timing command behind, so the scene still
 * paces to the dubbed audio but draws nothing. That is what the disc says and
 * what real hardware shows, so it is not corrected here — only reported, since
 * it reads as a port bug ("no subtitles") every time someone runs one. */
static int MsgIsVoicedButBlank(const char* s)
{
    int visible = 0;

    if (strstr(s, "~J0(") == NULL)
        return 0;

    for (; *s != '\0'; s++)
    {
        if (*s == '~')
        {
            /* Commands are ~ + letter + optional index digits + optional
             * "(2.0)" argument: ~E, ~C2, ~J0(6.0). */
            if (s[1] != '\0')
                s++;
            while (s[1] >= '0' && s[1] <= '9')
                s++;
            if (s[1] == '(')
            {
                while (s[1] != '\0' && s[1] != ')')
                    s++;
                if (s[1] == ')')
                    s++;
            }
            continue;
        }
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '_')
            visible = 1;
    }
    return !visible;
}

static unsigned int UsaDetectOverlayBase(const unsigned char* ovl, unsigned int ovlSize)
{
    static unsigned int s_rebuiltBase = 0; /* cache: a disc has ONE overlay base */
    unsigned int        tablePsx      = *(const unsigned int*)(ovl + 0x34);
    unsigned int        bestBase      = USA_OVL_BASE;
    int                 bestRun       = 0;
    unsigned int        b;

    if (MsgCleanRun(ovl, ovlSize, tablePsx, USA_OVL_BASE) >= MSG_MIN_CLEAN_RUN)
        return USA_OVL_BASE; /* vanilla / in-place patch — no scan */

    if (s_rebuiltBase != 0 &&
        MsgCleanRun(ovl, ovlSize, tablePsx, s_rebuiltBase) >= MSG_MIN_CLEAN_RUN)
        return s_rebuiltBase;

    for (b = USA_OVL_BASE - 0x2000; b <= USA_OVL_BASE + 0x400; b += 4)
    {
        int run = MsgCleanRun(ovl, ovlSize, tablePsx, b);
        if (run > bestRun)
        {
            bestRun  = run;
            bestBase = b;
        }
    }
    if (bestRun >= MSG_MIN_CLEAN_RUN)
    {
        s_rebuiltBase = bestBase;
        return bestBase;
    }
    return USA_OVL_BASE; /* unrecognized — caller's range checks keep English */
}

/* Link base of the internal pointers in a loaded USA map overlay. USA_OVL_BASE
 * for vanilla / in-place patched (Spanish) discs; a per-map LOWER base for a
 * REBUILT disc (Brazilian re-translation), where each overlay is relinked below
 * the US base by a per-map delta. The player map-anim loader
 * (GameFs_PlayerMapAnimLoad) uses this to rebase the field_38 / g_MapHeaderTable_38
 * pointer it reads straight out of the loaded overlay, the SAME delta the message
 * table (offset 0x34) is shifted by — otherwise field_38 resolves into unrelated
 * overlay bytes and Harry's per-map animation-transition table is garbage. `ovl`
 * is the loaded overlay (g_OvlDynamic); mapIdx bounds the base scan by the VIN
 * file size. Returns USA_OVL_BASE (delta 0, no-op) for anything but a USA disc
 * whose current overlay relinked below the US base. */
unsigned int Pc_UsaOverlayLinkBase(const void* ovl, int mapIdx)
{
    unsigned int ovlSize;

    if (g_GameRegion != Region_USA || ovl == NULL || mapIdx < 0 || mapIdx >= 45)
        return USA_OVL_BASE;

    ovlSize = (unsigned int)g_FileTable[FILE_VIN_MAP0_S00_BIN + mapIdx].blockCount << 8;
    if (ovlSize < 0x40)
        return USA_OVL_BASE;

    return UsaDetectOverlayBase((const unsigned char*)ovl, ovlSize);
}

/* USA map messages come straight off the disc image, not from g_OvlDynamic:
 * the shared queue buffer isn't guaranteed to hold this map's overlay
 * (Fs_QueueWaitForEmpty's timeout valve can abandon the read), and reading
 * it here keeps the vanilla USA load path free of the EUR/JPN queue drain.
 * Vanilla overlays match the compiled messages byte-for-byte (a matching
 * decompile), so any difference means a fan-translated disc: repoint the
 * header at verbatim copies of the disc strings (already US dialect,
 * identity index mapping). Everything off the disc is treated as untrusted —
 * unparseable data keeps the compiled English text. */
static void UsaPatchMapMessages(int mapIdx)
{
    const s_FileInfo* fe;
    unsigned char*    ovl;
    unsigned int      ovlSize;
    unsigned int      tablePsx;
    unsigned int      tableOff;
    unsigned int      ovlBase;
    unsigned int      srcPtrs[MSG_COUNT_MAX + 8];
    unsigned int      poolBytes;
    int               srcCount;
    int               cmpCount;
    int               diff;
    int               i;
    char*             newPool;
    char*             out;
    const char**      origMsgs;
    int               dubbed;
    extern s_MapOverlayHdr* g_pMapOverlayHeader;

    if (mapIdx < 0 || mapIdx >= 45 || g_pMapOverlayHeader == NULL)
        return;
    origMsgs = (const char**)g_pMapOverlayHeader->mapMessages;
    if (origMsgs == NULL)
        return;

    fe      = &g_FileTable[FILE_VIN_MAP0_S00_BIN + mapIdx];
    ovlSize = (unsigned int)fe->blockCount << 8;
    if (ovlSize < 0x40)
        return;
    ovl = Pc_LangReadDiscFile(fe->startSector, ovlSize);
    if (ovl == NULL)
        return;

    ovlBase  = UsaDetectOverlayBase(ovl, ovlSize);
    tablePsx = *(unsigned int*)(ovl + 0x34);
    if (tablePsx < ovlBase || tablePsx - ovlBase >= ovlSize)
    {
        free(ovl);
        return;
    }
    tableOff = tablePsx - ovlBase;

    /* Same walk as the EUR/JPN path, plus a NUL-inside-the-overlay check so
     * no later strlen/strcmp/memcpy can run past the buffer. */
    poolBytes = 0;
    for (srcCount = 0; srcCount < (int)(sizeof(srcPtrs) / sizeof(srcPtrs[0])); srcCount++)
    {
        unsigned int ptr;
        const char*  nul;

        if (tableOff + (srcCount + 1) * 4 > ovlSize)
            break;
        ptr = *(unsigned int*)(ovl + tableOff + srcCount * 4);
        if (ptr <= ovlBase || ptr - ovlBase >= ovlSize)
            break;
        ptr -= ovlBase;
        nul = (const char*)memchr(ovl + ptr, '\0', ovlSize - ptr);
        if (nul == NULL)
            break;
        srcPtrs[srcCount] = ptr;
        poolBytes += (unsigned int)(nul - (const char*)(ovl + ptr)) + 1;
    }

    /* 1MB cap: a real message table totals well under 64KB — anything bigger
     * is aliased/garbage pointers. */
    if (srcCount < 4 || poolBytes > (1u << 20))
    {
        free(ovl);
        return;
    }

    /* Every US map's compiled table opens with the same 15 shared
     * map_msg_common.h entries, so indices 0..14 are always in bounds on the
     * compiled side (the smallest table, map1_s05, is exactly those 15) —
     * and every known translation rewrites at least one of them ("Yes").
     * Comparing only that block stays safe against disc tables longer than
     * the compiled array (the compiled side has no count or terminator). */
    cmpCount = (srcCount < 15) ? srcCount : 15;
    diff     = 0;
    for (i = 0; i < cmpCount; i++)
    {
        if (origMsgs[i] == NULL || strcmp(origMsgs[i], (const char*)ovl + srcPtrs[i]) != 0)
        {
            diff = 1;
            break;
        }
    }
    if (!diff)
    {
        free(ovl);
        return;
    }

    newPool = (char*)malloc(poolBytes + 16);
    if (newPool == NULL)
    {
        free(ovl);
        return;
    }
    free(s_MsgPool);
    s_MsgPool = newPool;
    out       = s_MsgPool;

    dubbed = 0;
    for (i = 0; i < MSG_COUNT_MAX; i++)
    {
        if (i < srcCount)
        {
            const char* src = (const char*)ovl + srcPtrs[i];
            size_t      len = strlen(src);

            if (MsgIsVoicedButBlank(src))
                dubbed++;

            memcpy(out, src, len + 1);
            s_MsgPtrs[i] = out;
            out += len + 1;
        }
        else
        {
            s_MsgPtrs[i] = "";
        }
    }

    s_LangMapHeader             = *g_pMapOverlayHeader;
    s_LangMapHeader.mapMessages = s_MsgPtrs;
    g_pMapOverlayHeader         = &s_LangMapHeader;

    free(ovl);
    SH_LOG("[FANPATCH] map %d: %d translated messages installed", mapIdx, srcCount);
    if (dubbed > 0)
    {
        SH_LOG("[FANPATCH] map %d: %d voiced lines have no subtitle text on this disc "
               "(dub release — the patch removed the words and kept the timing)",
               mapIdx, dubbed);
    }
}

void Pc_LangPatchMapMessages(int mapIdx, void* ovl, unsigned int ovlSize)
{
    const unsigned char* bytes = (const unsigned char*)ovl;
    unsigned int         base;
    unsigned int         tablePsx;
    unsigned int         tableOff;
    unsigned int         srcPtrs[MSG_COUNT_MAX + 8];
    int                  srcCount;
    int                  usIdx;
    int                  srcIdx;
    int                  isJpn;
    char*                out;
    extern s_MapOverlayHdr* g_pMapOverlayHeader;

    if (g_GameRegion == Region_USA && !Pc_LangPackActive())
    {
        UsaPatchMapMessages(mapIdx);
        return;
    }

    isJpn = (g_GameRegion == Region_JPN);
    /* To the log file (the SH_LOG/SH_WARN below only reach stdout/the in-game
     * console, so a launcher run records nothing about why story text fell back
     * to compiled English). */
    SH_DBG("[LANG] map %d: region=%d isJpn=%d ovl=%p ovlSize=%u hdr=%p", mapIdx,
           (int)g_GameRegion, isJpn, ovl, ovlSize, (void*)g_pMapOverlayHeader);
    if ((!Pc_LangActive() && !isJpn) || ovl == NULL || g_pMapOverlayHeader == NULL || ovlSize < 0x40)
    {
        SH_DBG("[LANG] map %d: install SKIPPED (langActive=%d isJpn=%d ovl=%p hdr=%p ovlSize=%u<0x40?)",
               mapIdx, Pc_LangActive(), isJpn, ovl, (void*)g_pMapOverlayHeader, ovlSize);
        return;
    }

    base     = isJpn ? JPN_OVL_BASE : EUR_OVL_BASE;
    tablePsx = *(const unsigned int*)(bytes + 0x34);
    if (tablePsx < base || tablePsx - base >= ovlSize)
    {
        SH_WARN("[LANG] map %d: overlay message table pointer out of range (0x%08X)", mapIdx, tablePsx);
        SH_DBG("[LANG] map %d: table ptr 0x%08X out of range [base 0x%08X, +0x%X) — keeping English",
               mapIdx, tablePsx, base, ovlSize);
        return;
    }
    tableOff = tablePsx - base;

    /* Walk the pointer array until a word stops being a valid in-overlay
     * string pointer (matches the table's on-disc terminator). */
    for (srcCount = 0; srcCount < (int)(sizeof(srcPtrs) / sizeof(srcPtrs[0])); srcCount++)
    {
        unsigned int ptr;

        if (tableOff + (srcCount + 1) * 4 > ovlSize)
            break;

        ptr = *(const unsigned int*)(bytes + tableOff + srcCount * 4);
        if (ptr <= base || ptr - base >= ovlSize)
            break;

        srcPtrs[srcCount] = ptr - base;
    }

    if (srcCount < 4)
    {
        SH_WARN("[LANG] map %d: implausible overlay message count %d — keeping English", mapIdx, srcCount);
        SH_DBG("[LANG] map %d: implausible message count %d (<4) — keeping English", mapIdx, srcCount);
        return;
    }

    free(s_MsgPool);
    s_MsgPool = (char*)malloc(ovlSize * 2 + 4096);
    out       = s_MsgPool;

    if (isJpn)
    {
        /* JAP strings are already in engine format (same ~ codes, SJIS text
         * between them) — copy verbatim. The JP tables are 1:1 with US except
         * on the maps in s_JpnMsgMaps, where the US localization split or
         * added lines: those get an explicit index map, and US entries with
         * no JP counterpart (-1) keep the compiled English string. */
        const short* idxMap   = NULL;
        int          idxCount = 0;
        const char** origMsgs = (const char**)g_pMapOverlayHeader->mapMessages;
        int          i;

        for (i = 0; i < (int)(sizeof(s_JpnMsgMaps) / sizeof(s_JpnMsgMaps[0])); i++)
        {
            if (s_JpnMsgMaps[i].mapIdx == mapIdx)
            {
                idxMap   = s_JpnMsgMaps[i].map;
                idxCount = s_JpnMsgMaps[i].count;
                break;
            }
        }

        for (usIdx = 0; usIdx < MSG_COUNT_MAX; usIdx++)
        {
            int jpIdx;

            if (idxMap != NULL)
                jpIdx = (usIdx < idxCount) ? (int)idxMap[usIdx] : -2;
            else
                jpIdx = (usIdx < srcCount) ? usIdx : -2;

            if (jpIdx == -1)
            {
                /* US-only line: keep the compiled English message. */
                s_MsgPtrs[usIdx] = (origMsgs != NULL) ? origMsgs[usIdx] : "";
            }
            else if (jpIdx < 0 || jpIdx >= srcCount)
            {
                s_MsgPtrs[usIdx] = "";
            }
            else
            {
                /* Pack first when it is driving: the overlay under us is the
                 * Japanese disc's, so its strings are Japanese and would draw
                 * as nonsense through the Chinese glyph set. Index with jpIdx,
                 * because the pack stores the disc's own JP order and the map
                 * above has already been applied. */
                const char* src = s_ZhPackActive
                                      ? Pc_LangZhMapMessage(mapIdx, jpIdx)
                                      : NULL;
                size_t      len;

                if (src == NULL)
                {
                    src = (const char*)bytes + srcPtrs[jpIdx];
                }
                len = strlen(src);
                memcpy(out, src, len + 1);
                s_MsgPtrs[usIdx] = out;
                out += len + 1;
            }
        }

        s_LangMapHeader             = *g_pMapOverlayHeader;
        s_LangMapHeader.mapMessages = s_MsgPtrs;
        g_pMapOverlayHeader         = &s_LangMapHeader;

        SH_LOG("[LANG] map %d: %d Japanese messages installed", mapIdx, srcCount);
        SH_DBG("[LANG] map %d: %d Japanese messages installed OK", mapIdx, srcCount);
        return;
    }

    /* EUR index k maps to US index k for k<3; the entry at EUR index 3 is the
     * second half of the split intro message (a "{E}" stub in EN/FR) — join
     * it into US index 2 — and everything after shifts by one. The
     * s_MsgSplits table handles the handful of additional per-language page
     * splits the same way. */
    srcIdx = 0;
    for (usIdx = 0; usIdx < MSG_COUNT_MAX && srcIdx < srcCount; usIdx++)
    {
        int   parts = (usIdx == 2) ? 2 : SplitPartsFor(mapIdx, usIdx);
        int   p;
        char* start = out;
        char* seam[MSG_PARTS_MAX];
        int   seamCount = 0;

        s_MsgPtrs[usIdx] = start;

        for (p = 0; p < parts && srcIdx < srcCount; p++, srcIdx++)
        {
            if (p > 0)
            {
                /* Re-open after the previous part's NUL with a line break. */
                out = out - 1;
                if (seamCount < MSG_PARTS_MAX)
                {
                    seam[seamCount++] = out;
                }
                *out++ = '~';
                *out++ = 'N';
                *out++ = ' ';
            }
            out = TranslateMapMsg(out, bytes + srcPtrs[srcIdx], p == parts - 1);
        }

        if (seamCount != 0)
        {
            /* Retail shows these parts as separate pages; joining them is what
             * keeps the compiled maps' message indices resolving. When the join
             * does not fit one rendered page, promote the seams back to page
             * breaks so each part lands on retail's own boundary — otherwise the
             * renderer has to break mid-part at the line cap. */
            if (MsgLineCount(start) > g_PcMapMsgLineMax)
            {
                for (p = 0; p < seamCount; p++)
                {
                    seam[p][1] = MAP_MSG_CODE_PAGE;
                }
            }
            out = start + strlen(start) + 1;
        }
    }

    for (; usIdx < MSG_COUNT_MAX; usIdx++)
    {
        s_MsgPtrs[usIdx] = "";
    }

    /* PC-side pack (Polish): the walk above filled s_MsgPtrs with the disc's
     * own English text (this language clamps the file redirect to English), so
     * it doubles as the count discovery and the untranslated-line fallback.
     * Overlay the pack's translations, keyed by the same US message index the
     * split logic just resolved. Pack strings outlive this call (owned by
     * lang_pack), so pointing straight at them is safe. */
    if (Pc_LangPackActive())
    {
        int replaced = 0;

        for (usIdx = 0; usIdx < MSG_COUNT_MAX; usIdx++)
        {
            const char* tr = Pc_LangPackMapMsg(mapIdx, usIdx);

            if (tr != NULL)
            {
                s_MsgPtrs[usIdx] = tr;
                replaced++;
            }
        }
        SH_LOG("[LANG] map %d: %d pack messages overlaid (%s)", mapIdx, replaced,
               s_LangIds[g_PcConfig.language]);
    }

    s_LangMapHeader             = *g_pMapOverlayHeader;
    s_LangMapHeader.mapMessages = s_MsgPtrs;
    g_pMapOverlayHeader         = &s_LangMapHeader;

    SH_LOG("[LANG] map %d: %d localized messages installed (lang %d)", mapIdx, srcCount, g_PcConfig.language);
}

/* ------------------------------------------------------------------ */
/* Mod text overrides — gamedata/load/text_overrides.txt               */
/* ------------------------------------------------------------------ */
/* A minimal moddable per-message replacement. One entry per line:
 *     map0_s00.18 = Ah shit, ~N here we go again.
 * (key = the extractor's [MAPn_Snn.idx] lowercased; '#' comments, blank
 * lines ignored.) The map message's original leading ~J timing cue and a
 * trailing ~E are kept automatically, so a replacement stays voice-synced;
 * '_' and ' ' both render as a space and '~N' is a line break. Applied AFTER
 * any localization swap so it wins for every region; an absent file is a
 * no-op. This is the seed of a fuller mod text system (menus/items, the
 * localization-doc format) tracked separately. */

#define TEXT_OVR_MAX 128

typedef struct {
    short mapIdx;
    short msgIdx;
    char* userText; /* raw replacement from the file */
    char* built;    /* engine-format (prefix + converted + ~E), lazily built */
} s_TextOverride;

static s_TextOverride  s_TextOvr[TEXT_OVR_MAX];
static int             s_TextOvrCount = -1; /* -1 = not loaded yet */
static const char*     s_ModMsgs[MSG_COUNT_MAX];
static s_MapOverlayHdr s_ModMapHeader;

extern int MapRegistry_FindByName(const char* name);
extern int Pc_MapMsgCount(int mapIdx);

static void TextOverridesLoad(void)
{
    FILE* f;
    char  line[512];

    s_TextOvrCount = 0;
    f = fopen("gamedata/load/text_overrides.txt", "rb");
    if (f == NULL)
        return;

    while (fgets(line, sizeof(line), f) != NULL && s_TextOvrCount < TEXT_OVR_MAX)
    {
        char* p = line;
        char* eq;
        char* dot;
        char* key;
        char* val;
        char* end;
        char* c;
        int   mapIdx;
        int   msgIdx;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
            continue;

        eq = strchr(p, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        key = p;
        val = eq + 1;

        for (end = eq - 1; end >= key && (*end == ' ' || *end == '\t'); end--)
            *end = '\0';

        dot = strrchr(key, '.');
        if (dot == NULL)
            continue;
        *dot = '\0';
        msgIdx = atoi(dot + 1);
        for (c = key; *c != '\0'; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c = (char)(*c + 32);
        mapIdx = MapRegistry_FindByName(key);
        if (mapIdx < 0 || msgIdx < 0)
            continue;

        while (*val == ' ' || *val == '\t')
            val++;
        for (end = val + strlen(val) - 1;
             end >= val && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t'); end--)
            *end = '\0';

        s_TextOvr[s_TextOvrCount].mapIdx   = (short)mapIdx;
        s_TextOvr[s_TextOvrCount].msgIdx   = (short)msgIdx;
        s_TextOvr[s_TextOvrCount].userText = _strdup(val);
        s_TextOvr[s_TextOvrCount].built    = NULL;
        s_TextOvrCount++;
    }
    fclose(f);
    SH_LOG("[MODTEXT] loaded %d text override(s)", s_TextOvrCount);
}

/* Keep orig's leading ~J..(..) timing prefix (+ following whitespace), then the
 * user text (space -> '_', '~N' etc. pass through), then a trailing " ~E ". */
static char* TextOverrideBuild(const char* orig, const char* userText)
{
    char        prefix[64];
    int         pl = 0;
    const char* s  = orig;
    const char* u;
    char*       out;
    char*       o;

    if (s != NULL && s[0] == '~' && s[1] != '\0')
    {
        prefix[pl++] = *s++; /* ~ */
        prefix[pl++] = *s++; /* code letter, e.g. J */
        /* Copy any parameter chars between the code letter and the '(' timing
         * group. The PSX format is ~J0(2.5) — a page/variant digit sits before
         * the paren, NOT ~J(2.5). The old code checked for '(' immediately after
         * the letter, so it dropped the "0(2.5)\t" entirely and produced a bare
         * "~J<text>" the map-message timer couldn't parse — the override showed
         * nothing. Capture up to the '(' (or whitespace / next code / end). */
        while (*s != '\0' && *s != '(' && *s != ' ' && *s != '\t' && *s != '~' &&
               pl < (int)sizeof(prefix) - 2)
            prefix[pl++] = *s++;
        if (*s == '(')
        {
            while (*s != '\0' && *s != ')' && pl < (int)sizeof(prefix) - 2)
                prefix[pl++] = *s++;
            if (*s == ')')
                prefix[pl++] = *s++;
        }
        while ((*s == '\t' || *s == ' ') && pl < (int)sizeof(prefix) - 1)
            prefix[pl++] = *s++;
    }

    out = (char*)malloc((size_t)pl + strlen(userText) + 8);
    if (out == NULL)
        return NULL;
    o = out;
    memcpy(o, prefix, (size_t)pl);
    o += pl;
    for (u = userText; *u != '\0'; u++)
        *o++ = (*u == ' ') ? '_' : *u;
    *o++ = ' ';
    *o++ = '~';
    *o++ = 'E';
    *o++ = ' ';
    *o   = '\0';
    return out;
}

void Pc_TextOverrideApply(int mapIdx)
{
    const char** src;
    int          count;
    int          i;
    int          any = 0;

    if (s_TextOvrCount < 0)
        TextOverridesLoad();
    if (s_TextOvrCount == 0 || g_pMapOverlayHeader == NULL)
        return;

    for (i = 0; i < s_TextOvrCount; i++)
    {
        if (s_TextOvr[i].mapIdx == mapIdx)
        {
            any = 1;
            break;
        }
    }
    if (!any)
        return;

    src = (const char**)g_pMapOverlayHeader->mapMessages;
    if (src == NULL)
        return;

    /* Repoint to a static copy rather than writing the live array: on a vanilla
     * USA disc it is the compiled MAP_MESSAGES in read-only rodata. Bound the
     * copy by the map's real message count so it never reads past the array. */
    count = Pc_MapMsgCount(mapIdx);
    if (count <= 0 || count > MSG_COUNT_MAX)
        return;

    for (i = 0; i < count; i++)
        s_ModMsgs[i] = src[i];

    for (i = 0; i < s_TextOvrCount; i++)
    {
        s_TextOverride* o = &s_TextOvr[i];

        if (o->mapIdx != mapIdx || o->msgIdx < 0 || o->msgIdx >= count)
            continue;
        if (o->built == NULL)
            o->built = TextOverrideBuild(src[o->msgIdx], o->userText);
        if (o->built != NULL)
            s_ModMsgs[o->msgIdx] = o->built;
    }

    s_ModMapHeader             = *g_pMapOverlayHeader;
    s_ModMapHeader.mapMessages = s_ModMsgs;
    g_pMapOverlayHeader        = &s_ModMapHeader;

    SH_LOG("[MODTEXT] map %d: text override(s) applied", mapIdx);
}
