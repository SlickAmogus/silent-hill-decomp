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

#include "pc_retroachievements.h"

#ifdef SH_RETROACHIEVEMENTS

#include <SDL.h>
#include <rc_client.h>
#include <rc_hash.h>
#include <rc_consoles.h>

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
#include "pc_ra_toast.h"

extern const char* PcPort_GetGameDiscPath(void);

/* Defined below; the unlock event handler above it kicks the badge download. */
static void Pc_RaFetchBadge(const char* badgeName);

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

static s_RaRegion s_map[24];
static int        s_mapCount;

/* Map one native global at its retail PSX offset. sizeof() is the authority on
 * length, so a translation can never read past the native object even where the
 * symbol map claims a larger span. */
#define RA_MAP(psxOffset, sym) do { \
    if (s_mapCount < (int)(sizeof(s_map) / sizeof(s_map[0]))) { \
        s_map[s_mapCount].start  = (psxOffset); \
        s_map[s_mapCount].size   = (uint32_t)sizeof(sym); \
        s_map[s_mapCount].native = (const void*)&(sym); \
        s_map[s_mapCount].name   = #sym; \
        s_mapCount++; \
    } \
} while (0)

static void Pc_RaBuildMap(void)
{
    s_mapCount = 0;

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

static uint32_t Pc_RaReadMemory(uint32_t address, uint8_t* buffer,
                                uint32_t num_bytes, rc_client_t* client)
{
    int i;
    (void)client;

    if (num_bytes == 0)
        return 0;

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

    /* The player's own dump is what's running, so this hash is the genuine
     * article -- no spoofing involved. */
    if (!rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, disc))
    {
        SH_DBG("[RA] could not hash disc image '%s'", disc);
        return;
    }

    SH_DBG("[RA] disc hash %s", hash);
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
            /* Badge PNG: hand the bytes to the toast (it decodes and uploads). */
            if (done->status == 200 && done->body && done->body_len)
                Pc_RaToast_ProvideBadge(done->badge,
                                        (const unsigned char*)done->body, done->body_len);
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
        g_SysWork.sysState == SysState_Gameplay)
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

#endif /* SH_RETROACHIEVEMENTS */
