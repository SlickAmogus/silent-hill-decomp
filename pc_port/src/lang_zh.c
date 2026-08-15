/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chinese text for NTSC-J, read from gamedata/lang/zh.pack.
 *
 * The Chinese translation (goro / 十三月, 2006) is two halves: a patched PSX
 * BIOS carrying the GLYPHS, and a PPF carrying the WORDS. The port already
 * replaces the BIOS half with its own embedded font (kanji_font_cn.inc), which
 * is why the glyph set can be switched at runtime — but with a retail Japanese
 * disc there were no Chinese words to draw, so switching produced nothing but
 * wrong characters.
 *
 * This supplies the missing half. pc_port/tools/gen_zh_pack.py lifts the text
 * off a Chinese-patched disc once, offline, and this reads it back so an
 * UNPATCHED Japanese disc can show Chinese and switch on the fly. Redistributed
 * with the translators' permission.
 *
 * The stored bytes are the disc's own: the translation writes ordinary JIS
 * kuten codes whose glyphs its BIOS redefines, so nothing here is transcoded
 * and the engine's control codes (~N, ~E, ~J0(2.0)) survive untouched. The map
 * tables are in the disc's JP order, because the caller applies the port's
 * existing US->JP index map to them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "lang_zh.h"
#include "sh_log.h"

#define ZH_MAGIC   0x485A4853u /* 'SHZH' little-endian */
#define ZH_VERSION 1
#define ZH_NO_STR  0xFFFFFFFFu

/* Header, then itemCount name offsets, itemCount desc offsets, mapCount map
 * table offsets; each map table is a count followed by that many offsets; all
 * offsets index the string blob. */
typedef struct
{
    unsigned int magic;
    unsigned int version;
    unsigned int itemCount;
    unsigned int mapCount;
    unsigned int blobOff;
    unsigned int blobSize;
} s_ZhHeader;

static unsigned char* s_pack;
static unsigned int   s_packSize;
static int            s_tried;

static const s_ZhHeader* ZhHeader(void)
{
    return (const s_ZhHeader*)s_pack;
}

/* Every accessor goes through this: a truncated or hand-edited pack must fail
 * to a NULL string rather than walk off the allocation. */
static const char* ZhString(unsigned int off)
{
    const s_ZhHeader* h = ZhHeader();
    unsigned int      abs;

    if (s_pack == NULL || off == ZH_NO_STR || off >= h->blobSize)
    {
        return NULL;
    }
    abs = h->blobOff + off;
    if (abs >= s_packSize)
    {
        return NULL;
    }
    /* The blob is NUL-terminated throughout, but a damaged file might not be. */
    if (memchr(s_pack + abs, '\0', s_packSize - abs) == NULL)
    {
        return NULL;
    }
    return (const char*)(s_pack + abs);
}

static unsigned int ZhU32(unsigned int byteOff)
{
    unsigned int v;

    if (s_pack == NULL || byteOff + 4 > s_packSize)
    {
        return ZH_NO_STR;
    }
    memcpy(&v, s_pack + byteOff, 4);
    return v;
}

int Pc_LangZhAvailable(void)
{
    FILE*        f;
    long         size;
    s_ZhHeader   h;
    unsigned int need;

    if (s_tried)
    {
        return s_pack != NULL;
    }
    s_tried = 1;

    f = fopen("gamedata/lang/zh.pack", "rb");
    if (f == NULL)
    {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)sizeof(s_ZhHeader) || size > (16 << 20))
    {
        fclose(f);
        SH_WARN("[LANG-ZH] zh.pack is not a plausible size (%ld bytes) — ignoring", size);
        return 0;
    }

    s_pack = (unsigned char*)malloc((size_t)size);
    if (s_pack == NULL)
    {
        fclose(f);
        return 0;
    }
    if (fread(s_pack, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(s_pack);
        s_pack = NULL;
        return 0;
    }
    fclose(f);
    s_packSize = (unsigned int)size;

    memcpy(&h, s_pack, sizeof(h));
    /* The directory has to fit before the blob can, so check the whole shape
     * once here and let every accessor assume it afterwards. */
    need = (unsigned int)sizeof(s_ZhHeader) + h.itemCount * 8 + h.mapCount * 4;
    if (h.magic != ZH_MAGIC || h.version != ZH_VERSION ||
        h.itemCount == 0 || h.itemCount > 4096 ||
        h.mapCount == 0 || h.mapCount > 256 ||
        need > s_packSize || h.blobOff > s_packSize ||
        h.blobSize > s_packSize - h.blobOff)
    {
        SH_WARN("[LANG-ZH] zh.pack header failed validation — ignoring");
        free(s_pack);
        s_pack = NULL;
        s_packSize = 0;
        return 0;
    }

    SH_LOG("[LANG-ZH] zh.pack loaded (%u items, %u maps, %u bytes of text)",
           h.itemCount, h.mapCount, h.blobSize);
    return 1;
}

const char* Pc_LangZhItemName(int idx)
{
    const s_ZhHeader* h;

    if (!Pc_LangZhAvailable())
    {
        return NULL;
    }
    h = ZhHeader();
    if (idx < 0 || (unsigned int)idx >= h->itemCount)
    {
        return NULL;
    }
    return ZhString(ZhU32((unsigned int)sizeof(s_ZhHeader) + (unsigned int)idx * 4));
}

const char* Pc_LangZhItemDesc(int idx)
{
    const s_ZhHeader* h;

    if (!Pc_LangZhAvailable())
    {
        return NULL;
    }
    h = ZhHeader();
    if (idx < 0 || (unsigned int)idx >= h->itemCount)
    {
        return NULL;
    }
    return ZhString(ZhU32((unsigned int)sizeof(s_ZhHeader) + (h->itemCount + (unsigned int)idx) * 4));
}

int Pc_LangZhMapMessageCount(int mapIdx)
{
    const s_ZhHeader* h;
    unsigned int      tableOff;

    if (!Pc_LangZhAvailable())
    {
        return 0;
    }
    h = ZhHeader();
    if (mapIdx < 0 || (unsigned int)mapIdx >= h->mapCount)
    {
        return 0;
    }
    tableOff = ZhU32((unsigned int)sizeof(s_ZhHeader) + h->itemCount * 8 + (unsigned int)mapIdx * 4);
    if (tableOff == 0 || tableOff == ZH_NO_STR || tableOff + 4 > s_packSize)
    {
        return 0;
    }
    return (int)ZhU32(tableOff);
}

const char* Pc_LangZhMapMessage(int mapIdx, int msgIdx)
{
    const s_ZhHeader* h;
    unsigned int      tableOff;
    int               count;

    count = Pc_LangZhMapMessageCount(mapIdx);
    if (msgIdx < 0 || msgIdx >= count)
    {
        return NULL;
    }
    h = ZhHeader();
    tableOff = ZhU32((unsigned int)sizeof(s_ZhHeader) + h->itemCount * 8 + (unsigned int)mapIdx * 4);
    return ZhString(ZhU32(tableOff + 4 + (unsigned int)msgIdx * 4));
}
