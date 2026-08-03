/*
 * ra_xbox.c - RetroAchievements (softcore) integration for the Original Xbox port.
 *
 * Adapted from pc_port/src/pc_retroachievements.c. Same idea: the console runs
 * the player's own PSX disc image (the BIN on the HDD), so rc_hash over that
 * image yields the genuine RetroAchievements hash and the official Silent Hill
 * set applies. Unlocks post to the real account. Softcore is forced (quick
 * save/load, debug controls, free cameras => hardcore is untenable).
 *
 * TWO things differ from the PC build, both dictated by the platform:
 *
 *  1. NO threads, NO SDL. rc_client is single-threaded, so every rc_client_*
 *     call stays on the main thread. The PC ran HTTP on an SDL worker and
 *     dispatched completions from Update; here the HTTP transport
 *     (Net_XboxHttpRequest) is BLOCKING, so we queue each server call rc_client
 *     hands us and drain ONE per Pc_Ra_Update (a bounded pump). At most one
 *     blocking request per frame keeps the login/load handshake from freezing
 *     the frame for a second at boot, and rc_client stays strictly main-thread.
 *
 *  2. NO GetProcAddress/--export-all-symbols auto-map. The PC rebuilt the whole
 *     PSX address space from the decomp symbol files by resolving every exported
 *     name at runtime; an XBE has no equivalent. Instead we HAND-map the handful
 *     of native globals the live set actually reads. Crucially, on 32-bit Xbox
 *     the mapping is 1:1 by &address: a pointer is 4 bytes == s32, so s_SysWork's
 *     field_2510 widening (game.h, SH_PC_PORT) does NOT shift anything, and there
 *     is no +4 hole the way the 64-bit PC build had. g_SysWork and g_GameWork map
 *     straight through at their retail offsets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef SH_RETROACHIEVEMENTS

#include <rc_client.h>
#include <rc_hash.h>
#include <rc_consoles.h>

/* Vendored rcheevos PRIVATE headers, resolved via the same -I.../src the library
 * TUs use. They let Ra_DumpWorklist walk the parsed memref list at game-load and
 * print the COMPLETE, finite set of addresses the whole 66-achievement set reads
 * -- the authoritative worklist for finishing the translation table, in one shot
 * with no gameplay. Included BEFORE the decomp headers: they carry only rc_*
 * symbols, so they can't collide with game.h's `byte`/RECT/etc. */
#include "rc_client_internal.h"   /* rc_client_t.game -> rc_client_game_info_t.runtime */
#include "rcheevos/rc_internal.h" /* rc_memrefs_t / rc_memref_list_t struct bodies    */

#include "game.h"
#include "bodyprog/savegame.h"
/* Globals the live achievement set was observed reading (see the table below). */
#include "bodyprog/bodyprog.h"                      /* g_KcetLogoImg, MAP_EFFECTS_INFOS, g_WorldGfxWork, D_800AD4C8 */
#include "bodyprog/demo.h"                          /* g_Demo_IsLoadingChunks */
#include "bodyprog/item_screens.h"                  /* g_Inventory_EquippedItem, g_Item_MapLoadableItems, g_Items_Coords */
#include "bodyprog/screen/screen_data.h"            /* g_OtTags1 */
#include "bodyprog/collision/collision.h"           /* g_ActiveCollisionTriggers */
#include "bodyprog/sound/sound_system.h"            /* g_XaItemData, Sd_PlaySfx */
#include "bodyprog/libsd.h"                         /* smf_song (SMF_SONG), PitchTbl */
#include "bodyprog/sound/sfx_id_enum.h"             /* Sfx_MenuConfirm (trophy chime) */
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_MapEventLastUsedItem */
#include "bodyprog/ranking.h"                        /* end-of-run stat scalars D_800C48xx */

#include "pc_config.h"
#include "sh_log.h"
#include "net_xbox.h"

/* The BIN path cd_xbox.c resolved (e.g. "Q:\\Silent Hill (USA).bin"); rc_hash
 * opens its own handle on it via the custom filereader below. */
extern char g_CdBinPath[];

/* Optional on-screen notification sink. xbox_pcfeature_stubs.c defines this as a
 * NULL function pointer; if the lead ever wires an overlay toast, RA unlocks flow
 * through it automatically. Until then the D: log (SH_DBG) is the channel. */
extern void (*g_ShOverlayToastLine)(const char* line);

/* ------------------------------------------------------------------------- */
/* PSX address -> native global translation (hand table)                      */
/* ------------------------------------------------------------------------- */
/*
 * rcheevos hands us RAM offsets (0x000000-0x1FFFFF), i.e. PSX 0x800xxxxx with the
 * segment stripped. Two adjacent regions carry effectively all observable game
 * state (SysWork ends exactly where GameWork begins, which cross-checks both
 * bases): g_SysWork @0x0B9FC0 (0x2768) and g_GameWork @0x0BC728 (0x05D8, embeds
 * autosave @+0x90 and savegame @+0x30C). On 32-bit both map 1:1 by sizeof, no
 * split -- sizeof() bounds every entry so a read can never run past the native
 * object even if a symbol-map span claimed more.
 */
typedef struct
{
    uint32_t    start;      /* RA/RAM offset of the first mapped byte */
    uint32_t    size;       /* bytes covered                          */
    const void* native;     /* native bytes backing `start`           */
    const char* name;
} s_RaRegion;

static s_RaRegion s_map[48];
static int        s_mapCount;

#define RA_MAP(psxOffset, sym) do { \
    if (s_mapCount < (int)(sizeof(s_map) / sizeof(s_map[0]))) { \
        s_map[s_mapCount].start  = (uint32_t)(psxOffset); \
        s_map[s_mapCount].size   = (uint32_t)sizeof(sym); \
        s_map[s_mapCount].native = (const void*)&(sym); \
        s_map[s_mapCount].name   = #sym; \
        s_mapCount++; \
    } \
} while (0)

