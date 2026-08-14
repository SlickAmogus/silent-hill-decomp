/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_retroachievements.c - RetroAchievements (softcore) integration.
 *
 * The port runs the player's own PSX disc image, so rc_hash over that file
 * yields the genuine RetroAchievements hash for Silent Hill and the official
 * achievement set applies. What it does NOT get for free is memory: RA
 * conditions read absolute PSX RAM addresses, while this port's state lives in
 * ordinary native globals placed by the linker. Pc_RaReadMemory bridges that
 * with an explicit descriptor table (see s_map) rather than exposing a fake RAM
 * blob -- g_PsxRam holds only file/overlay staging buffers and contains no game
 * state at all.
 *
 * Threading: rc_client is single-threaded. HTTP runs on a worker thread, but
 * completions are queued and dispatched from Pc_Ra_Update, so every rc_client_*
 * call happens on the main thread.
 *
 * Softcore is forced. The port has quick save/load, debug controls, free
 * cameras, randomizer and cheat keys; hardcore would require an integrity
 * regime none of that can satisfy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pc_retroachievements.h"
#include "pc_ra_browser.h"

#ifdef SH_RETROACHIEVEMENTS

#include <SDL.h>
#include <rc_client.h>
#include <rc_hash.h>
#include <rc_consoles.h>
#include "rc_client_internal.h"   /* parsed triggers, for the RAWHY diagnostic */

#include "game.h"
#include "main/fileinfo.h"   /* g_GameRegion */
#include "bodyprog/savegame.h"
/* Globals the live achievement set reads; see the translation table below. */
#include "bodyprog/bodyprog.h"                      /* g_WorldGfxWork, MAP_EFFECTS_INFOS, g_KcetLogoImg */
#include "bodyprog/demo.h"                          /* g_Demo_IsLoadingChunks */
#include "bodyprog/item_screens.h"                  /* inventory globals */
#include "bodyprog/collision/collision.h"           /* g_ActiveCollisionTriggers */
#include "bodyprog/sound/sound_system.h"            /* g_XaItemData */
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_MapEventLastUsedItem */
#include "pc_config.h"
#include "sh_log.h"
#include "dbg_overlay.h"
#include "pc_ra_http.h"
#include "map_registry.h"   /* RAWHY names the map index an operand compares */
#include "pc_ra_toast.h"

extern const char* PcPort_GetGameDiscPath(void);

/* Defined below; the unlock event handler above it kicks the badge download. */
static void Pc_RaFetchBadge(const char* badgeName);

/* RA's server injects informational pseudo-achievements for a client or a disc
 * it does not recognise: an unregistered client gets "Warning: Unknown
 * Emulator", and a hash that identifies the game but has no supported set gets
 * "Unsupported Game Version". They are notices, not part of any set -- zero
 * points, and they "unlock" the moment a session starts, so on an unmatched
 * disc the player is greeted by two trophy popups for achievements nobody
 * earned. Suppressed from BOTH the unlock toast and the browser list.
 *
 * Gated on zero points as well as the title, so a real achievement that merely
 * begins with one of these words can never be swallowed. */
