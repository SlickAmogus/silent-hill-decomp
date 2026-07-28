/*
 * Optional RetroAchievements support for the Linux PC port.
 *
 * This module is intentionally runtime-loaded so the normal build has no new
 * link-time dependencies. It becomes active only when all three variables are
 * present:
 *
 *   SH_RA_USERNAME=<RetroAchievements username>
 *   SH_RA_TOKEN=<web API token>
 *   SH_RA_DISC=/absolute/path/to/Silent Hill.bin
 *
 * The token is never written to config.cfg or the log. This first integration
 * runs softcore-only. The PC port exposes quick save/load, debug controls,
 * alternate cameras, randomizer and other enhancements which have not been
 * audited for RetroAchievements hardcore compliance.
 */

#if defined(__linux__)

#include <SDL.h>
#include <dlfcn.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

extern "C" {
#include "psx_memory.h"
#include "sh_log.h"
}

/* Minimal public rcheevos ABI declarations. The implementation is loaded from
 * librcheevos.so at runtime, so the repository does not vendor the library. */
typedef struct rc_client_t rc_client_t;
typedef struct rc_client_async_handle_t rc_client_async_handle_t;

typedef struct rc_api_request_t {
    const char* url;
    const char* post_data;
    const char* content_type;
    unsigned char opaque_buffer[64];
} rc_api_request_t;

typedef struct rc_api_server_response_t {
    const char* body;
    size_t body_length;
    int http_status_code;
} rc_api_server_response_t;

typedef struct rc_client_achievement_t {
    const char* title;
    const char* description;
    char badge_name[8];
    char measured_progress[24];
    float measured_percent;
    uint32_t id;
    uint32_t points;
} rc_client_achievement_t;

typedef struct rc_client_event_t {
    uint32_t type;
    rc_client_achievement_t* achievement;
    void* leaderboard;
    void* leaderboard_tracker;
    void* leaderboard_scoreboard;
    void* server_error;
    void* subset;
} rc_client_event_t;

typedef uint32_t (*ra_read_memory_t)(uint32_t, uint8_t*, uint32_t, rc_client_t*);
typedef void (*ra_server_callback_t)(const rc_api_server_response_t*, void*);
typedef void (*ra_server_call_t)(const rc_api_request_t*, ra_server_callback_t, void*, rc_client_t*);
typedef void (*ra_callback_t)(int, const char*, rc_client_t*, void*);
typedef void (*ra_event_handler_t)(const rc_client_event_t*, rc_client_t*);
typedef void (*ra_log_handler_t)(const char*, const rc_client_t*);

struct RaApi {
    void* handle;
    rc_client_t* (*create)(ra_read_memory_t, ra_server_call_t);
    void (*destroy)(rc_client_t*);
    void (*set_hardcore)(rc_client_t*, int);
    void (*set_background_reads)(rc_client_t*, int);
    void (*set_event_handler)(rc_client_t*, ra_event_handler_t);
    void (*enable_logging)(rc_client_t*, int, ra_log_handler_t);
    rc_client_async_handle_t* (*login_token)(rc_client_t*, const char*, const char*, ra_callback_t, void*);
    rc_client_async_handle_t* (*identify_load)(rc_client_t*, uint32_t, const char*, const uint8_t*, size_t, ra_callback_t, void*);
    void (*do_frame)(rc_client_t*);
};

/* Minimal libcurl easy API declarations and stable option values. */
typedef void CURL;
typedef int CURLcode;
typedef size_t (*curl_write_cb)(char*, size_t, size_t, void*);

struct CurlApi {
    void* handle;
    CURL* (*easy_init)(void);
    CURLcode (*easy_setopt)(CURL*, int, ...);
    CURLcode (*easy_perform)(CURL*);
    CURLcode (*easy_getinfo)(CURL*, int, ...);
    void (*easy_cleanup)(CURL*);
};

static const int CURLOPT_WRITEDATA      = 10001;
static const int CURLOPT_URL            = 10002;
static const int CURLOPT_POSTFIELDS     = 10015;
static const int CURLOPT_USERAGENT      = 10018;
static const int CURLOPT_WRITEFUNCTION  = 20011;
static const int CURLOPT_FOLLOWLOCATION = 52;
static const int CURLOPT_TIMEOUT        = 13;
static const int CURLINFO_RESPONSE_CODE = 0x200002;

