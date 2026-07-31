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

#include "game.h"
#include "bodyprog/savegame.h"
/* Globals the live achievement set was observed reading (see the table below). */
#include "bodyprog/bodyprog.h"                      /* g_KcetLogoImg, MAP_EFFECTS_INFOS, g_WorldGfxWork */
#include "bodyprog/demo.h"                          /* g_Demo_IsLoadingChunks */
#include "bodyprog/item_screens.h"                  /* g_Inventory_EquippedItem, g_Item_MapLoadableItems */
#include "bodyprog/collision/collision.h"           /* g_ActiveCollisionTriggers */
#include "bodyprog/sound/sound_system.h"            /* g_XaItemData, Sd_PlaySfx */
#include "bodyprog/sound/sfx_id_enum.h"             /* Sfx_MenuConfirm (trophy chime) */
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_MapEventLastUsedItem */

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

static s_RaRegion s_map[16];
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

static void Ra_BuildMap(void)
{
    s_mapCount = 0;

    /* The two big work structs. On 32-bit Xbox field_2510 (game.h, SH_PC_PORT)
     * is a 4-byte pointer == the retail s32, so g_SysWork keeps its retail layout
     * and maps whole with no +4 correction. */
    RA_MAP(0x0B9FC0u, g_SysWork);
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
 * "SLUS_00707" (the probes are 32-bit big-endian: 0x534C5553 "SLUS" @0x24C10,
 * 0x30303730 "0070" @0x24C15, '7' @0x24C19).
 */
#define RA_BOOTID_BASE 0x024C10u
static const unsigned char s_bootId[] = {
    'S','L','U','S','_','0','0','7','0','7',' ',' ',' ',' ',' ',' '
};

static uint32_t RC_CCONV Ra_ReadMemory(uint32_t address, uint8_t* buffer,
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
static int          s_active;    /* set loaded and evaluating */
static int          s_enabled;   /* configured, credentialed, client created */
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

static void RC_CCONV Ra_EventHandler(const rc_client_event_t* event, rc_client_t* client)
{
    char line[128];
    (void)client;

    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        /* Trophy chime: the game's own positive "confirm" cue (Sfx_MenuConfirm,
         * in the persistent BASE.VAB). This handler only runs from
         * rc_client_do_frame in settled gameplay (main thread, SPU pumping), so a
         * direct Sd_PlaySfx is safe and needs ZERO new asset. Already-unlocked
         * achievements loaded at boot do NOT fire this event, so no spurious ding. */
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);

        /* Two distinctly-formatted lines (banner + title) so an unlock never reads
         * like the plain status lines that also flow through Ra_Toast. */
        Ra_Toast("* * ACHIEVEMENT UNLOCKED * *");
        snprintf(line, sizeof(line), "%s  (%u pts)",
                 event->achievement->title, (unsigned)event->achievement->points);
        Ra_Toast(line);
        Ra_RefreshStatus();

        /* Badge image: fetch + decode + upload the unlock icon and arm the
         * on-screen quad (ra_badge_xbox.c). BLOCKING one-shot on the main thread;
         * a brief hitch on unlock is fine (rare, settled gameplay). Any failure is
         * a silent skip - the chime + toast above already fired. */
        { extern int RaBadge_Fetch(const char*); RaBadge_Fetch(event->achievement->badge_name); }
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
     * coherent world state. rc_client_idle still services pings/retries. */
    if (s_active &&
        g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState   == SysState_Gameplay)
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

/* Menu action (Xbox Options "RetroAchievements" row): toast the current login
 * state so the user can check it from the menu without the D: log. Routes through
 * Ra_Toast -> g_ShOverlayToastLine (wired to DbgOverlay_XboxToast). */
void Pc_Ra_StatusToast(void)
{
    const rc_client_user_t* user;
    char                    line[96];

    if (!g_PcConfig.retroAchievements)
    {
        Ra_Toast("RA: off (set retroachievements=1)");
        return;
    }

    user = s_client ? rc_client_get_user_info(s_client) : NULL;
    if (user)
    {
        const char* nm = (user->display_name && user->display_name[0]) ? user->display_name
                       : (user->username ? user->username : "?");
        snprintf(line, sizeof(line), "RA: signed in as %s (%u pts)",
                 nm, (unsigned)user->score_softcore);
        Ra_Toast(line);
    }
    else
    {
        Ra_Toast("RA: not signed in");
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