/* Explicit-span mapping for regions whose NATIVE size differs from retail. */
#define RA_MAP_SEG(psxOffset, span, nativePtr, nm) do { \
    if (s_mapCount < (int)(sizeof(s_map) / sizeof(s_map[0]))) { \
        s_map[s_mapCount].start  = (uint32_t)(psxOffset); \
        s_map[s_mapCount].size   = (uint32_t)(span); \
        s_map[s_mapCount].native = (const void*)(nativePtr); \
        s_map[s_mapCount].name   = (nm); \
        s_mapCount++; \
    } \
} while (0)

static void Ra_BuildMap(void)
{
    s_mapCount = 0;

    /* g_SysWork must NOT be mapped whole with sizeof: under SH_PC_PORT
     * NPC_COUNT_MAX is 32 (retail 6), widening npcs[] and npcBoneCoordBuffer[]
     * so sizeof(s_SysWork) is ~38588, not the retail 0x2768. A whole-struct
     * entry therefore spanned RA 0x0B9FC0-0x0C367C and -- because Ra_ReadMemory
     * returns on FIRST match -- SHADOWED the g_GameWork entry (0x0BC728+): every
     * event-flag/GameWork achievement operand was served from inside the widened
     * bone-coordinate arrays (matrix noise), so NO gameplay achievement could
     * ever trigger. Map the retail layout as FOUR segments anchored at the
     * native members instead (per-member layout is unchanged; only the two
     * array lengths differ, so retail slots 0..5 = the first 6 native slots). */
    RA_MAP_SEG(0x0B9FC0u,           0x1A0u,  &g_SysWork,                      "sysWork.head");
    RA_MAP_SEG(0x0B9FC0u + 0x1A0u,  0x6F0u,  &g_SysWork.npcs[0],              "sysWork.npcs");
    RA_MAP_SEG(0x0B9FC0u + 0x890u,  0x19F0u, &g_SysWork.playerBoneCoords[0],  "sysWork.bones");
    RA_MAP_SEG(0x0B9FC0u + 0x2280u, 0x4E8u,  &g_SysWork.npcFlagsId,           "sysWork.tail");
    SH_DBG("[RA] syswork native: npcs=0x%X bones=0x%X tail=0x%X sizeof=%u",
           (unsigned)offsetof(s_SysWork, npcs),
           (unsigned)offsetof(s_SysWork, playerBoneCoords),
           (unsigned)offsetof(s_SysWork, npcFlagsId),
           (unsigned)sizeof(s_SysWork));

    RA_MAP(0x0BC728u, g_GameWork);

    /* The rest of what the live set (game 11252, 66 achievements) was observed
     * sampling on PC. Every type here is pointer-free and its retail size matches
     * configs/USA/sym.bodyprog.txt, so retail offsets hold with no widening. */
    RA_MAP(0x0A9004u, g_KcetLogoImg);
    RA_MAP(0x0A93CCu, MAP_EFFECTS_INFOS);
    RA_MAP(0x0A9A18u, g_MapEventLastUsedItem);

    /* g_XaItemData is declared as an incomplete array (s_XaItemData[]), so sizeof
     * is unavailable -- take the span straight from the symbol map. */
    if (s_mapCount < (int)(sizeof(s_map) / sizeof(s_map[0])))
    {
        s_map[s_mapCount].start  = 0x0AA894u;
        s_map[s_mapCount].size   = 0x2214u;
        s_map[s_mapCount].native = (const void*)&g_XaItemData[0];
        s_map[s_mapCount].name   = "g_XaItemData";
        s_mapCount++;
    }

    RA_MAP(0x0AE184u, g_Inventory_EquippedItem);
    RA_MAP(0x0BCE18u, g_WorldGfxWork);
    RA_MAP(0x0C3BB8u, g_Item_MapLoadableItems);
    RA_MAP(0x0C4478u, g_ActiveCollisionTriggers);
    RA_MAP(0x0C489Cu, g_Demo_IsLoadingChunks);

    /* ---- Worklist additions (2026-07-31): the 17 addresses the live set was
     * observed reading that fell in no mapped region. Each entry below maps the
     * whole CONTAINING named global at its retail base (from configs/USA/
     * sym.bodyprog.txt), verified against a real C global with a computable size.
     * Several worklist addresses that fall in PSY-Q library scratch (libgte/libgs/
     * libcd) or unnamed statics have NO backing C global and are intentionally
     * left unmapped -- they safely read 0 via the RAM fallback in Ra_ReadMemory.
     *
     * NOTE: the Split Head boss-kill flag (EventFlag_131) is NOT among these; the
     * event flags live in g_GameWork.savegame.eventFlags (g_SavegamePtr ==
     * &g_GameWork.savegame), so they are ALREADY covered by the g_GameWork entry
     * above. */

    /* Weapon-attack / combat data table `s_800AD4C8 D_800AD4C8[70]` (bodyprog.h,
     * 70 * 24 = 0x690). Covers 0x800AD77C. */
    RA_MAP(0x0AD4C8u, D_800AD4C8);

    /* Ordering table `g_OtTags1`. On SH_PC_PORT the NATIVE array is [3][2048]
     * (0x6000), but retail is [2][2048] (0x4000) and g_GravitySpeed/vcWork/g_SysWork
     * sit immediately after it in the RA offset space. Map ONLY the retail 0x4000
     * span (explicit, like g_XaItemData) so this region can never shadow the
     * g_SysWork@0x0B9FC0 entry. Covers 0x800B95B8, 0x800B96AE, 0x800B9B98,
     * 0x800B9C8E. */
    if (s_mapCount < (int)(sizeof(s_map) / sizeof(s_map[0])))
    {
        s_map[s_mapCount].start  = 0x0B5CC4u;
        s_map[s_mapCount].size   = 0x4000u;
        s_map[s_mapCount].native = (const void*)&g_OtTags1[0][0];
        s_map[s_mapCount].name   = "g_OtTags1";
        s_mapCount++;
    }

    /* Item-screen display transforms `GsCOORDINATE2 g_Items_Coords[10]`
     * (item_screens.h, 10 * 0x50 = 0x320). Covers 0x800C3EA2. */
    RA_MAP(0x0C3E48u, g_Items_Coords);

    /* libsd MIDI sequencer state `SMF_SONG smf_song[2]` (libsd.h, 2 * 1340 = 0xA78).
     * Covers 0x800C945D. */
    RA_MAP(0x0C8B00u, smf_song);

    /* ---- Worklist additions (2026-07-31, log 013 COMPLETE memref dump) -----
     * The full-set dump (Ra_DumpWorklist) resolved every unmapped address the 66
     * achievements + rich presence read. The biggest cluster (0x800C48A0-0x800C48D1)
     * is the RANKING module (src/bodyprog/ranking.c .bss) -- the game's end-of-run
     * stats: save/continue/clear counts, play time, item + kill counts, ending
     * flags, computed rank. These back the completion / kill-count / time / rank
     * achievements, which read individual scalars here. Each is a pointer-free
     * integer at its own retail offset (extern in bodyprog/ranking.h), mapped 1:1
     * so a native pointer value can never leak in. */
    RA_MAP(0x0C48A0u, D_800C48A0);   /* s16 savegame count       */
    RA_MAP(0x0C48A2u, D_800C48A2);   /* u16 gameplay hours       */
    RA_MAP(0x0C48A6u, D_800C48A6);   /* u16 walk distance        */
    RA_MAP(0x0C48ACu, D_800C48AC);   /* u16 picked-up item count */
    RA_MAP(0x0C48AEu, D_800C48AE);   /* u8  minutes              */
    RA_MAP(0x0C48B0u, D_800C48B0);   /* u8  clear-game count     */
    RA_MAP(0x0C48B2u, D_800C48B2);   /* u8  ending flags         */
    RA_MAP(0x0C48B4u, D_800C48B4);   /* u8  special-item count   */
    RA_MAP(0x0C48B5u, D_800C48B5);   /* s8  computed rank/score  */
    RA_MAP(0x0C48B8u, D_800C48B8);   /* u16 ranged kill count    */
    RA_MAP(0x0C48BAu, D_800C48BA);   /* u16 melee kill count     */
    RA_MAP(0x0C48D1u, D_800C48D1);   /* u8  continue count       */

    /* ---- Worklist additions (2026-07-31, log 017 — THE region fix) ----------
     * The live set does NOT read the ranking block / equipped item at the USA
     * addresses mapped above; it reads them at the addresses this decomp calls the
     * JAP0 layout (configs/JAP0/sym.bodyprog.txt): D_800C48A0 lives at 0x800C6DD0,
     * g_Inventory_EquippedItem at 0x800B0544. The 0x0C48xx / 0x0AE184 entries above
     * were therefore DEAD (nothing sampled them) and the ending / rank / weapon
     * achievements all read unmapped memory -> never evaluated. Map the SAME
     * pointer-free globals at the offsets the set actually reads (verified byte-for-
     * byte: all twelve 0x800C6Dxx/0x800C6E01 addresses match the ranking field
     * offsets, JAP0 = USA + 0x2530). Weapon achievements read the equipped item;
     * ending/rank achievements read the ranking stats. */
    RA_MAP(0x0B0544u, g_Inventory_EquippedItem); /* Drillin/Chainsaw/Weeb/Gift */
    RA_MAP(0x0C6DD0u, D_800C48A0);   /* s16 savegame count       */
    RA_MAP(0x0C6DD2u, D_800C48A2);   /* u16 gameplay hours       */
    RA_MAP(0x0C6DD6u, D_800C48A6);   /* u16 walk distance        */
    RA_MAP(0x0C6DDCu, D_800C48AC);   /* u16 picked-up item count */
    RA_MAP(0x0C6DDEu, D_800C48AE);   /* u8  minutes              */
    RA_MAP(0x0C6DE0u, D_800C48B0);   /* u8  clear-game count     */
    RA_MAP(0x0C6DE2u, D_800C48B2);   /* u8  ending flags         */
    RA_MAP(0x0C6DE4u, D_800C48B4);   /* u8  special-item count   */
    RA_MAP(0x0C6DE5u, D_800C48B5);   /* s8  computed rank/score  */
    RA_MAP(0x0C6DE8u, D_800C48B8);   /* u16 ranged kill count    */
    RA_MAP(0x0C6DEAu, D_800C48BA);   /* u16 melee kill count     */
    RA_MAP(0x0C6E01u, D_800C48D1);   /* u8  continue count       */

    /* libsd pitch table `u16 PitchTbl[12][128]` (bodyprog/libsd.h, 0xC00). The set
     * reads 0x800B2214/0x800B2218, inside this const table; the port's copy is
     * byte-identical so the reads return the genuine retail values. */
    RA_MAP(0x0B1728u, PitchTbl);

    /* Deliberately NOT mapped (verified 2026-07-31, subagent a93f036f):
     *  - 0x25740/45/49: an UNNAMED .rodata gap between SFX_PAIRS (ends 0x25320)
     *    and the 12x16 font table -- not inside SFX_PAIRS (only 0x64 bytes), no
     *    single named C object to bind. If a real achievement needs it, the culprit
     *    dump will name it and we resolve that specific object.
     *  - D_800C4449, MSTACK, dire, load_buf, StFunc1/2, Clear, VWD0: PSY-Q library
     *    internals (libcd/libgs/libsd) with NO pointer-free port C global. They
     *    keep reading 0 via the RAM fallback (safe). */

    /* Overlap guard: Ra_ReadMemory returns on FIRST match, so an oversized
     * region silently shadows every later one inside its span -- exactly the
     * sizeof(g_SysWork)-under-SH_PC_PORT bug that blocked ALL gameplay unlocks.
     * Name any overlap at boot so that class can never hide again. */
    {
        int i, j;
        for (i = 0; i < s_mapCount; i++)
            for (j = i + 1; j < s_mapCount; j++)
                if (s_map[i].start < s_map[j].start + s_map[j].size &&
                    s_map[j].start < s_map[i].start + s_map[i].size)
                    SH_DBG("[RA] MAP OVERLAP: %s (0x%X+0x%X) vs %s (0x%X+0x%X)",
                           s_map[i].name, s_map[i].start, s_map[i].size,
                           s_map[j].name, s_map[j].start, s_map[j].size);
    }
}