static int Pc_RaIsPseudoAchievement(const char* title, unsigned points)
{
    static const char* const prefixes[] = {
        "Warning:",
        "Unsupported Game Version",
        "Unsupported Emulator",
        "Unknown Emulator",
    };
    size_t i;

    if (title == NULL || points != 0)
        return 0;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        if (strncmp(title, prefixes[i], strlen(prefixes[i])) == 0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* PSX address -> native global translation                                   */
/* ------------------------------------------------------------------------- */
/*
 * RA addresses are RAM offsets (0x000000-0x1FFFFF), i.e. PSX 0x800xxxxx with
 * the segment stripped. Two regions carry effectively all observable game
 * state, and they are adjacent in PSX RAM (SysWork ends exactly where GameWork
 * begins, which cross-checks both base addresses):
 *
 *   0x0B9FC0  g_SysWork   0x2768 bytes  (sys state, player work, camera)
 *   0x0BC728  g_GameWork  0x05D8 bytes  (options, autosave, savegame @ +0x30C)
 *
 * s_GameWork and s_Savegame carry no SH_PC_PORT conditionals, so their layouts
 * are byte-identical to retail. s_SysWork has exactly one: field_2510 widens
 * from s32 to a real pointer (game.h), so everything from 0x2514 on sits 4
 * bytes higher natively -- hence the split entry and the deliberate hole over
 * the pointer itself (an RA rule reading it would only ever have seen a PSX
 * handle value, meaningless here).
 */
#define RA_SYSWORK_BASE   0x0B9FC0u
#define RA_SYSWORK_SPLIT  0x2510u          /* first widened member  */
#define RA_SYSWORK_SIZE   0x2768u          /* retail sizeof         */
#define RA_GAMEWORK_BASE  0x0BC728u
#define RA_GAMEWORK_SIZE  0x05D8u

typedef struct
{
    uint32_t    start;      /* RA/RAM offset of the first mapped byte */
    uint32_t    size;       /* bytes covered                          */
    const void* native;     /* native bytes backing `start`           */
    const char* name;
} s_RaRegion;

static s_RaRegion* s_map;
static int         s_mapCap;
static int        s_mapCount;

/* ra_hash_override: report a different disc hash than the mounted image has.
 *
 * RA keys the achievement set on the disc hash alone, so a disc it has no entry
 * for loads nothing -- an NTSC-J image lands in RA's "Unsupported Game Version"
 * bucket at 0/0, and a fan-translated image hashes to something RA has never
 * seen at all. The achievement LOGIC is region-agnostic here: the port compiles
 * one struct layout whatever disc is mounted, and it already synthesises the
 * USA boot serial at 0x024C10 so the set's region gate opens regardless. Only
 * the reported hash stands between those discs and a working set.
 *
 * OFF BY DEFAULT, and deliberately opt-in: with this set, unlocks post to the
 * player's real account under a hash that is not the disc they are running.
 * That is the player's call to make on their own account -- this build is
 * softcore-only, so nothing here touches hardcore leaderboards -- but it is not
 * a decision the port should make for anyone silently.
 *
 * Accepts a literal 32-char MD5, or an alias below. Aliases are only ever added
 * from a hash OBSERVED loading the real set, never guessed: a wrong constant
 * would fail to match and read as a bug rather than as a bad value. */
typedef struct
{
    const char* name;
    const char* hash;
} s_RaHashAlias;

static const s_RaHashAlias s_raHashAliases[] = {
    /* Both verified loading game 11252 "Silent Hill" with the full
     * 66-achievement set. Add further entries only from a log line that did
     * the same.
     *
     * `usa` is the one to prefer. The port always presents itself to the
     * achievement logic AS the USA build -- one compiled struct layout, the USA
     * symbol map, and a synthesised USA boot serial at 0x024C10 -- so the USA
     * disc is the only configuration where the code, the addresses and the
     * disc's own data all agree. */
    { "usa",  "469d5cf1428d3f6f2234aa5be058f3ec" },
    { "ntsc-u", "469d5cf1428d3f6f2234aa5be058f3ec" },
    { "pal",  "e6f638d44f54d7498a17244453722eb5" },
};

static int Pc_RaIsMd5(const char* text)
{
    int i;

    if (text == NULL)
        return 0;
    for (i = 0; i < 32; i++)
    {
        if (!isxdigit((unsigned char)text[i]))
            return 0;
    }
    return text[32] == '\0';
}

/* NULL = report the real hash (the default).
 *
 * `realHash` is the mounted disc's own hash. An override is SKIPPED when that
 * already matches a hash RA supports, so the launcher's "hash bypass" toggle
 * can be left on permanently: it does nothing on a USA or PAL disc and only
 * engages for the discs that actually need it (NTSC-J, fan translations).
 * Deciding this here rather than in the launcher is deliberate -- it depends on
 * which disc is mounted right now, which the launcher cannot know. */
static const char* Pc_RaResolveHashOverride(const char* realHash)
{
    const char* value = g_PcConfig.raHashOverride;
    size_t      i;

    if (value == NULL || value[0] == '\0')
        return NULL;

    for (i = 0; i < sizeof(s_raHashAliases) / sizeof(s_raHashAliases[0]); i++)
    {
        if (realHash != NULL && SDL_strcasecmp(realHash, s_raHashAliases[i].hash) == 0)
        {
            SH_DBG("[RA] ra_hash_override ignored: this disc (%s) is already one "
                   "RetroAchievements supports", s_raHashAliases[i].name);
            return NULL;
        }
    }

    for (i = 0; i < sizeof(s_raHashAliases) / sizeof(s_raHashAliases[0]); i++)
    {
        /* SDL_strcasecmp, not _stricmp: the latter is an MSVC spelling and this
         * build also targets Linux. */
        if (SDL_strcasecmp(value, s_raHashAliases[i].name) == 0)
            return s_raHashAliases[i].hash;
    }
    if (Pc_RaIsMd5(value))
        return value;

    SH_DBG("[RA] ra_hash_override '%s' is neither a known alias nor a 32-char "
           "MD5 - ignored, reporting the real disc hash", value);
    return NULL;
}

/* Grow the map by one slot. The only reason RA_MAP may decline is a failed
 * allocation. */
static int Pc_RaMapReserve(void)
{
    s_RaRegion* grown;
    int         newCap;

    if (s_mapCount < s_mapCap)
        return 1;
    newCap = s_mapCap ? s_mapCap * 2 : 64;
    grown  = (s_RaRegion*)realloc(s_map, (size_t)newCap * sizeof(*s_map));
    if (grown == NULL)
        return 0;
    s_map    = grown;
    s_mapCap = newCap;
    return 1;
}

/* Map one native global at its retail PSX offset. sizeof() is the authority on
 * length, so a translation can never read past the native object even where the
 * symbol map claims a larger span.
 *
 * The capacity test was sizeof(s_map)/sizeof(s_map[0]). That was right while
 * s_map was a fixed array, but it became a heap pointer when the table outgrew
 * its original 24 entries -- and 8/24 is 0, so the guard read `s_mapCount < 0`
 * and every hand-mapped entry was silently dropped. Only harmless so far
 * because Pc_RaBuildMapAuto returns before the hand table is built; the moment
 * the symbol files are absent, this fallback has to actually work. */
#define RA_MAP(psxOffset, sym) do { \
    if (Pc_RaMapReserve()) { \
        s_map[s_mapCount].start  = (psxOffset); \
        s_map[s_mapCount].size   = (uint32_t)sizeof(sym); \
        s_map[s_mapCount].native = (const void*)&(sym); \
        s_map[s_mapCount].name   = #sym; \
        s_mapCount++; \
    } \
} while (0)

/* Build the address map automatically from the decomp symbol files.
 *
 * This is the whole game rather than a hand-picked handful: the port links with
 * -Wl,--export-all-symbols, so every non-static global is resolvable by name at
 * runtime, and configs/USA/sym.*.txt says which PSX address each of those names
 * had. Pairing the two reconstructs the PSX address space wholesale, which is
 * what the achievement set is actually reading.
 *
 * Entries are sorted and clamped so one symbol's declared span can never run
 * into the next symbol's address -- a sym "size" is often just the distance to
 * the following entry and would otherwise cover globals that are adjacent on
 * PSX but nowhere near each other natively. */
static int Pc_RaMapCompare(const void* a, const void* b)
{
    const s_RaRegion* ra = (const s_RaRegion*)a;
    const s_RaRegion* rb = (const s_RaRegion*)b;
    return (ra->start < rb->start) ? -1 : (ra->start > rb->start) ? 1 : 0;
}

static void Pc_RaLoadSymbolFile(const char* path, int* found, int* missing)
{
    char  line[512];
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        SH_DBG("[RA] symbol file missing: %s", path);
        return;
    }
    while (fgets(line, sizeof(line), f))
    {
        char     name[128];
        unsigned addr = 0, size = 0;
        char*    eq;
        char*    sz;
        void*    native;
        size_t   n;

        eq = strchr(line, '=');
        if (!eq) continue;
        n = (size_t)(eq - line);
        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '	')) n--;
        if (n == 0 || n >= sizeof(name)) continue;
        memcpy(name, line, n);
        name[n] = ' ';
        if (sscanf(eq + 1, " %x", &addr) != 1) continue;
        /* Only main RAM; ignore scratchpad/IO/BIOS entries. */
        if ((addr & 0xFF000000u) != 0x80000000u) continue;

        /* Only ~5% of entries carry an explicit size:. Leave the rest at 0 and
         * let the sort pass below infer the span from the next symbol's
         * address -- defaulting to 4 truncates every array and struct, which
         * silently drops most of the game state back out of the map. */
        sz = strstr(line, "size:");
        if (sz) sscanf(sz + 5, "%x", &size);

        native = Pc_RaSymbolLookup(name);
        if (!native) { (*missing)++; continue; }

        if (s_mapCount >= s_mapCap)
        {
            int newCap = s_mapCap ? s_mapCap * 2 : 1024;
            s_RaRegion* grown = (s_RaRegion*)realloc(s_map, (size_t)newCap * sizeof(*s_map));
            if (!grown) break;
            s_map = grown; s_mapCap = newCap;
        }
        s_map[s_mapCount].start  = addr & 0x1FFFFFu;
        s_map[s_mapCount].size   = size;
        s_map[s_mapCount].native = native;
        s_map[s_mapCount].name   = NULL;
        s_mapCount++;
        (*found)++;
    }
    fclose(f);
}