static RaApi s_ra = {};
static CurlApi s_curl = {};
static rc_client_t* s_client = NULL;
static SDL_TimerID s_timer = 0;
static int s_started = 0;
static std::string s_disc_path;

static void* LoadSymbol(void* handle, const char* name)
{
    void* symbol = dlsym(handle, name);
    if (!symbol)
        SH_LOG("[RA] missing symbol %s", name);
    return symbol;
}

static int LoadLibraries(void)
{
    const char* ra_names[] = { "librcheevos.so.0", "librcheevos.so", NULL };
    const char* curl_names[] = { "libcurl.so.4", "libcurl.so", NULL };

    for (int i = 0; ra_names[i] && !s_ra.handle; ++i)
        s_ra.handle = dlopen(ra_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_ra.handle)
        return 0;

    for (int i = 0; curl_names[i] && !s_curl.handle; ++i)
        s_curl.handle = dlopen(curl_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_curl.handle) {
        dlclose(s_ra.handle);
        s_ra.handle = NULL;
        return 0;
    }

#define RA_SYM(field, name) s_ra.field = reinterpret_cast<decltype(s_ra.field)>(LoadSymbol(s_ra.handle, name))
    RA_SYM(create, "rc_client_create");
    RA_SYM(destroy, "rc_client_destroy");
    RA_SYM(set_hardcore, "rc_client_set_hardcore_enabled");
    RA_SYM(set_background_reads, "rc_client_set_allow_background_memory_reads");
    RA_SYM(set_event_handler, "rc_client_set_event_handler");
    RA_SYM(enable_logging, "rc_client_enable_logging");
    RA_SYM(login_token, "rc_client_begin_login_with_token");
    RA_SYM(identify_load, "rc_client_begin_identify_and_load_game");
    RA_SYM(do_frame, "rc_client_do_frame");
#undef RA_SYM

#define CURL_SYM(field, name) s_curl.field = reinterpret_cast<decltype(s_curl.field)>(LoadSymbol(s_curl.handle, name))
    CURL_SYM(easy_init, "curl_easy_init");
    CURL_SYM(easy_setopt, "curl_easy_setopt");
    CURL_SYM(easy_perform, "curl_easy_perform");
    CURL_SYM(easy_getinfo, "curl_easy_getinfo");
    CURL_SYM(easy_cleanup, "curl_easy_cleanup");
#undef CURL_SYM

    return s_ra.create && s_ra.destroy && s_ra.set_hardcore &&
           s_ra.set_background_reads && s_ra.set_event_handler &&
           s_ra.enable_logging && s_ra.login_token && s_ra.identify_load &&
           s_ra.do_frame && s_curl.easy_init && s_curl.easy_setopt &&
           s_curl.easy_perform && s_curl.easy_getinfo && s_curl.easy_cleanup;
}

static size_t CurlWrite(char* data, size_t size, size_t count, void* userdata)
{
    std::string* body = static_cast<std::string*>(userdata);
    const size_t bytes = size * count;
    body->append(data, bytes);
    return bytes;
}

static void ServerCall(const rc_api_request_t* request, ra_server_callback_t callback,
                       void* callback_data, rc_client_t*)
{
    rc_api_server_response_t response = {};
    std::string body;
    CURL* curl = s_curl.easy_init();
    long status = 0;

    if (!curl) {
        response.http_status_code = -1;
        callback(&response, callback_data);
        return;
    }

    s_curl.easy_setopt(curl, CURLOPT_URL, request->url);
    s_curl.easy_setopt(curl, CURLOPT_USERAGENT, "SilentHillPC/RetroAchievements");
    s_curl.easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    s_curl.easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    s_curl.easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    s_curl.easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (request->post_data && request->post_data[0])
        s_curl.easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);

    const CURLcode result = s_curl.easy_perform(curl);
    if (result == 0)
        s_curl.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else
        status = -1;

    response.body = body.c_str();
    response.body_length = body.size();
    response.http_status_code = static_cast<int>(status);
    callback(&response, callback_data);
    s_curl.easy_cleanup(curl);
}

static uint32_t ReadMemory(uint32_t address, uint8_t* buffer, uint32_t bytes, rc_client_t*)
{
    const uint32_t ram_size = 2u * 1024u * 1024u;
    if (!buffer || address >= ram_size || bytes > ram_size - address)
        return 0;

    memcpy(buffer, g_PsxRam + address, bytes);
    return bytes;
}