/* First few distinct pages the set touches, hit or miss: reveals which build's
 * address space the conditions were authored in. And a per-page miss log so a
 * real session's D: log is the precise worklist of regions still to map. */
#define RA_SAMPLE_MAX 8
#define RA_MISS_MAX   64
static uint32_t s_samplePage[RA_SAMPLE_MAX];
static int      s_sampleCount;
static uint32_t s_missAddr[RA_MISS_MAX];
static int      s_missCount;
static int      s_missOverflow;

static void Ra_NoteRead(uint32_t address, int hit)
{
    int i;
    const uint32_t page = address & ~0xFFu;
    for (i = 0; i < s_sampleCount; i++)
        if (s_samplePage[i] == page)
            return;
    if (s_sampleCount < RA_SAMPLE_MAX)
    {
        s_samplePage[s_sampleCount++] = page;
        SH_DBG("[RA] set reads 0x80%06X (%s)", address, hit ? "mapped" : "UNMAPPED");
    }
}

static void Ra_NoteMiss(uint32_t address)
{
    int i;
    for (i = 0; i < s_missCount; i++)
        if ((s_missAddr[i] & ~0xFFu) == (address & ~0xFFu))
            return;
    if (s_missCount >= RA_MISS_MAX)
    {
        if (!s_missOverflow)
        {
            s_missOverflow = 1;
            SH_DBG("[RA] unmapped-read log full; further misses suppressed");
        }
        return;
    }
    s_missAddr[s_missCount++] = address;
    SH_DBG("[RA] achievement read of unmapped PSX address 0x80%06X - needs a translation entry",
           address);
}