static void Pc_RaBuildMapAuto(void)
{
    int found = 0, missing = 0, i;

    Pc_RaLoadSymbolFile("gamedata/ra/sym.bodyprog.txt", &found, &missing);
    Pc_RaLoadSymbolFile("gamedata/ra/sym.main.txt", &found, &missing);
    if (s_mapCount <= 0)
        return;

    qsort(s_map, (size_t)s_mapCount, sizeof(*s_map), Pc_RaMapCompare);

    /* Resolve spans: an unsized symbol runs up to the next one, and a sized
     * symbol is clamped so it can never bleed past it either. */
    for (i = 0; i < s_mapCount; i++)
    {
        uint32_t gap = (i + 1 < s_mapCount)
                     ? (s_map[i+1].start - s_map[i].start)
                     : 4u;
        if (s_map[i].size == 0 || s_map[i].size > gap)
            s_map[i].size = gap;
        if (s_map[i].size == 0)
            s_map[i].size = 4u;
    }

    /* s_SysWork is the one struct whose PC layout differs (field_2510 widens
     * s32 -> pointer), so everything above it sits +4 natively. Split the
     * auto-generated entry rather than let it map straight through. */
    for (i = 0; i < s_mapCount; i++)
    {
        if (s_map[i].start == RA_SYSWORK_BASE && s_map[i].size > RA_SYSWORK_SPLIT)
        {
            s_map[i].size = RA_SYSWORK_SPLIT;
            if (s_mapCount < s_mapCap)
            {
                s_map[s_mapCount].start  = RA_SYSWORK_BASE + RA_SYSWORK_SPLIT + 4u;
                s_map[s_mapCount].size   = RA_SYSWORK_SIZE - (RA_SYSWORK_SPLIT + 4u);
                s_map[s_mapCount].native = (const u8*)s_map[i].native + RA_SYSWORK_SPLIT + 8u;
                s_map[s_mapCount].name   = "SysWork.hi";
                s_mapCount++;
                qsort(s_map, (size_t)s_mapCount, sizeof(*s_map), Pc_RaMapCompare);
            }
            break;
        }
    }

    SH_DBG("[RA] address map: %d globals mapped from symbol files (%d names not exported)",
           found, missing);
}

static void Pc_RaBuildMap(void)
{
    s_mapCount = 0;
    Pc_RaBuildMapAuto();
    if (s_mapCount > 0)
        return;   /* symbol files present: they cover everything below */
    SH_DBG("[RA] symbol files unavailable - falling back to the built-in map");

    s_mapCap = 32;
    s_map = (s_RaRegion*)realloc(s_map, (size_t)s_mapCap * sizeof(*s_map));
    if (!s_map) { s_mapCap = 0; return; }

    /* SysWork below the widened pointer: retail offsets hold exactly. */
    s_map[s_mapCount].start  = RA_SYSWORK_BASE;
    s_map[s_mapCount].size   = RA_SYSWORK_SPLIT;
    s_map[s_mapCount].native = (const void*)&g_SysWork;
    s_map[s_mapCount].name   = "SysWork.lo";
    s_mapCount++;

    /* SysWork above it: same fields, shifted +4 by the pointer widening. */
    s_map[s_mapCount].start  = RA_SYSWORK_BASE + RA_SYSWORK_SPLIT + 4u;
    s_map[s_mapCount].size   = RA_SYSWORK_SIZE - (RA_SYSWORK_SPLIT + 4u);
    s_map[s_mapCount].native = (const u8*)&g_SysWork + RA_SYSWORK_SPLIT + 8u;
    s_map[s_mapCount].name   = "SysWork.hi";
    s_mapCount++;

    /* GameWork, which embeds both savegames verbatim. */
    s_map[s_mapCount].start  = RA_GAMEWORK_BASE;
    s_map[s_mapCount].size   = RA_GAMEWORK_SIZE;
    s_map[s_mapCount].native = (const void*)&g_GameWork;
    s_map[s_mapCount].name   = "GameWork";
    s_mapCount++;

    /* The rest of what the live set (game 11252, 66 achievements) was observed
     * reading. Every type here is pointer-free and its retail size matches
     * configs/USA/sym.bodyprog.txt exactly — s_WorldGfxWork 0x2DBC,
     * MAP_EFFECTS_INFOS 21*44 = 0x39C — so retail offsets hold with no widening
     * correction of the kind SysWork needs. */
    RA_MAP(0x0A9004u, g_KcetLogoImg);
    RA_MAP(0x0A93CCu, MAP_EFFECTS_INFOS);
    RA_MAP(0x0A9A18u, g_MapEventLastUsedItem);
    /* Declared as an incomplete array (s_XaItemData[]), so sizeof is unavailable
     * — take the span straight from the symbol map instead. */
    s_map[s_mapCount].start  = 0x0AA894u;
    s_map[s_mapCount].size   = 0x2214u;
    s_map[s_mapCount].native = (const void*)&g_XaItemData[0];
    s_map[s_mapCount].name   = "g_XaItemData";
    s_mapCount++;
    RA_MAP(0x0AE184u, g_Inventory_EquippedItem);
    RA_MAP(0x0BCE18u, g_WorldGfxWork);
    RA_MAP(0x0C3BB8u, g_Item_MapLoadableItems);
    RA_MAP(0x0C4478u, g_ActiveCollisionTriggers);
    RA_MAP(0x0C489Cu, g_Demo_IsLoadingChunks);
}

