#include "lang_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/map/map.h" /* s_MapOverlayHdr, g_pMapOverlayHeader, e_MapIdx */
#include "font_region.h"      /* g_FontLayout, Font_SetGlyphWidths (fan-patch kerning) */
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
/* Must cover the largest per-map message table: MAP7_S02 has 159 US entries.
 * (Was 96 — the replaced pointer array under-covered MAP4_S01/MAP7_S01/
 * MAP7_S02 on PAL, sending reads past it.) */
#define MSG_COUNT_MAX   176
#define MSG_LINES_MAX   9 /* FONT_12X16_LINE_COUNT_MAX — the renderer clips past it */

#define RAW_SECTOR_SIZE 2352
#define SECTOR_DATA_OFF 24
#define SECTOR_DATA_LEN 2048

static const char* s_ItemBinNames[5] = { "ITEM_ENG", "ITEM_GER", "ITEM_FRN", "ITEM_SPN", "ITEM_ITL" };

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

/* NTSC-J: US->JAP message index mapping for the maps whose tables differ. */
#include "lang_jpn_msgmap.inc"

int Pc_LangActive(void)
{
    /* Any language INCLUDING English: PAL-EN is its own retranslation
     * ("Take them?" vs the US "Take_it?"), so a PAL disc always shows its
     * own text rather than the compiled US strings. */
    return g_GameRegion == Region_EUR;
}

static int s_FanTextActive;

int Pc_FanTextActive(void)
{
    return s_FanTextActive;
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
                           unsigned int base, unsigned int nameOff, unsigned int descOff)
{
    extern const char* INVENTORY_ITEM_NAMES[];
    extern const char* g_ItemDescriptions[];

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

        compiled = INVENTORY_ITEM_NAMES[i];
        if ((compiled == NULL) != (fanNames[i] == NULL) ||
            (compiled != NULL && strcmp(compiled, fanNames[i]) != 0))
        {
            diff = 1;
        }
        compiled = g_ItemDescriptions[i];
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
 * NUL-terminated within maxLen, and starting on a string boundary (the byte
 * before it is a NUL). Rejects pointers that land mid-string, which is what
 * separates the true link base from off-by-a-few neighbours. */
static int IsPlaintextName(const unsigned char* bin, unsigned int size, unsigned int so, unsigned int maxLen)
{
    unsigned int j;

    if (so >= size)
        return 0;
    if (so > 0 && bin[so - 1] != 0)
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

/* Locate a relinked, UNENCRYPTED item-name pointer array in a rebuilt disc's
 * BODYPROG (Brazilian re-translation). The item set is unchanged from US, so
 * the array's set/empty slot pattern matches the compiled build exactly; every
 * non-empty pointer must resolve to a clean name that starts on a string
 * boundary. An encrypted vanilla/in-place disc has no such array in the raw
 * bytes (both requirements fail on random data), so this returns 0 for them
 * and the caller decrypts as before. On success, writes the detected link base
 * and name-array file offset. */
static int BrazilFindItemArray(const unsigned char* bin, unsigned int size,
                               unsigned int* outBase, unsigned int* outNameOff)
{
    extern const char* INVENTORY_ITEM_NAMES[];

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
            int ok = 1;

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
    unsigned char* bin  = ReadDiscFile(g_FileTable[FILE_1ST_BODYPROG_BIN].startSector, size);
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
        int r = AdoptItemArrays(bin, size, brBase, brNameOff, brNameOff + ITEM_TEXT_COUNT * 4);

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
        Font_SetGlyphWidths(bin + (USA_WIDTHS_ADDR - USA_BODY_VRAM));
        s_FanTextActive = 1;
        SH_LOG("[FANPATCH] modified FONT16 kerning table adopted from disc");
    }

    switch (AdoptItemArrays(bin, size, USA_BODY_VRAM,
                            USA_ITEM_NAME_ADDR - USA_BODY_VRAM,
                            USA_ITEM_DESC_ADDR - USA_BODY_VRAM))
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

    if (g_GameRegion == Region_USA)
    {
        FanTextInit();
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

/* Live language switch — title-screen options menu only (no map loaded, so
 * nothing already-extracted goes stale; the next New Game/Load/demo picks
 * everything up through the normal per-map path). Persists the config key,
 * rebinds the file table and reloads item text. */
void Pc_LangSetLanguage(int lang)
{
    static const char* const ids[5] = { "en", "de", "fr", "es", "it" };

    if (lang < 0 || lang > 4)
        lang = 0;

    g_PcConfig.language = lang;
    PcConfig_SaveKeyValue("language", ids[lang]);
    Fs_ApplyLanguageRedirects();
    Pc_LangInit();
    SH_LOG("[LANG] language switched to '%s'", ids[lang]);
}

/* The options menu shows the Language row only on EUR discs and only when
 * entered from the title screen (mirrors how retail applied language at the
 * front end; in-game the row stays Auto Load). */
int Pc_LangMenuRowActive(void)
{
    /* Also on fan-translated USA discs: there the row only switches the
     * port's menu translations (story/item text stays whatever language the
     * disc patch carries). */
    return (g_GameRegion == Region_EUR ||
            (g_GameRegion == Region_USA && s_FanTextActive)) &&
           g_GameWork.gameStatePrev == GameState_MainMenu;
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
    ovl = ReadDiscFile(fe->startSector, ovlSize);
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

    for (i = 0; i < MSG_COUNT_MAX; i++)
    {
        if (i < srcCount)
        {
            size_t len = strlen((const char*)ovl + srcPtrs[i]);

            memcpy(out, ovl + srcPtrs[i], len + 1);
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

    if (g_GameRegion == Region_USA)
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
                size_t len = strlen((const char*)bytes + srcPtrs[jpIdx]);

                memcpy(out, bytes + srcPtrs[jpIdx], len + 1);
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

        s_MsgPtrs[usIdx] = start;

        for (p = 0; p < parts && srcIdx < srcCount; p++, srcIdx++)
        {
            if (p > 0)
            {
                /* Re-open after the previous part's NUL with a line break. */
                out    = out - 1;
                *out++ = '~';
                *out++ = 'N';
                *out++ = ' ';
            }
            out = TranslateMapMsg(out, bytes + srcPtrs[srcIdx], p == parts - 1);
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

    SH_LOG("[LANG] map %d: %d localized messages installed (lang %d)", mapIdx, srcCount, g_PcConfig.language);
}