/*
 * The set identifies which regional build it is running on by reading the PSX
 * boot executable's serial out of RAM -- each ALT group is one region, gated on
 * a big-endian string compare ("SLUS..." = USA). A native recompile has no PSX
 * executable in memory, so every gate failed and no ALT group could activate:
 * that, not address coverage, is why nothing unlocked on PC until this was faked.
 * We answer with the USA serial regardless of the mounted disc because the port
 * has ONE struct layout and our map is the USA one -- so the USA group is the
 * only one whose addresses we serve correctly. Serial is stored WITHOUT the dot:
 * "SLUS_00707" (the probe is 32-bit big-endian: 'SLUS' @+0, skip the separator
 * @+4, '0070' @+5, '7' @+9 -- the standard RA PSX serial parse).
 *
 * The set reads the serial at TWO addresses. The complete memref dump (log 015)
 * proved that ALL 65 core achievements read 0x800257{40,45,49} -- THIS is the gate
 * the Silent Hill set actually uses, and with it returning 0 nothing could ever
 * unlock on Xbox (e.g. Split Head / "Who's Afraid of a Reptile?" reads only this
 * trio). 0x24C10 is kept too (harmless if unused); both serve the same bytes.
 */
#define RA_BOOTID_BASE  0x024C10u
#define RA_SERIAL2_BASE 0x025740u   /* the address the SH set's region gate reads */
static const unsigned char s_bootId[] = {
    'S','L','U','S','_','0','0','7','0','7',' ',' ',' ',' ',' ',' '
};

/* Serve the fake serial for either base; 0 if `address` is in neither region. */
static uint32_t Ra_ServeSerial(uint32_t base, uint32_t address,
                               uint8_t* buffer, uint32_t num_bytes)
{
    if (address >= base && address < base + sizeof(s_bootId))
    {
        const uint32_t avail = (uint32_t)(base + sizeof(s_bootId) - address);
        const uint32_t n     = (num_bytes < avail) ? num_bytes : avail;
        if (buffer)
            memcpy(buffer, s_bootId + (address - base), n);
        return n;
    }
    return 0;
}

static uint32_t RC_CCONV Ra_ReadMemory(uint32_t address, uint8_t* buffer,
                                       uint32_t num_bytes, rc_client_t* client)
{
    int i;
    (void)client;

    if (num_bytes == 0)
        return 0;

    {
        uint32_t n = Ra_ServeSerial(RA_BOOTID_BASE, address, buffer, num_bytes);
        if (n == 0)
            n = Ra_ServeSerial(RA_SERIAL2_BASE, address, buffer, num_bytes);
        if (n)
            return n;
    }

    for (i = 0; i < s_mapCount; i++)
    {
        const s_RaRegion* r = &s_map[i];
        if (address >= r->start && address < r->start + r->size)
        {
            const uint32_t avail = r->start + r->size - address;
            const uint32_t n     = (num_bytes < avail) ? num_bytes : avail;
            if (buffer)
                memcpy(buffer, (const unsigned char*)r->native + (address - r->start), n);
            Ra_NoteRead(address, 1);
            return n;
        }
    }

    Ra_NoteRead(address, 0);
    Ra_NoteMiss(address);

    /* Anything inside the PSX's 2 MB of RAM MUST report as readable even with no
     * native global behind it. rcheevos treats a 0 return as "invalid address"
     * and rc_client_invalidate_memref_achievements then DISABLES every achievement
     * whose trigger touches it -- one unmapped byte took the whole 66-achievement
     * set out on PC (all reported UNSUPPORTED). Zeros are merely wrong for that
     * operand; refusing the read is fatal. */
    if (address < 0x200000u)
    {
        if (buffer)
            memset(buffer, 0, num_bytes);
        return num_bytes;
    }
    return 0;
}

/* Does some native region (or the boot-id shim) back this RA/RAM offset? Mirrors
 * the containment test in Ra_ReadMemory so the worklist agrees with what actually
 * gets served. */
static int Ra_AddrServed(uint32_t address)
{
    int i;
    if (address >= RA_BOOTID_BASE && address < RA_BOOTID_BASE + sizeof(s_bootId))
        return 1;
    if (address >= RA_SERIAL2_BASE && address < RA_SERIAL2_BASE + sizeof(s_bootId))
        return 1;
    for (i = 0; i < s_mapCount; i++)
        if (address >= s_map[i].start && address < s_map[i].start + s_map[i].size)
            return 1;
    return 0;
}