/* Unmapped-read reporting: the official set's exact address list isn't known
 * ahead of time, so log each distinct miss once. A real session's log is then
 * the precise worklist of regions still to map. */
#define RA_MISS_MAX 64
static uint32_t s_missAddr[RA_MISS_MAX];
static int      s_missCount;
static int      s_missOverflow;

/* First few distinct pages the set touches, hit or miss: this is what reveals
 * which build's address space the achievement conditions were authored in. */
#define RA_SAMPLE_MAX 8
static uint32_t s_samplePage[RA_SAMPLE_MAX];
static int      s_sampleCount;

static void Pc_RaNoteRead(uint32_t address, int hit)
{
    int i;
    const uint32_t page = address & ~0xFFu;
    for (i = 0; i < s_sampleCount; i++)
    {
        if (s_samplePage[i] == page)
            break;
    }
    if (i == s_sampleCount && s_sampleCount < RA_SAMPLE_MAX)
    {
        s_samplePage[s_sampleCount++] = page;
        SH_DBG("[RA] set reads 0x80%06X (%s)", address, hit ? "mapped" : "UNMAPPED");
    }
}

static void Pc_RaNoteMiss(uint32_t address)
{
    int i;
    for (i = 0; i < s_missCount; i++)
    {
        if ((s_missAddr[i] & ~0xFFu) == (address & ~0xFFu))
            return; /* same 256-byte page already reported */
    }
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
    SH_DBG("[RA] achievement read of unmapped PSX address 0x80%06X - "
            "needs a translation entry", address);
}

/*
 * The set identifies which regional build it is running on by reading the PSX
 * boot executable's serial out of RAM -- each ALT group is one region, gated on
 * a string compare ("SLUS..." = USA, "SLES..." = PAL). A native recompile has no
 * PSX executable anywhere in memory, so every one of those gates failed and no
 * ALT group could activate: that, not address coverage, is why nothing unlocked.
 *
 * We answer with the USA serial regardless of which disc is mounted, because the
 * port has ONE struct layout and our address map is the USA one -- so the USA
 * group is the group whose addresses we actually serve correctly.
 *
 * Bytes are placed to satisfy the observed conditions exactly:
 * The probes are 32-bit BIG-ENDIAN reads, which is what pins the exact bytes:
 *   BE u32 @0x24C10 == 1397511507 = 0x534C5553 = "SLUS"
 *   BE u32 @0x24C15 ==  808466224 = 0x30303730 = "0070"
 *   u8     @0x24C19 ==         55 = '7'
 * i.e. the serial is stored WITHOUT the dot: "SLUS_00707".
 */
#define RA_BOOTID_BASE 0x024C10u
static const unsigned char s_bootId[] = {
    'S','L','U','S','_','0','0','7','0','7',' ',' ',' ',' ',' ',' '
};