static void RaLog(const char* message, const rc_client_t*)
{
    if (message)
        SH_LOG("[RA] %s", message);
}

static void RaEvent(const rc_client_event_t* event, rc_client_t*)
{
    enum { AchievementTriggered = 1, GameCompleted = 15,
           Disconnected = 17, Reconnected = 18 };

    if (!event)
        return;

    switch (event->type) {
    case AchievementTriggered:
        if (event->achievement)
            SH_LOG("[RA] Achievement unlocked: %s (%u points)",
                   event->achievement->title ? event->achievement->title : "Unknown",
                   event->achievement->points);
        break;
    case GameCompleted:
        SH_LOG("[RA] Game completed");
        break;
    case Disconnected:
        SH_LOG("[RA] Connection lost; unlocks are pending");
        break;
    case Reconnected:
        SH_LOG("[RA] Reconnected; pending unlocks synchronized");
        break;
    default:
        break;
    }
}

static void GameLoaded(int result, const char* error, rc_client_t*, void*)
{
    if (result == 0)
        SH_LOG("[RA] Silent Hill identified; achievements are active in softcore mode");
    else
        SH_LOG("[RA] Game identification failed: %s", error ? error : "unknown error");
}

static void LoggedIn(int result, const char* error, rc_client_t* client, void*)
{
    if (result != 0) {
        SH_LOG("[RA] Login failed: %s", error ? error : "unknown error");
        return;
    }

    SH_LOG("[RA] Login successful; identifying disc image");
    s_ra.identify_load(client, 12 /* PlayStation */, s_disc_path.c_str(), NULL, 0,
                       GameLoaded, NULL);
}

static Uint32 FrameTimer(Uint32 interval, void*)
{
    if (s_client)
        s_ra.do_frame(s_client);
    return interval;
}

static int StartRetroAchievements(void)
{
    const char* username = getenv("SH_RA_USERNAME");
    const char* token = getenv("SH_RA_TOKEN");
    const char* disc = getenv("SH_RA_DISC");

    if (!username || !username[0] || !token || !token[0] || !disc || !disc[0])
        return 0;

    FILE* fp = fopen(disc, "rb");
    if (!fp) {
        SH_LOG("[RA] SH_RA_DISC does not point to a readable file: %s", disc);
        return 0;
    }
    fclose(fp);

    if (!LoadLibraries()) {
        SH_LOG("[RA] librcheevos and libcurl are required; integration disabled");
        return 0;
    }

    s_disc_path = disc;
    s_client = s_ra.create(ReadMemory, ServerCall);
    if (!s_client) {
        SH_LOG("[RA] could not create rcheevos client");
        return 0;
    }

    s_ra.set_hardcore(s_client, 0);
    s_ra.set_background_reads(s_client, 1);
    s_ra.set_event_handler(s_client, RaEvent);
    s_ra.enable_logging(s_client, 2 /* warnings */, RaLog);
    s_ra.login_token(s_client, username, token, LoggedIn, NULL);

    s_timer = SDL_AddTimer(33, FrameTimer, NULL);
    if (!s_timer) {
        SH_LOG("[RA] could not start frame timer: %s", SDL_GetError());
        s_ra.destroy(s_client);
        s_client = NULL;
        return 0;
    }

    return 1;
}

/* SDL and PSX RAM are initialized later in main(). Waiting here avoids touching
 * either subsystem from a static constructor. */
static int StarterThread(void*)
{
    SDL_Delay(5000);
    if (!s_started) {
        s_started = 1;
        StartRetroAchievements();
    }
    return 0;
}

__attribute__((constructor)) static void RetroAchievementsModuleLoad(void)
{
    SDL_CreateThread(StarterThread, "SH-RetroAchievements", NULL);
}

__attribute__((destructor)) static void RetroAchievementsModuleUnload(void)
{
    if (s_timer)
        SDL_RemoveTimer(s_timer);
    s_timer = 0;

    if (s_client && s_ra.destroy)
        s_ra.destroy(s_client);
    s_client = NULL;

    if (s_curl.handle)
        dlclose(s_curl.handle);
    if (s_ra.handle)
        dlclose(s_ra.handle);
    s_curl = {};
    s_ra = {};
}

#endif /* __linux__ */