/* One-shot at game-load: walk the parsed memref list (every address the WHOLE set
 * -- all achievements + leaderboards + rich-presence -- reads) and print each
 * distinct one that no native region backs. This is the authoritative, finite
 * worklist for finishing the translation table: unlike the per-frame miss log it
 * needs no gameplay and can't miss a condition that this session didn't happen to
 * evaluate. Distinct unmapped addresses are deduped and capped so the D: log stays
 * bounded even if the set reads hundreds of scratch bytes. */
#define RA_WORKLIST_MAX 160
static void Ra_DumpWorklist(rc_client_t* client)
{
    const rc_client_game_info_t* game;
    const rc_memrefs_t*          memrefs;
    const rc_memref_list_t*      list;
    uint32_t             seen[RA_WORKLIST_MAX];    /* distinct unmapped addresses */
    const rc_memref_t*   seenRef[RA_WORKLIST_MAX]; /* the memref backing each     */
    uint32_t total    = 0;   /* every memref entry, served or not          */
    uint32_t unserved = 0;   /* raw entries with no backing region          */
    uint32_t distinct = 0;   /* distinct unserved addresses actually logged */

    if (!client)
        return;
    game = client->game;
    if (!game || !game->runtime.memrefs)
    {
        SH_DBG("[RA] worklist: no parsed memrefs (set not loaded?)");
        return;
    }

    memrefs = game->runtime.memrefs;
    for (list = &memrefs->memrefs; list != NULL; list = list->next)
    {
        int i;
        for (i = 0; i < (int)list->count; i++)
        {
            const uint32_t addr = list->items[i].address;
            const unsigned sz   = (unsigned)list->items[i].value.size;
            uint32_t       j;
            int            dup   = 0;

            total++;
            if (Ra_AddrServed(addr))
                continue;
            unserved++;

            for (j = 0; j < distinct; j++)
                if (seen[j] == addr) { dup = 1; break; }
            if (dup)
                continue;

            if (distinct < RA_WORKLIST_MAX)
            {
                seen[distinct]    = addr;
                seenRef[distinct] = &list->items[i];
                distinct++;
                /* sz is an RC_MEMSIZE_* code, not a byte count -- it tells the
                 * mapper the operand width (8/16/24/32/bit) at this address. */
                SH_DBG("[RA] worklist UNMAPPED 0x80%06X (memsize=%u)", addr, sz);
            }
        }
    }

    SH_DBG("[RA] worklist: %u memrefs total, %u unserved, %u distinct-unmapped listed%s",
           total, unserved, distinct,
           (distinct >= RA_WORKLIST_MAX) ? " (cap hit -- raise RA_WORKLIST_MAX)" : "");
    if (unserved == 0)
    {
        SH_DBG("[RA] worklist: table COMPLETE -- every address the set reads is backed");
        return;
    }

    /* Name the culprit: for every still-unmapped memref, print which achievement
     * reads it (rc_trigger_contains_memref is rcheevos' own membership test). This
     * turns the raw address list into "achievement <title> can't evaluate because
     * it reads 0xADDR" -- e.g. exactly which unmapped operand is blocking the Split
     * Head unlock, by name, with no gameplay needed. */
    {
        const rc_runtime_t* rt = &game->runtime;
        uint32_t ti;
        for (ti = 0; ti < rt->trigger_count; ti++)
        {
            const rc_runtime_trigger_t* run = &rt->triggers[ti];
            uint32_t k;
            if (!run->trigger)
                continue;
            for (k = 0; k < distinct; k++)
            {
                if (rc_trigger_contains_memref(run->trigger, seenRef[k]))
                {
                    const rc_client_achievement_t* ach =
                        rc_client_get_achievement_info(client, run->id);
                    SH_DBG("[RA] culprit: achiev id=%u '%s' reads unmapped 0x80%06X",
                           (unsigned)run->id,
                           (ach && ach->title) ? ach->title : "?",
                           seenRef[k]->address);
                }
            }
        }
    }
}

/* Full core roster with lock state, at load. Every LOCKED line is an achievement
 * that CAN still fire on this console (already-unlocked ones are inert here); the
 * UNLK lines name what the account already has, so a PC-earned unlock is never
 * mistaken for proof of the Xbox path. */
/* A sample achievement (first one with a badge in the roster) captured at load, so
 * the Xbox-Options "RetroAchievements" action can fire a full MOCK unlock card --
 * real badge image + card text -- on demand, to preview/prove the notification UI
 * without waiting for a real unlock. */
static char     s_sampleTitle[64];
static char     s_sampleBadge[16];
static unsigned s_samplePoints;