static uint32_t Pc_RaReadMemory(uint32_t address, uint8_t* buffer,
                                uint32_t num_bytes, rc_client_t* client)
{
    int i;
    (void)client;

    if (num_bytes == 0)
        return 0;

    if (address >= RA_BOOTID_BASE && address < RA_BOOTID_BASE + sizeof(s_bootId))
    {
        const uint32_t avail = (uint32_t)(RA_BOOTID_BASE + sizeof(s_bootId) - address);
        const uint32_t n     = (num_bytes < avail) ? num_bytes : avail;
        if (buffer)
            memcpy(buffer, s_bootId + (address - RA_BOOTID_BASE), n);
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
                memcpy(buffer, (const u8*)r->native + (address - r->start), n);
            Pc_RaNoteRead(address, 1);
            return n;
        }
    }

    Pc_RaNoteRead(address, 0);
    Pc_RaNoteMiss(address);

    /* Anything inside the PSX's 2 MB of RAM must report as READABLE even when we
     * have no native global behind it. rcheevos treats a 0 return as "invalid
     * address", and rc_client_invalidate_memref_achievements then DISABLES every
     * achievement whose trigger touches that address -- one unmapped byte took
     * the whole 66-achievement set out (all reported state 3 / UNSUPPORTED).
     * Reading zeros is merely wrong for that operand; refusing the read is fatal
     * for the achievement. */
    if (address < 0x200000u)
    {
        if (buffer)
            memset(buffer, 0, num_bytes);
        return num_bytes;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* HTTP transport (worker thread; completions dispatched on the main thread)   */
/* ------------------------------------------------------------------------- */

typedef struct s_RaJob
{
    struct s_RaJob*             next;
    char*                       url;
    char*                       post;
    char*                       content_type;
    rc_client_server_callback_t callback;
    void*                       callback_data;
    char*                       body;       /* filled by the worker */
    size_t                      body_len;
    int                         status;
    /* Non-empty = this is a badge-image fetch of ours, NOT an rcheevos server
     * call, so the completion goes to the toast instead of rc_client. */
    char                        badge[16];
} s_RaJob;

static SDL_Thread*  s_httpThread;
static SDL_mutex*   s_queueLock;
static SDL_sem*     s_queueSem;
static s_RaJob*     s_pending;      /* worker inbox  */
static s_RaJob*     s_completed;    /* main-thread inbox */
static volatile int s_httpQuit;

static char* Pc_RaStrdup(const char* s)
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

static void Pc_RaJobFree(s_RaJob* job)
{
    if (!job)
        return;
    free(job->url);
    free(job->post);
    free(job->content_type);
    free(job->body);
    free(job);
}

static void Pc_RaQueuePush(s_RaJob** list, s_RaJob* job)
{
    s_RaJob* tail;
    job->next = NULL;
    if (!*list)
    {
        *list = job;
        return;
    }
    for (tail = *list; tail->next; tail = tail->next)
        ;
    tail->next = job;
}

static s_RaJob* Pc_RaQueuePop(s_RaJob** list)
{
    s_RaJob* job = *list;
    if (job)
        *list = job->next;
    return job;
}

/* One blocking request on the worker thread; the platform stack lives in
 * pc_ra_http.c because <windows.h> cannot be included alongside the PSX
 * headers. */
static void Pc_RaHttpPerform(s_RaJob* job)
{
    job->status = Pc_RaHttpRequest(job->url, job->post, &job->body, &job->body_len);
}

static int SDLCALL Pc_RaHttpThread(void* unused)
{
    (void)unused;
    for (;;)
    {
        s_RaJob* job;

        SDL_SemWait(s_queueSem);
        if (s_httpQuit)
            break;

        SDL_LockMutex(s_queueLock);
        job = Pc_RaQueuePop(&s_pending);
        SDL_UnlockMutex(s_queueLock);
        if (!job)
            continue;

        Pc_RaHttpPerform(job);

        SDL_LockMutex(s_queueLock);
        Pc_RaQueuePush(&s_completed, job);
        SDL_UnlockMutex(s_queueLock);
    }
    return 0;
}

/* rc_client asks us to talk to the server. Hand it to the worker and return
 * immediately -- the response is dispatched from Pc_Ra_Update. */
static void Pc_RaServerCall(const rc_api_request_t* request,
                            rc_client_server_callback_t callback,
                            void* callback_data, rc_client_t* client)
{
    s_RaJob* job;
    (void)client;

    job = (s_RaJob*)calloc(1, sizeof(*job));
    if (!job)
        return;

    job->url           = Pc_RaStrdup(request->url);
    job->post          = Pc_RaStrdup(request->post_data);
    job->content_type  = Pc_RaStrdup(request->content_type);
    job->callback      = callback;
    job->callback_data = callback_data;

    SDL_LockMutex(s_queueLock);
    Pc_RaQueuePush(&s_pending, job);
    SDL_UnlockMutex(s_queueLock);
    SDL_SemPost(s_queueSem);
}

/* ------------------------------------------------------------------------- */
/* Client lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static rc_client_t* s_client;
static int          s_active;       /* set is loaded and evaluating */
static int          s_enabled;      /* compiled in, configured, credentialed */
static int          s_spectator;    /* evaluate + log, never post (unverified region) */
static char         s_status[64];

static void Pc_RaToast(const char* line)
{
    DbgOverlay_ToastLine(line);
    SH_DBG("%s", line);
}

static void Pc_RaRefreshStatus(void)
{
    rc_client_user_game_summary_t summary;
    if (!s_client || !s_active)
    {
        s_status[0] = '\0';
        return;
    }
    rc_client_get_user_game_summary(s_client, &summary);
    snprintf(s_status, sizeof(s_status), "%u/%u (%u pts)",
             summary.num_unlocked_achievements, summary.num_core_achievements,
             summary.points_unlocked);
}

static void RC_CCONV Pc_RaEventHandler(const rc_client_event_t* event, rc_client_t* client)
{
    char line[96];
    (void)client;

    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        snprintf(line, sizeof(line), "Achievement: %s (%u)",
                 event->achievement->title, event->achievement->points);
        SH_DBG("%s", line);
        if (Pc_RaIsPseudoAchievement(event->achievement->title,
                                     event->achievement->points))
        {
            /* Still logged above -- it is the clearest signal that the mounted
             * disc has no supported set -- but never shown as an unlock. */
            Pc_RaRefreshStatus();
            break;
        }
        Pc_RaToast_Show(event->achievement->title,
                        event->achievement->description,
                        event->achievement->badge_name,
                        event->achievement->points);
        Pc_RaFetchBadge(event->achievement->badge_name);
        Pc_RaRefreshStatus();
        break;

    case RC_CLIENT_EVENT_GAME_COMPLETED:
        Pc_RaToast("All achievements complete!");
        break;

    case RC_CLIENT_EVENT_SERVER_ERROR:
        SH_DBG("[RA] server error: %s",
                event->server_error ? event->server_error->error_message : "unknown");
        break;

    case RC_CLIENT_EVENT_DISCONNECTED:
        Pc_RaToast("RetroAchievements offline - unlocks queued");
        break;

    case RC_CLIENT_EVENT_RECONNECTED:
        Pc_RaToast("RetroAchievements reconnected");
        break;

    default:
        break;
    }
}

static void RC_CCONV Pc_RaLoadGameCallback(int result, const char* error_message,
                                           rc_client_t* client, void* userdata)
{
    (void)client; (void)userdata;

    if (result != RC_OK)
    {
        /* NO_GAME_LOADED = this disc has no set (or an unrecognized dump);
         * that is a normal outcome, not a failure worth alarming about. */
        SH_DBG("[RA] achievement set unavailable: %s",
                error_message ? error_message : "unknown");
        return;
    }

    s_active = 1;
    Pc_RaRefreshStatus();
    {
        const rc_client_game_t* game = rc_client_get_game_info(client);
        if (game)
            SH_DBG("[RA] matched game %u \"%s\" (hash identifies the mounted disc)",
                   game->id, game->title ? game->title : "?");
    }
    SH_DBG("[RA] achievements active (softcore) - %s", s_status);
    {
        char line[96];
        snprintf(line, sizeof(line), "RetroAchievements: %s", s_status);
        Pc_RaToast(line);
    }
}

static void RC_CCONV Pc_RaLoginCallback(int result, const char* error_message,
                                        rc_client_t* client, void* userdata)
{
    const char* disc;
    char        hash[33];
    (void)userdata;

    if (result != RC_OK)
    {
        SH_DBG("[RA] login failed: %s. Re-authenticate in the launcher.",
                error_message ? error_message : "unknown");
        return;
    }

    SH_DBG("[RA] logged in as %s", g_PcConfig.raUsername);

    disc = PcPort_GetGameDiscPath();
    if (!disc || !disc[0])
    {
        SH_DBG("[RA] no disc image resolved - cannot identify the game");
        return;
    }

    /* By default the player's own dump is what's running, so this hash is the
     * genuine article and nothing is misreported. ra_hash_override changes
     * that deliberately -- see Pc_RaResolveHashOverride. */
    if (!rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, disc))
    {
        SH_DBG("[RA] could not hash disc image '%s'", disc);
        return;
    }

    SH_DBG("[RA] disc hash %s", hash);
    {
        const char* forced = Pc_RaResolveHashOverride(hash);
        if (forced != NULL)
        {
            SH_DBG("[RA] ra_hash_override active: reporting %s instead of the "
                   "mounted disc's own hash", forced);
            snprintf(hash, sizeof(hash), "%s", forced);
        }
    }
    rc_client_begin_load_game(client, hash, Pc_RaLoadGameCallback, NULL);
}

/* Queue the achievement's badge art. The toast plays regardless; the image
 * simply appears once it lands (usually within the rise animation). */