static void Ra_DumpRoster(rc_client_t* client)
{
    rc_client_achievement_list_t* list;
    uint32_t b, a;
    uint32_t locked = 0, unlocked = 0;

    list = rc_client_create_achievement_list(client,
              RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
              RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
    {
        SH_DBG("[RA] roster: unavailable");
        return;
    }

    for (b = 0; b < list->num_buckets; b++)
    {
        const rc_client_achievement_bucket_t* bk = &list->buckets[b];
        for (a = 0; a < bk->num_achievements; a++)
        {
            const rc_client_achievement_t* ac = bk->achievements[a];
            const int isUnlocked =
                (ac->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) || ac->unlocked;
            if (isUnlocked) unlocked++; else locked++;

            /* Keep the first achievement that has a badge as the menu self-test
             * sample (real title + points + badge image). */
            if (!s_sampleBadge[0] && ac->badge_name[0])
            {
                strncpy(s_sampleTitle, ac->title ? ac->title : "Achievement",
                        sizeof(s_sampleTitle) - 1);
                s_sampleTitle[sizeof(s_sampleTitle) - 1] = '\0';
                strncpy(s_sampleBadge, ac->badge_name, sizeof(s_sampleBadge) - 1);
                s_sampleBadge[sizeof(s_sampleBadge) - 1] = '\0';
                s_samplePoints = (unsigned)ac->points;
            }
            SH_DBG("[RA] ach %s id=%u %up '%s'",
                   isUnlocked ? "UNLK" : "lock",
                   (unsigned)ac->id, (unsigned)ac->points,
                   ac->title ? ac->title : "?");
        }
    }
    SH_DBG("[RA] roster: %u locked, %u unlocked (%u total)",
           locked, unlocked, locked + unlocked);
    rc_client_destroy_achievement_list(list);
}

/* ------------------------------------------------------------------------- */
/* HTTP transport: bounded main-thread pump (no worker thread on Xbox)         */
/* ------------------------------------------------------------------------- */

typedef struct s_RaJob
{
    struct s_RaJob*             next;
    char*                       url;
    char*                       post;
    rc_client_server_callback_t callback;
    void*                       callback_data;
} s_RaJob;

static s_RaJob* s_pending;   /* FIFO of server calls rc_client asked us to make */

static char* Ra_Strdup(const char* s)
{
    size_t n;
    char*  p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char*)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void Ra_JobFree(s_RaJob* job)
{
    if (!job)
        return;
    free(job->url);
    free(job->post);
    free(job);
}

/* rc_client asks us to talk to the server. Queue it and return immediately; the
 * bounded pump in Pc_Ra_Update does the blocking request and invokes the callback
 * on the main thread. Queuing (rather than blocking here) avoids deep recursion
 * through rc_client's chained server -> callback -> server load sequence. */
static void RC_CCONV Ra_ServerCall(const rc_api_request_t* request,
                                   rc_client_server_callback_t callback,
                                   void* callback_data, rc_client_t* client)
{
    s_RaJob* job;
    s_RaJob* tail;
    (void)client;

    job = (s_RaJob*)calloc(1, sizeof(*job));
    if (!job)
        return;

    job->url           = Ra_Strdup(request->url);
    /* post_data present (and non-empty) => POST; else Net_XboxHttpRequest does a
     * GET. rcheevos always sends application/x-www-form-urlencoded, which is what
     * Net_XboxHttpRequest assumes, so request->content_type is not needed. */
    job->post          = Ra_Strdup(request->post_data);
    job->callback      = callback;
    job->callback_data = callback_data;

    if (!s_pending)
        s_pending = job;
    else
    {
        for (tail = s_pending; tail->next; tail = tail->next)
            ;
        tail->next = job;
    }
}

/* Do ONE queued request, blocking, and hand the response to rc_client. Returns 1
 * if a job was processed. */
static int Ra_PumpOne(void)
{
    s_RaJob* job = s_pending;
    char*    body = NULL;
    int      len  = 0;
    int      status;
    rc_api_server_response_t response;

    if (!job)
        return 0;
    s_pending = job->next;

    SH_DBG("[RA] http %s %.90s", job->post ? "POST" : "GET", job->url ? job->url : "(null)");
    status = Net_XboxHttpRequest(job->url, job->post, &body, &len);
    SH_DBG("[RA] http -> status=%d bodyLen=%d body='%.60s'", status, len, body ? body : "");

    memset(&response, 0, sizeof(response));
    response.body             = body ? body : "";
    response.body_length      = (size_t)(len > 0 ? len : 0);
    response.http_status_code = status;
    if (job->callback)
        job->callback(&response, job->callback_data);

    if (body)
        free(body);
    Ra_JobFree(job);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* rc_hash custom filereader (nxdk stdio over the BIN path)                    */
/* ------------------------------------------------------------------------- */
/* The BIN is < 2 GB, so 32-bit long offsets are sufficient; opening our own
 * handle keeps rc_hash's seeks independent of cd_xbox's live CD handle. */
static void* RC_CCONV Ra_Fr_Open(const char* path)
{
    return (void*)fopen(path, "rb");
}
static void RC_CCONV Ra_Fr_Seek(void* h, int64_t offset, int origin)
{
    if (h)
        fseek((FILE*)h, (long)offset, origin);
}
static int64_t RC_CCONV Ra_Fr_Tell(void* h)
{
    return h ? (int64_t)ftell((FILE*)h) : 0;
}
static size_t RC_CCONV Ra_Fr_Read(void* h, void* buffer, size_t requested_bytes)
{
    return h ? fread(buffer, 1, requested_bytes, (FILE*)h) : 0;
}
static void RC_CCONV Ra_Fr_Close(void* h)
{
    if (h)
        fclose((FILE*)h);
}

/* ------------------------------------------------------------------------- */
/* Client lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static rc_client_t* s_client;
static int          s_active;      /* set loaded and evaluating */
static int          s_enabled;     /* configured, credentialed, client created */
static char         s_status[64];

static void Ra_Toast(const char* line)
{
    SH_DBG("%s", line);
    if (g_ShOverlayToastLine)
        g_ShOverlayToastLine(line);
}

static void Ra_RefreshStatus(void)
{
    rc_client_user_game_summary_t summary;
    if (!s_client || !s_active)
    {
        s_status[0] = '\0';
        return;
    }
    rc_client_get_user_game_summary(s_client, &summary);
    snprintf(s_status, sizeof(s_status), "%u/%u (%u pts)",
             (unsigned)summary.num_unlocked_achievements,
             (unsigned)summary.num_core_achievements,
             (unsigned)summary.points_unlocked);
}

/* The ONE unlock-notification path: chime + on-screen CARD (badge image top-left,
 * title + "Unlocked - N points" beside it, ~5 s) + the D: log line. Shared by a
 * real RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED and the menu self-test so they render
 * identically. Badge fetch is a BLOCKING one-shot (a brief hitch, fine on a rare
 * unlock or a deliberate menu press). */
static void Ra_ShowUnlock(const char* title, unsigned points, const char* badge_name)
{
    extern void DbgOverlay_XboxUnlock(const char* title, unsigned points);
    extern int  RaBadge_Fetch(const char*);
    extern int  RaSound_PlayUnlock(void);   /* custom achievement.wav (dsound_xbox.c) */

    /* Prefer the user's custom unlock sound (achievement.wav beside the xbe); fall
     * back to the game's own confirm chime if it isn't present / not loadable. */
    if (!RaSound_PlayUnlock())
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
    DbgOverlay_XboxUnlock(title ? title : "Achievement", points);
    SH_DBG("[RA] UNLOCK card '%s' (%u pts) badge=%s",
           title ? title : "?", points,
           (badge_name && badge_name[0]) ? badge_name : "-");
    if (badge_name && badge_name[0])
        RaBadge_Fetch(badge_name);
}

static void RC_CCONV Ra_EventHandler(const rc_client_event_t* event, rc_client_t* client)
{
    char line[128];
    (void)client;
    (void)line;

    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        /* Real unlock: the full card (chime + badge image + title/points, ~5 s),
         * then refresh the X/Y summary. This handler only runs from
         * rc_client_do_frame in settled gameplay (main thread, SPU pumping), so the
         * chime + blocking badge fetch inside Ra_ShowUnlock are safe. Already-
         * unlocked achievements loaded at boot do NOT fire this event. */
        Ra_ShowUnlock(event->achievement->title,
                      (unsigned)event->achievement->points,
                      event->achievement->badge_name);
        Ra_RefreshStatus();
        break;

    case RC_CLIENT_EVENT_GAME_COMPLETED:
        Ra_Toast("All achievements complete!");
        break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
        SH_DBG("[RA] server error: %s",
               event->server_error ? event->server_error->error_message : "unknown");
        break;

    case RC_CLIENT_EVENT_DISCONNECTED:
        Ra_Toast("RetroAchievements offline - unlocks queued");
        break;

    case RC_CLIENT_EVENT_RECONNECTED:
        Ra_Toast("RetroAchievements reconnected");
        break;

    case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
        /* Primed: an achievement's primary condition is met and it's now waiting
         * on its trigger. Diagnostic for the Split Head miss: if it primes here
         * but never TRIGGERS, its final operand is reading wrong (a map-worklist
         * gap) rather than a logic issue. */
        SH_DBG("[RA] primed (challenge active): id=%u %s",
               event->achievement ? (unsigned)event->achievement->id : 0u,
               event->achievement ? event->achievement->title : "?");
        break;

    default:
        break;
    }
}

static void RC_CCONV Ra_LoadGameCallback(int result, const char* error_message,
                                         rc_client_t* client, void* userdata)
{
    (void)userdata;

    if (result != RC_OK)
    {
        /* NO_GAME_LOADED = this disc has no set (or an unrecognized dump); a
         * normal outcome, not an alarm. */
        SH_DBG("[RA] achievement set unavailable: %s",
               error_message ? error_message : "unknown");
        return;
    }

    s_active = 1;
    Ra_RefreshStatus();
    {
        const rc_client_game_t* game = rc_client_get_game_info(client);
        if (game)
            SH_DBG("[RA] matched game %u \"%s\"",
                   (unsigned)game->id, game->title ? game->title : "?");
    }

    /* Print the complete address worklist now that every achievement is parsed. */
    Ra_DumpWorklist(client);

    /* And the full roster + lock state: shows exactly which achievements are still
     * LOCKED (hence testable on THIS console -- an already-unlocked one, e.g. any
     * earned earlier on the PC port under the same account, will NOT re-fire here),
     * their point values, and names the unlocked ones so a PC-earned unlock can't
     * be mistaken for an Xbox one. */
    Ra_DumpRoster(client);
    {
        char line[96];
        snprintf(line, sizeof(line), "RetroAchievements: %s", s_status);
        Ra_Toast(line);
    }
}

static void RC_CCONV Ra_LoginCallback(int result, const char* error_message,
                                      rc_client_t* client, void* userdata)
{
    char hash[33];
    (void)userdata;

    if (result != RC_OK)
    {
        char t[96];
        SH_DBG("[RA] login failed: %s. Check ra_username / ra_token / ra_password in silenthill.cfg.",
               error_message ? error_message : "unknown");
        snprintf(t, sizeof(t), "RA login failed: %s", error_message ? error_message : "check user/password");
        Ra_Toast(t);
        return;
    }

    SH_DBG("[RA] logged in as %s", g_PcConfig.raUsername);
    { char t[80]; snprintf(t, sizeof(t), "RA: signed in as %s", g_PcConfig.raUsername); Ra_Toast(t); }

    if (!g_CdBinPath[0] || g_CdBinPath[0] == 'N') /* "NOT FOUND..." */
    {
        SH_DBG("[RA] no BIN resolved (g_CdBinPath='%s') - cannot identify the game", g_CdBinPath);
        return;
    }

    /* The player's own dump is running, so this hash is the genuine article. */
    if (!rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, g_CdBinPath))
    {
        SH_DBG("[RA] could not hash disc image '%s'", g_CdBinPath);
        return;
    }

    SH_DBG("[RA] disc hash %s", hash);
    rc_client_begin_load_game(client, hash, Ra_LoadGameCallback, NULL);
}