static void Pc_RaFetchBadge(const char* badgeName)
{
    s_RaJob* job;
    char     url[160];

    if (!badgeName || !badgeName[0] || !s_queueLock || !s_queueSem)
        return;

    job = (s_RaJob*)calloc(1, sizeof(*job));
    if (!job)
        return;

    snprintf(url, sizeof(url), "https://media.retroachievements.org/Badge/%s.png", badgeName);
    job->url = Pc_RaStrdup(url);
    strncpy(job->badge, badgeName, sizeof(job->badge) - 1);

    SDL_LockMutex(s_queueLock);
    Pc_RaQueuePush(&s_pending, job);
    SDL_UnlockMutex(s_queueLock);
    SDL_SemPost(s_queueSem);
}

/* Public wrapper for the browser, which requests badge art per visible row.
 * De-duplicated here rather than at the call site: the browser re-asks for the
 * rows under the viewport every frame it scrolls, and one HTTP job per frame
 * per row would flood the worker queue. */
void Pc_Ra_RequestBadge(const char* badgeName)
{
    static char s_asked[256][16];
    static int  s_askedCount;
    int         i;

    if (!badgeName || !badgeName[0])
        return;
    for (i = 0; i < s_askedCount; i++)
    {
        if (strcmp(s_asked[i], badgeName) == 0)
            return;
    }
    if (s_askedCount < (int)(sizeof(s_asked) / sizeof(s_asked[0])))
    {
        strncpy(s_asked[s_askedCount], badgeName, sizeof(s_asked[0]) - 1);
        s_askedCount++;
    }
    Pc_RaFetchBadge(badgeName);
}

/* Flatten the loaded set for the main-menu browser. rcheevos owns the strings
 * and frees them with the list, so everything is copied out. */