void Pc_Ra_Init(void)
{
    rc_hash_filereader_t fr;

    SH_DBG("[RA] init: enabled=%d user='%s' token=%s passwordLen=%d",
           g_PcConfig.retroAchievements,
           g_PcConfig.raUsername,
           g_PcConfig.raToken[0] ? "present" : "none",
           (int)strlen(g_PcConfig.raPassword));

    if (!g_PcConfig.retroAchievements)
    {
        SH_DBG("[RA] disabled in config (retroachievements = 0) - off");
        return;
    }

    if (!g_PcConfig.raUsername[0] ||
        (!g_PcConfig.raToken[0] && !g_PcConfig.raPassword[0]))
    {
        SH_DBG("[RA] enabled but not signed in - set ra_username and ra_token "
               "(or ra_password) in silenthill.cfg");
        return;
    }

    /* Network first: DHCP can take a couple of seconds, paid once here only when
     * RA is enabled. No network => no achievements. */
    if (Net_XboxBringUp() < 0)
    {
        SH_DBG("[RA] network bring-up failed - RA disabled this session");
        return;
    }

    Ra_BuildMap();

    /* Plain HTTP: Net_XboxHttpRequest has no TLS, so point rcheevos at the http
     * endpoint. Unlocks still post to the real account. */
    rc_api_set_host("http://retroachievements.org");

    /* Custom filereader so rc_hash reads the BIN through nxdk stdio (its own
     * handle, independent of cd_xbox's live CD handle). */
    memset(&fr, 0, sizeof(fr));
    fr.open  = Ra_Fr_Open;
    fr.seek  = Ra_Fr_Seek;
    fr.tell  = Ra_Fr_Tell;
    fr.read  = Ra_Fr_Read;
    fr.close = Ra_Fr_Close;
    rc_hash_init_custom_filereader(&fr);

    s_client = rc_client_create(Ra_ReadMemory, Ra_ServerCall);
    if (!s_client)
    {
        SH_DBG("[RA] client creation failed - disabled");
        return;
    }

    /* THE line that makes login work on Xbox: rc_client uses its OWN host
     * (client->state.host), which defaults to https://retroachievements.org and
     * IGNORES the global rc_api_set_host above. Our transport has no TLS, so
     * every request went out https:// and got rejected ("No response"). Point the
     * CLIENT at the plain-HTTP endpoint. (rc_api_set_host is kept for any rc_api-
     * direct paths; the badge host is plain-HTTP in ra_badge_xbox.c already.) */
    rc_client_set_host(s_client, "http://retroachievements.org");

    rc_client_set_hardcore_enabled(s_client, 0); /* softcore, permanently */
    rc_client_set_event_handler(s_client, Ra_EventHandler);

    s_enabled = 1;

    /* Prefer the stored token (never sends the password). Fall back to a password
     * login, which fetches and caches a token inside rc_client for the session;
     * the user can copy a token into the config later to skip the password. */
    if (g_PcConfig.raToken[0])
        rc_client_begin_login_with_token(s_client, g_PcConfig.raUsername,
                                         g_PcConfig.raToken, Ra_LoginCallback, NULL);
    else
        rc_client_begin_login_with_password(s_client, g_PcConfig.raUsername,
                                            g_PcConfig.raPassword, Ra_LoginCallback, NULL);
}