int Pc_Ra_SnapshotAchievements(PcRaAch* out, int max)
{
    rc_client_achievement_list_t* list;
    uint32_t b, a;
    int      n = 0;

    if (!out || max <= 0 || !s_client || !s_active)
        return 0;

    list = rc_client_create_achievement_list(s_client,
               RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
               RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
        return 0;

    for (b = 0; b < list->num_buckets && n < max; b++)
    {
        for (a = 0; a < list->buckets[b].num_achievements && n < max; a++)
        {
            const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
            PcRaAch* dst;

            if (!ach)
                continue;
            /* Keep RA's informational pseudo-achievements out of the list and
             * out of the totals; they arrive in the CORE category like any
             * other entry. Same predicate the unlock toast uses, so the two
             * can never disagree about what counts as a real achievement. */
            if (Pc_RaIsPseudoAchievement(ach->title, ach->points))
                continue;
            dst = &out[n++];
            memset(dst, 0, sizeof(*dst));
            if (ach->title)
                strncpy(dst->title, ach->title, sizeof(dst->title) - 1);
            if (ach->description)
                strncpy(dst->desc, ach->description, sizeof(dst->desc) - 1);
            strncpy(dst->badge, ach->badge_name, sizeof(dst->badge) - 1);
            dst->points = ach->points;
            /* `state` is the live runtime verdict; `unlocked` is the account
             * bitfield and stays set for a hardcore-only unlock we did not
             * earn in this session. Either one means earned, and this build is
             * softcore, so accept both. */
            dst->unlocked = (ach->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) ||
                            (ach->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE);
            dst->unlockTime = (long long)ach->unlock_time;
        }
    }
    rc_client_destroy_achievement_list(list);
    return n;
}

/* Console preview: show a REAL achievement from the loaded set (title,
 * description, points and its actual badge art) so the popup can be judged
 * exactly as players will see it. 0 = no set loaded, caller shows canned text. */
int Pc_Ra_PreviewFirst(void)
{
    rc_client_achievement_list_t* list;
    const rc_client_achievement_t* ach = NULL;
    uint32_t b;

    if (!s_client || !s_active)
        return 0;

    list = rc_client_create_achievement_list(s_client,
               RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
               RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
        return 0;

    for (b = 0; b < list->num_buckets && !ach; b++)
    {
        if (list->buckets[b].num_achievements > 0)
            ach = list->buckets[b].achievements[0];
    }
    if (ach)
    {
        Pc_RaToast_Show(ach->title, ach->description, ach->badge_name, ach->points);
        Pc_RaFetchBadge(ach->badge_name);
    }
    rc_client_destroy_achievement_list(list);
    return ach ? 1 : 0;
}

/* RAWHY: dump what an achievement's conditions actually read, next to the value
 * we hand back. Turns "why didn't it unlock" into a direct comparison against
 * the condition published on the achievement's page. */

/* savegame.mapIdx, the field almost every SH1 achievement gates on:
 * g_GameWork (0x0BC728) + savegame (0x30C) + mapIdx (0xA4). */
#define RA_MAPIDX_ADDR (RA_GAMEWORK_BASE + 0x30Cu + 0xA4u)

/* Assemble the bytes the way THIS operand reads them.
 *
 * The dump used to print a 4-byte little-endian assembly for every operand
 * regardless of memsize, which is actively misleading twice over: a big-endian
 * probe looked like a mismatch when it matched (the boot-serial region gate
 * read "SULS"/1398099027 instead of "SLUS"/1397511507), and an 8-bit field
 * looked like a wild number when it was correct (mapIdx 37 printed as 293,
 * because the read ran on into mapRoomIdx). Both cost real debugging time. */
static uint32_t Pc_RaDecodeOperand(const uint8_t* b, uint8_t size, const char** outKind)
{
    const char* kind = "u32";
    uint32_t    value;

    switch (size)
    {
    case RC_MEMSIZE_8_BITS:     kind = "u8";    value = b[0]; break;
    case RC_MEMSIZE_16_BITS:    kind = "u16";   value = (uint32_t)(b[0] | (b[1] << 8)); break;
    case RC_MEMSIZE_24_BITS:    kind = "u24";   value = (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16)); break;
    case RC_MEMSIZE_16_BITS_BE: kind = "u16be"; value = (uint32_t)((b[0] << 8) | b[1]); break;
    case RC_MEMSIZE_24_BITS_BE: kind = "u24be"; value = (uint32_t)((b[0] << 16) | (b[1] << 8) | b[2]); break;
    case RC_MEMSIZE_32_BITS_BE: kind = "u32be";
        value = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; break;
    case RC_MEMSIZE_LOW:        kind = "lo4";   value = b[0] & 0x0Fu; break;
    case RC_MEMSIZE_HIGH:       kind = "hi4";   value = (b[0] >> 4) & 0x0Fu; break;
    case RC_MEMSIZE_BITCOUNT:   kind = "bits";
    {
        int      n = 0;
        unsigned v = b[0];
        while (v) { n += (int)(v & 1u); v >>= 1; }
        value = (uint32_t)n;
        break;
    }
    default:
        if (size >= RC_MEMSIZE_BIT_0 && size <= RC_MEMSIZE_BIT_7)
        {
            static const char* const bitKinds[8] = {
                "bit0", "bit1", "bit2", "bit3", "bit4", "bit5", "bit6", "bit7"
            };
            const int bit = (int)size - (int)RC_MEMSIZE_BIT_0;
            kind  = bitKinds[bit];
            value = (uint32_t)((b[0] >> bit) & 1u);
        }
        else
        {
            value = (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
        }
        break;
    }
    if (outKind)
        *outKind = kind;
    return value;
}

/* " (MAP5_S00 \"Sewers - lower and upper levels\")" for a map index, else "".
 * Nearly every SH1 achievement compares mapIdx against a bare number, and
 * decoding those by hand against the enum is where this investigation kept
 * going wrong. */
static const char* Pc_RaMapNote(uint32_t mapIdx, char* buf, size_t cap)
{
    const char* name;
    const char* desc;

    buf[0] = '\0';
    if (mapIdx > (uint32_t)MapIdx_MAPX_S00)
        return buf;
    name = MapRegistry_GetName((e_MapIdx)mapIdx);
    desc = MapRegistry_GetDescription((e_MapIdx)mapIdx);
    if (name == NULL && desc == NULL)
        return buf;
    snprintf(buf, cap, " (%s%s%s%s)",
             name ? name : "",
             desc ? " \"" : "", desc ? desc : "", desc ? "\"" : "");
    return buf;
}

static const char* Pc_RaOperandDesc(const rc_operand_t* op, int mapIdxContext,
                                    char* buf, size_t cap)
{
    char note[96];

    if (op->type == RC_OPERAND_ADDRESS || op->type == RC_OPERAND_DELTA ||
        op->type == RC_OPERAND_PRIOR)
    {
        uint32_t    addr    = op->value.memref->address;
        uint8_t     bytes[4] = { 0, 0, 0, 0 };
        uint32_t    got     = Pc_RaReadMemory(addr, bytes, 4, NULL);
        const char* kind    = "u32";
        uint32_t    value   = Pc_RaDecodeOperand(bytes, op->size, &kind);

        snprintf(buf, cap, "%s0x80%06X%s.%s=%u%s",
                 (op->type == RC_OPERAND_DELTA) ? "delta " :
                 (op->type == RC_OPERAND_PRIOR) ? "prior " : "",
                 addr, got ? "" : "(UNMAPPED)", kind, (unsigned)value,
                 (addr == RA_MAPIDX_ADDR) ? Pc_RaMapNote(value, note, sizeof(note)) : "");
    }
    else
    {
        /* A bare number next to a mapIdx read is a map index; name it. */
        snprintf(buf, cap, "t%u:%u%s", (unsigned)op->type, (unsigned)op->value.num,
                 mapIdxContext ? Pc_RaMapNote(op->value.num, note, sizeof(note)) : "");
    }
    return buf;
}

int Pc_Ra_Why(const char* filter)
{
    rc_client_achievement_list_t* list;
    uint32_t b, i;
    int shown = 0;

    if (!s_client || !s_active)
        return 0;
    list = rc_client_create_achievement_list(s_client,
               RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
               RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
        return 0;

    for (b = 0; b < list->num_buckets; b++)
    {
        for (i = 0; i < list->buckets[b].num_achievements; i++)
        {
            const rc_client_achievement_t* pub = list->buckets[b].achievements[i];
            const rc_client_achievement_info_t* info;
            const rc_condset_t* set;
            int condIdx = 0;

            if (filter && filter[0])
            {
                /* Console upper-cases arguments, so match case-insensitively. */
                const char* h = pub->title;
                const char* found = NULL;
                for (; *h && !found; h++)
                {
                    const char* a = h;
                    const char* b = filter;
                    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
                    if (!*b) found = h;
                }
                if (!found)
                    continue;
            }
            if (shown >= 3)
                break;
            shown++;

            info = (const rc_client_achievement_info_t*)pub;
            SH_DBG("[RAWHY] \"%s\" (id %u, state %u)", pub->title, pub->id, pub->state);
            if (!info->trigger)
            {
                SH_DBG("[RAWHY]   no parsed trigger");
                continue;
            }
            /* rcheevos splits a trigger into the CORE group (requirement) and a
             * chain of ALT groups (alternative). Dumping only core showed a
             * single meaningless condition; the real logic usually lives in the
             * alts, so walk both. */
            {
                int grp;
                for (grp = 0; grp < 2; grp++)
                {
                    const rc_condset_t* head = grp ? info->trigger->alternative
                                                   : info->trigger->requirement;
                    int setIdx = 0;
                    for (set = head; set; set = set->next, setIdx++)
                    {
                        const rc_condition_t* c;
                        SH_DBG("[RAWHY]   -- %s group %d --", grp ? "ALT" : "CORE", setIdx);
                        for (c = set->conditions; c; c = c->next)
                        {
                            char a[192], bb[192];
                            /* Either side may be the mapIdx read; the other side
                             * is then a map index worth naming. */
                            const int op1IsMap =
                                (c->operand1.type == RC_OPERAND_ADDRESS ||
                                 c->operand1.type == RC_OPERAND_DELTA ||
                                 c->operand1.type == RC_OPERAND_PRIOR) &&
                                c->operand1.value.memref->address == RA_MAPIDX_ADDR;
                            const int op2IsMap =
                                (c->operand2.type == RC_OPERAND_ADDRESS ||
                                 c->operand2.type == RC_OPERAND_DELTA ||
                                 c->operand2.type == RC_OPERAND_PRIOR) &&
                                c->operand2.value.memref->address == RA_MAPIDX_ADDR;

                            SH_DBG("[RAWHY]   c%-2d type%u %s  op%u  %s   hits %u/%u",
                                   condIdx++, (unsigned)c->type,
                                   Pc_RaOperandDesc(&c->operand1, op2IsMap, a, sizeof(a)),
                                   (unsigned)c->oper,
                                   Pc_RaOperandDesc(&c->operand2, op1IsMap, bb, sizeof(bb)),
                                   (unsigned)c->current_hits, (unsigned)c->required_hits);
                            if (condIdx > 40) break;
                        }
                        if (condIdx > 40) break;
                    }
                }
            }
        }
    }
    rc_client_destroy_achievement_list(list);
    return shown;
}

void Pc_Ra_Init(void)
{
    SH_DBG("[RA] init: enabled=%d user='%s' token=%s region=%d",
                g_PcConfig.retroAchievements,
                g_PcConfig.raUsername,
                g_PcConfig.raToken[0] ? "present" : "MISSING",
                (int)g_GameRegion);

    if (!g_PcConfig.retroAchievements)
    {
        SH_DBG("[RA] disabled in config (retroachievements = 0) - off");
        return;
    }

    /* Region handling — resolved empirically 2026-07-28. A PAL session proved
     * RA matches game 11252 and serves the SAME official 66-achievement set,
     * and that the set reads USA addresses: its reads landed on savegame +0xA4
     * (mapIdx), +0x24A (clearGameCount) and +0x16E (eventFlags) through this
     * table. Since the port compiles one struct layout for every disc, the map
     * is correct for PAL and NTSC-J as well, so no region is held back now.
     * `ra_spectator = 1` still forces evaluate-and-toast-without-submitting for
     * testing. */
    s_spectator = g_PcConfig.raSpectator;

    if (!g_PcConfig.raUsername[0] || !g_PcConfig.raToken[0])
    {
        SH_DBG("[RA] enabled but not signed in - use the launcher's "
               "RetroAchievements login");
        return;
    }

    Pc_RaBuildMap();

    s_queueLock = SDL_CreateMutex();
    s_queueSem  = SDL_CreateSemaphore(0);
    if (!s_queueLock || !s_queueSem)
    {
        SH_DBG("[RA] could not create worker primitives - disabled");
        return;
    }

    s_httpThread = SDL_CreateThread(Pc_RaHttpThread, "SH_RA_HTTP", NULL);
    if (!s_httpThread)
    {
        SH_DBG("[RA] could not start HTTP worker - disabled");
        return;
    }

    s_client = rc_client_create(Pc_RaReadMemory, Pc_RaServerCall);
    if (!s_client)
    {
        SH_DBG("[RA] client creation failed - disabled");
        return;
    }

    /* Softcore, permanently: see the file header. */
    rc_client_set_hardcore_enabled(s_client, 0);
    rc_client_set_event_handler(s_client, Pc_RaEventHandler);
    if (s_spectator)
    {
        rc_client_set_spectator_mode_enabled(s_client, 1);
        SH_DBG("[RA] non-USA disc: SPECTATOR mode - achievements evaluate and log "
               "but nothing is submitted. Check the addresses logged below, then set "
               "ra_unverified_region = 1 to submit for real.");
    }

    s_enabled = 1;
    rc_client_begin_login_with_token(s_client, g_PcConfig.raUsername,
                                     g_PcConfig.raToken, Pc_RaLoginCallback, NULL);
}

void Pc_Ra_Update(void)
{
    s_RaJob* done;

    if (!s_enabled || !s_client)
        return;

    /* Dispatch finished HTTP on the main thread so rc_client stays
     * single-threaded. */
    for (;;)
    {
        SDL_LockMutex(s_queueLock);
        done = Pc_RaQueuePop(&s_completed);
        SDL_UnlockMutex(s_queueLock);
        if (!done)
            break;

        if (done->badge[0])
        {
            /* Badge PNG: hand the bytes to whoever asked. Offered to BOTH
             * consumers unconditionally — each ignores a name it has no row or
             * toast waiting on — because the browser and an unlock popup can
             * legitimately want the same badge at the same time. */
            if (done->status == 200 && done->body && done->body_len)
            {
                Pc_RaToast_ProvideBadge(done->badge,
                                        (const unsigned char*)done->body, done->body_len);
                Pc_RaBrowser_ProvideBadge(done->badge,
                                          (const unsigned char*)done->body, done->body_len);
            }
            else
                SH_DBG("[RA] badge %s fetch failed (http %d)", done->badge, done->status);
        }
        else if (done->callback)
        {
            rc_api_server_response_t response;
            memset(&response, 0, sizeof(response));
            response.body             = done->body ? done->body : "";
            response.body_length      = done->body_len;
            response.http_status_code = done->status;
            done->callback(&response, done->callback_data);
        }
        Pc_RaJobFree(done);
    }

    /* Evaluate only during live gameplay: menus, FMV and load fades don't
     * hold coherent world state, and the same gate already guards the
     * flashlight shadow pass. */
    if (s_active &&
        g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState == SysState_Gameplay &&
        /* An attract demo drives real gameplay state from recorded input and
         * would otherwise unlock achievements nobody played for. */
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
    if (s_queueSem)
    {
        s_httpQuit = 1;
        SDL_SemPost(s_queueSem);
    }
    if (s_httpThread)
    {
        SDL_WaitThread(s_httpThread, NULL);
        s_httpThread = NULL;
    }
    if (s_client)
    {
        rc_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_queueSem)
    {
        SDL_DestroySemaphore(s_queueSem);
        s_queueSem = NULL;
    }
    if (s_queueLock)
    {
        SDL_DestroyMutex(s_queueLock);
        s_queueLock = NULL;
    }
    while (s_pending)
        Pc_RaJobFree(Pc_RaQueuePop(&s_pending));
    while (s_completed)
        Pc_RaJobFree(Pc_RaQueuePop(&s_completed));
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

#else /* !SH_RETROACHIEVEMENTS */

void        Pc_Ra_Init(void)     {}
void        Pc_Ra_Update(void)   {}
void        Pc_Ra_Shutdown(void) {}
int         Pc_Ra_IsActive(void) { return 0; }
const char* Pc_Ra_StatusLine(void) { return ""; }
void        Pc_Ra_RequestBadge(const char* badgeName) { (void)badgeName; }
int         Pc_Ra_SnapshotAchievements(PcRaAch* out, int max) { (void)out; (void)max; return 0; }

#endif /* SH_RETROACHIEVEMENTS */