void Pc_Ra_Update(void)
{
    if (!s_enabled || !s_client)
        return;

    /* Bounded pump: one blocking request per frame keeps the boot login/load
     * handshake from freezing a frame for a second, and keeps every rc_client_*
     * call (including the response callbacks) on the main thread. */
    Ra_PumpOne();

    /* Evaluate only in settled gameplay; menus, FMV and load fades hold no
     * coherent world state. rc_client_idle still services pings/retries.
     * !DemoActive: the attract demo runs real gameplay state -- without this a
     * menu-idle demo loop could earn achievements (PC gates the same way). */
    if (s_active &&
        g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState   == SysState_Gameplay &&
        !(g_SysWork.sysFlags & SysFlag_DemoActive))
    {
        rc_client_do_frame(s_client);
    }
    else
    {
        rc_client_idle(s_client);
    }
}

void Pc_Ra_Shutdown(void)
{
    /* MainLoop never returns on Xbox (exit = reboot), so this is here for parity
     * only. Drain the queue and destroy the client. */
    if (s_client)
    {
        rc_client_destroy(s_client);
        s_client = NULL;
    }
    while (s_pending)
    {
        s_RaJob* j = s_pending;
        s_pending = j->next;
        Ra_JobFree(j);
    }
    s_enabled = 0;
    s_active  = 0;
}

int Pc_Ra_IsActive(void)
{
    return s_active;
}

const char* Pc_Ra_StatusLine(void)
{
    return s_status;
}

/* Menu action (Xbox Options "RetroAchievements" row). When signed in with the set
 * loaded, this doubles as an on-demand SELF-TEST: it fires a full mock unlock card
 * -- a real achievement's badge image + "Title / Unlocked - N points" -- so the
 * notification UI can be previewed and proven any time from the menu, without
 * waiting for a real unlock. When not signed in / RA off / set not yet loaded, it
 * falls back to a plain status toast. */
void Pc_Ra_StatusToast(void)
{
    const rc_client_user_t* user;
    char                    line[96];

    if (!g_PcConfig.retroAchievements)
    {
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);   /* menu feedback (no card in this branch) */
        Ra_Toast("RA: off (set retroachievements=1)");
        return;
    }

    user = s_client ? rc_client_get_user_info(s_client) : NULL;
    if (!user)
    {
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        Ra_Toast("RA: not signed in");
        return;
    }

    SH_DBG("[RA] menu self-test: signed in as %s (%u pts softcore), set=%s",
           user->username ? user->username : "?",
           (unsigned)user->score_softcore, s_active ? "loaded" : "not loaded");

    if (s_active && s_sampleBadge[0])
    {
        /* LOCAL-ONLY preview of the unlock card (badge image + layout + sound).
         * This path is display-only -- Ra_ShowUnlock never awards or contacts the
         * server; only a real RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED posts an unlock.
         * Use a self-describing title (not the sample achievement's real name) so
         * the preview can never be mistaken for an actual unlock. */
        Ra_ShowUnlock("Notification Preview", s_samplePoints, s_sampleBadge);
    }
    else
    {
        const char* nm = (user->display_name && user->display_name[0]) ? user->display_name
                       : (user->username ? user->username : "?");
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        snprintf(line, sizeof(line), "RA: signed in as %s (%u pts)",
                 nm, (unsigned)user->score_softcore);
        Ra_Toast(line);
    }
}

#else /* !SH_RETROACHIEVEMENTS */

void Pc_Ra_StatusToast(void)
{
    extern void (*g_ShOverlayToastLine)(const char* line);
    if (g_ShOverlayToastLine)
        g_ShOverlayToastLine("RA: not built in");
}

void        Pc_Ra_Init(void)       {}
void        Pc_Ra_Update(void)     {}
void        Pc_Ra_Shutdown(void)   {}
int         Pc_Ra_IsActive(void)   { return 0; }
const char* Pc_Ra_StatusLine(void) { return ""; }

#endif /* SH_RETROACHIEVEMENTS */
