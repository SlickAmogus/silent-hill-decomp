/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_steam.c - Steam lobbies, invites, presence and P2P, loaded at runtime.
 *
 * See sh_net_steam.h for why this is a runtime loader rather than a link
 * against the Steamworks SDK. Two things follow from that decision and shape
 * everything in this file:
 *
 * 1. THE FLAT C API. steam_api64.dll exports a C entry point for every
 *    interface method (SteamAPI_ISteamMatchmaking_CreateLobby and friends).
 *    They are declared here by hand. This is the same route every non-C++
 *    binding takes.
 *
 * 2. MANUAL CALLBACK DISPATCH. The SDK's normal callback mechanism is C++
 *    template machinery that registers itself from a constructor; there is no
 *    way to reach it from C. SteamAPI_ManualDispatch_* exists precisely for
 *    this case: pump a queue, read a callback id, cast the payload. It is what
 *    ShSteam_RunCallbacks does.
 *
 * THE COST is that a handful of Steam struct layouts are asserted here rather
 * than being taken from a header, so they are all together in the block below
 * with their offsets spelled out. Every one of them is a public, stable part of
 * the ABI that shipped games depend on, and only the FRONT of each struct is
 * ever read, which is the half that cannot move without breaking those games.
 * ShSteam_Interfaces() reports what resolved, so a mismatch in someone's DLL is
 * diagnosable rather than a silent nothing.
 *
 * This TU includes <windows.h> and NOTHING from the game, for the same reason
 * pc_ra_http.c and sh_net_platform.c do: RECT16, `byte` and the critical
 * section family collide with the PSX headers.
 */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_net_steam.h"
#include "sh_log.h"

/* ------------------------------------------------------------------ */
/* ABI                                                                 */
/* ------------------------------------------------------------------ */

typedef signed char        s8_;
typedef unsigned char      u8_;
typedef int                s32_;
typedef unsigned int       u32_;
typedef long long          s64_;
typedef unsigned long long u64_;

/* Callback ids. Each interface reserves a block of 100. */
#define CB_FRIENDS_BASE      300
#define CB_MATCHMAKING_BASE  500
#define CB_MESSAGES_BASE     1250

#define CB_GameLobbyJoinRequested (CB_FRIENDS_BASE + 33)     /* 333 */
#define CB_LobbyEnter             (CB_MATCHMAKING_BASE + 4)  /* 504 */
#define CB_LobbyDataUpdate        (CB_MATCHMAKING_BASE + 5)  /* 505 */
#define CB_LobbyChatUpdate        (CB_MATCHMAKING_BASE + 6)  /* 506 */
#define CB_LobbyCreated           (CB_MATCHMAKING_BASE + 13) /* 513 */
#define CB_MessagesSessionRequest (CB_MESSAGES_BASE + 1)     /* 1251 */
#define CB_MessagesSessionFailed  (CB_MESSAGES_BASE + 2)     /* 1252 */

#define ERESULT_OK 1

/* SteamNetworkingIdentity: { int eType; int cbSize; union { ... } } and the
 * union is 128 bytes, so the whole thing is 136. eType 16 is a SteamID, and
 * for that case cbSize is 8 and the id sits at the front of the union.
 * Hand-filled rather than going through the exported setters, which older DLLs
 * do not all have. */
#define SNI_SIZE          136
#define SNI_TYPE_STEAMID  16

typedef struct { u8_ raw[SNI_SIZE]; } ShSteamIdentity;

static void ShSteamIdentity_Set(ShSteamIdentity* id, u64_ steamId)
{
    memset(id, 0, sizeof(*id));
    *(s32_*)(id->raw + 0) = SNI_TYPE_STEAMID;
    *(s32_*)(id->raw + 4) = 8;
    memcpy(id->raw + 8, &steamId, sizeof(steamId));
}

static u64_ ShSteamIdentity_Get(const void* p)
{
    u64_ v = 0;
    memcpy(&v, (const u8_*)p + 8, sizeof(v));
    return v;
}

/* CallbackMsg_t: { int32 hSteamUser; int iCallback; uint8* pubParam;
 * int cubParam; } - 24 bytes once the pointer is aligned on x64. */
typedef struct
{
    s32_ hSteamUser;
    s32_ iCallback;
    u8_* pubParam;
    s32_ cubParam;
    s32_ pad;
} ShSteamCallbackMsg;

/* Send flags. */
#define SEND_UNRELIABLE 0
#define SEND_RELIABLE   8

/* SteamNetworkingMessage_t, front only: m_pData at 0, m_cbSize at 8, and
 * m_identityPeer starting at 16 (so the peer's SteamID is at 24). Nothing
 * past that is read, which keeps this independent of the tail of the struct. */
#define MSG_OFS_DATA     0
#define MSG_OFS_SIZE     8
#define MSG_OFS_IDENTITY 16

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

/* SteamAPI_Init has been a header MACRO since SDK 1.59, not an export: the
 * real entry point is SteamAPI_InitFlat, which fills a 1024-byte SteamErrMsg
 * and returns an ESteamAPIInitResult (0 = OK). Older DLLs export the plain
 * SteamAPI_Init returning a bool. Both are tried, newest first, because a
 * player's steam_api64.dll could be either. */
typedef int  (__cdecl *PFN_InitFlat)(char* outErrMsg);
typedef int  (__cdecl *PFN_Init)(void);
typedef void (__cdecl *PFN_Shutdown)(void);
typedef s32_ (__cdecl *PFN_GetHSteamPipe)(void);
typedef void (__cdecl *PFN_MD_Init)(void);
typedef void (__cdecl *PFN_MD_RunFrame)(s32_ pipe);
typedef int  (__cdecl *PFN_MD_GetNextCallback)(s32_ pipe, ShSteamCallbackMsg* out);
typedef void (__cdecl *PFN_MD_FreeLastCallback)(s32_ pipe);
typedef int  (__cdecl *PFN_MD_GetAPICallResult)(s32_ pipe, u64_ call, void* cb,
                                                int cubCallback, int expected, int* failed);
typedef void* (__cdecl *PFN_Accessor)(void);

typedef u64_        (__cdecl *PFN_User_GetSteamID)(void*);
typedef const char* (__cdecl *PFN_Friends_GetPersonaName)(void*);
typedef const char* (__cdecl *PFN_Friends_GetFriendPersonaName)(void*, u64_);
typedef int         (__cdecl *PFN_Friends_RequestUserInformation)(void*, u64_, int);
typedef void        (__cdecl *PFN_Friends_ActivateInviteDialog)(void*, u64_);
typedef int         (__cdecl *PFN_Friends_SetRichPresence)(void*, const char*, const char*);
typedef void        (__cdecl *PFN_Friends_ClearRichPresence)(void*);

typedef u64_        (__cdecl *PFN_MM_CreateLobby)(void*, int, int);
typedef u64_        (__cdecl *PFN_MM_JoinLobby)(void*, u64_);
typedef void        (__cdecl *PFN_MM_LeaveLobby)(void*, u64_);
typedef int         (__cdecl *PFN_MM_GetNumLobbyMembers)(void*, u64_);
typedef u64_        (__cdecl *PFN_MM_GetLobbyMemberByIndex)(void*, u64_, int);
typedef u64_        (__cdecl *PFN_MM_GetLobbyOwner)(void*, u64_);
typedef int         (__cdecl *PFN_MM_SetLobbyData)(void*, u64_, const char*, const char*);
typedef const char* (__cdecl *PFN_MM_GetLobbyData)(void*, u64_, const char*);

typedef int  (__cdecl *PFN_Msg_SendMessageToUser)(void*, const void* identity,
                                                  const void* data, u32_ cb,
                                                  int flags, int channel);
typedef int  (__cdecl *PFN_Msg_ReceiveOnChannel)(void*, int channel, void** out, int maxMessages);
typedef int  (__cdecl *PFN_Msg_AcceptSession)(void*, const void* identity);
typedef void (__cdecl *PFN_Msg_CloseSession)(void*, const void* identity);
typedef void (__cdecl *PFN_NetMsg_Release)(void* msg);

static HMODULE s_dll;

static PFN_InitFlat            s_InitFlat;
static PFN_Init                s_Init;
static PFN_Shutdown            s_ShutdownFn;
static PFN_GetHSteamPipe       s_GetPipe;
static PFN_MD_Init             s_MdInit;
static PFN_MD_RunFrame         s_MdRunFrame;
static PFN_MD_GetNextCallback  s_MdNext;
static PFN_MD_FreeLastCallback s_MdFree;
static PFN_MD_GetAPICallResult s_MdResult;

static void* s_iUser;
static void* s_iFriends;
static void* s_iMatchmaking;
static void* s_iUtils;
static void* s_iMessages;

static PFN_User_GetSteamID                s_UserGetSteamID;
static PFN_Friends_GetPersonaName         s_GetPersonaName;
static PFN_Friends_GetFriendPersonaName   s_GetFriendName;
static PFN_Friends_RequestUserInformation s_RequestUserInfo;
static PFN_Friends_ActivateInviteDialog   s_InviteDialog;
static PFN_Friends_SetRichPresence        s_SetRichPresence;
static PFN_Friends_ClearRichPresence      s_ClearRichPresence;

static PFN_MM_CreateLobby          s_CreateLobby;
static PFN_MM_JoinLobby            s_JoinLobby;
static PFN_MM_LeaveLobby           s_LeaveLobby;
static PFN_MM_GetNumLobbyMembers   s_NumMembers;
static PFN_MM_GetLobbyMemberByIndex s_MemberByIndex;
static PFN_MM_GetLobbyOwner        s_LobbyOwner;
static PFN_MM_SetLobbyData         s_SetLobbyData;
static PFN_MM_GetLobbyData         s_GetLobbyData;

static PFN_Msg_SendMessageToUser s_SendToUser;
static PFN_Msg_ReceiveOnChannel  s_ReceiveOnChannel;
static PFN_Msg_AcceptSession     s_AcceptSession;
static PFN_Msg_CloseSession      s_CloseSession;
static PFN_NetMsg_Release        s_MsgRelease;

#endif /* _WIN32 */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int   s_ready;
static int   s_triedInit;
static int   s_interfaces;
static s32_  s_pipe;

static u64_  s_selfId;
static char  s_personaName[SHSTEAM_NAME_MAX];

static int   s_lobbyState;
static u64_  s_lobbyId;
static u64_  s_pendingCreateCall;
static u64_  s_pendingJoinCall;
static u64_  s_pendingJoinLobby;   /* asked for from outside the game */
static u64_  s_cmdLineLobby;

static u64_  s_members[SHSTEAM_MAX_MEMBERS];
static int   s_memberCount;

static char  s_statusLine[128];

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

static void* ShSteam_Sym(const char* name)
{
    return s_dll ? (void*)GetProcAddress(s_dll, name) : NULL;
}

/* Interface accessor names carry a version suffix that moves between SDK
 * releases, and a player's steam_api64.dll came from whichever game they
 * copied it out of. Try the versions in descending order and take the first
 * that resolves, rather than pinning one and failing on everything else. */
static void* ShSteam_Interface(const char* const* candidates, int count, const char* label)
{
    int i;
    for (i = 0; i < count; i++)
    {
        PFN_Accessor fn = (PFN_Accessor)ShSteam_Sym(candidates[i]);
        if (fn)
        {
            void* p = fn();
            if (p)
            {
                SH_DBG("[STEAM] %s -> %s", label, candidates[i]);
                return p;
            }
        }
    }
    SH_DBG("[STEAM] %s: no supported version exported by this steam_api64.dll", label);
    return NULL;
}

/* SteamAPI_Init reads the app id from steam_appid.txt in the working
 * directory when the game was not launched by Steam. Writing it if it is
 * missing turns "nothing happens and there is no message" into a working
 * first run; the file is tiny, and its contents are the configured id. */
static void ShSteam_EnsureAppIdFile(unsigned int appId)
{
    FILE* f = fopen("steam_appid.txt", "r");
    if (f)
    {
        fclose(f);
        return;
    }
    f = fopen("steam_appid.txt", "w");
    if (!f)
    {
        SH_DBG("[STEAM] could not write steam_appid.txt - Steam will not initialise "
               "unless the game is launched from Steam");
        return;
    }
    fprintf(f, "%u\n", appId);
    fclose(f);
    SH_DBG("[STEAM] wrote steam_appid.txt (%u)", appId);
}

#endif /* _WIN32 */

int ShSteam_Init(unsigned int appId)
{
#if !defined(_WIN32)
    (void)appId;
    return 0;
#else
    static const char* const userNames[] = {
        "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022",
        "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020"
    };
    static const char* const friendNames[] = {
        "SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017",
        "SteamAPI_SteamFriends_v016", "SteamAPI_SteamFriends_v015"
    };
    static const char* const mmNames[] = {
        "SteamAPI_SteamMatchmaking_v009"
    };
    static const char* const utilNames[] = {
        "SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009"
    };
    static const char* const msgNames[] = {
        "SteamAPI_SteamNetworkingMessages_SteamAPI_v002",
        "SteamAPI_SteamNetworkingMessages_v002"
    };

    if (s_triedInit)
    {
        return s_ready;
    }
    s_triedInit = 1;

    if (appId == 0)
    {
        appId = SHSTEAM_DEFAULT_APPID;
    }

    s_dll = LoadLibraryA("steam_api64.dll");
    if (!s_dll)
    {
        SH_DBG("[STEAM] steam_api64.dll not found next to the exe - Steam features off. "
               "Drop one in from the Steamworks SDK (redistributable_bin/win64) to enable them.");
        return 0;
    }

    s_InitFlat   = (PFN_InitFlat)ShSteam_Sym("SteamAPI_InitFlat");
    s_Init       = (PFN_Init)ShSteam_Sym("SteamAPI_Init");
    if (!s_Init)
    {
        s_Init = (PFN_Init)ShSteam_Sym("SteamAPI_InitSafe");
    }
    s_ShutdownFn = (PFN_Shutdown)ShSteam_Sym("SteamAPI_Shutdown");
    s_GetPipe    = (PFN_GetHSteamPipe)ShSteam_Sym("SteamAPI_GetHSteamPipe");
    s_MdInit     = (PFN_MD_Init)ShSteam_Sym("SteamAPI_ManualDispatch_Init");
    s_MdRunFrame = (PFN_MD_RunFrame)ShSteam_Sym("SteamAPI_ManualDispatch_RunFrame");
    s_MdNext     = (PFN_MD_GetNextCallback)ShSteam_Sym("SteamAPI_ManualDispatch_GetNextCallback");
    s_MdFree     = (PFN_MD_FreeLastCallback)ShSteam_Sym("SteamAPI_ManualDispatch_FreeLastCallback");
    s_MdResult   = (PFN_MD_GetAPICallResult)ShSteam_Sym("SteamAPI_ManualDispatch_GetAPICallResult");

    {
        /* Report the symbol that is actually missing. An earlier version of
         * this blamed ManualDispatch for every failure and was wrong the first
         * time it fired: the DLL had all of ManualDispatch and was only
         * missing SteamAPI_Init, which modern SDKs do not export at all. */
        const char* missing = NULL;
        if (!s_InitFlat && !s_Init) missing = "SteamAPI_InitFlat / SteamAPI_Init";
        else if (!s_GetPipe)        missing = "SteamAPI_GetHSteamPipe";
        else if (!s_MdInit)         missing = "SteamAPI_ManualDispatch_Init";
        else if (!s_MdRunFrame)     missing = "SteamAPI_ManualDispatch_RunFrame";
        else if (!s_MdNext)         missing = "SteamAPI_ManualDispatch_GetNextCallback";
        else if (!s_MdFree)         missing = "SteamAPI_ManualDispatch_FreeLastCallback";
        if (missing)
        {
            SH_DBG("[STEAM] steam_api64.dll does not export %s - it is older than "
                   "SDK 1.47 (2020). Replace it with one from a current SDK.", missing);
            FreeLibrary(s_dll);
            s_dll = NULL;
            return 0;
        }
    }

    ShSteam_EnsureAppIdFile(appId);

    if (s_InitFlat)
    {
        /* SteamErrMsg is char[1024], and InitFlat fills it on failure. */
        char err[1024];
        int  r;
        memset(err, 0, sizeof(err));
        r = s_InitFlat(err);
        if (r != 0)
        {
            SH_DBG("[STEAM] SteamAPI_InitFlat failed (%d): %s (app id %u)",
                   r, err[0] ? err : "no detail", appId);
            FreeLibrary(s_dll);
            s_dll = NULL;
            return 0;
        }
    }
    else if (!s_Init())
    {
        SH_DBG("[STEAM] SteamAPI_Init failed - is Steam running, and is the account "
               "logged in? (app id %u)", appId);
        FreeLibrary(s_dll);
        s_dll = NULL;
        return 0;
    }

    s_pipe = s_GetPipe();
    s_MdInit();

    s_iUser        = ShSteam_Interface(userNames,   (int)(sizeof(userNames) / sizeof(userNames[0])),   "ISteamUser");
    s_iFriends     = ShSteam_Interface(friendNames, (int)(sizeof(friendNames) / sizeof(friendNames[0])), "ISteamFriends");
    s_iMatchmaking = ShSteam_Interface(mmNames,     (int)(sizeof(mmNames) / sizeof(mmNames[0])),       "ISteamMatchmaking");
    s_iUtils       = ShSteam_Interface(utilNames,   (int)(sizeof(utilNames) / sizeof(utilNames[0])),   "ISteamUtils");
    s_iMessages    = ShSteam_Interface(msgNames,    (int)(sizeof(msgNames) / sizeof(msgNames[0])),     "ISteamNetworkingMessages");

    s_UserGetSteamID    = (PFN_User_GetSteamID)ShSteam_Sym("SteamAPI_ISteamUser_GetSteamID");
    s_GetPersonaName    = (PFN_Friends_GetPersonaName)ShSteam_Sym("SteamAPI_ISteamFriends_GetPersonaName");
    s_GetFriendName     = (PFN_Friends_GetFriendPersonaName)ShSteam_Sym("SteamAPI_ISteamFriends_GetFriendPersonaName");
    s_RequestUserInfo   = (PFN_Friends_RequestUserInformation)ShSteam_Sym("SteamAPI_ISteamFriends_RequestUserInformation");
    s_InviteDialog      = (PFN_Friends_ActivateInviteDialog)ShSteam_Sym("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    s_SetRichPresence   = (PFN_Friends_SetRichPresence)ShSteam_Sym("SteamAPI_ISteamFriends_SetRichPresence");
    s_ClearRichPresence = (PFN_Friends_ClearRichPresence)ShSteam_Sym("SteamAPI_ISteamFriends_ClearRichPresence");

    s_CreateLobby   = (PFN_MM_CreateLobby)ShSteam_Sym("SteamAPI_ISteamMatchmaking_CreateLobby");
    s_JoinLobby     = (PFN_MM_JoinLobby)ShSteam_Sym("SteamAPI_ISteamMatchmaking_JoinLobby");
    s_LeaveLobby    = (PFN_MM_LeaveLobby)ShSteam_Sym("SteamAPI_ISteamMatchmaking_LeaveLobby");
    s_NumMembers    = (PFN_MM_GetNumLobbyMembers)ShSteam_Sym("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    s_MemberByIndex = (PFN_MM_GetLobbyMemberByIndex)ShSteam_Sym("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    s_LobbyOwner    = (PFN_MM_GetLobbyOwner)ShSteam_Sym("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    s_SetLobbyData  = (PFN_MM_SetLobbyData)ShSteam_Sym("SteamAPI_ISteamMatchmaking_SetLobbyData");
    s_GetLobbyData  = (PFN_MM_GetLobbyData)ShSteam_Sym("SteamAPI_ISteamMatchmaking_GetLobbyData");

    s_SendToUser       = (PFN_Msg_SendMessageToUser)ShSteam_Sym("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    s_ReceiveOnChannel = (PFN_Msg_ReceiveOnChannel)ShSteam_Sym("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    s_AcceptSession    = (PFN_Msg_AcceptSession)ShSteam_Sym("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    s_CloseSession     = (PFN_Msg_CloseSession)ShSteam_Sym("SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    s_MsgRelease       = (PFN_NetMsg_Release)ShSteam_Sym("SteamAPI_SteamNetworkingMessage_t_Release");

    s_interfaces = 0;
    if (s_iUser && s_UserGetSteamID)                  s_interfaces |= SHSTEAM_IF_USER;
    if (s_iFriends && s_GetPersonaName)               s_interfaces |= SHSTEAM_IF_FRIENDS;
    if (s_iMatchmaking && s_CreateLobby && s_JoinLobby) s_interfaces |= SHSTEAM_IF_MATCHMAKE;
    if (s_iUtils)                                     s_interfaces |= SHSTEAM_IF_UTILS;
    if (s_iMessages && s_SendToUser && s_ReceiveOnChannel && s_MsgRelease)
                                                      s_interfaces |= SHSTEAM_IF_MESSAGES;

    if (s_interfaces & SHSTEAM_IF_USER)
    {
        s_selfId = s_UserGetSteamID(s_iUser);
    }
    if (s_interfaces & SHSTEAM_IF_FRIENDS)
    {
        const char* n = s_GetPersonaName(s_iFriends);
        if (n)
        {
            strncpy(s_personaName, n, sizeof(s_personaName) - 1);
            s_personaName[sizeof(s_personaName) - 1] = '\0';
        }
    }

    s_ready = 1;
    SH_DBG("[STEAM] ready: \"%s\" (%llu), app id %u, interfaces 0x%02X",
           s_personaName[0] ? s_personaName : "?",
           (unsigned long long)s_selfId, appId, s_interfaces);
    if (!(s_interfaces & SHSTEAM_IF_MESSAGES))
    {
        SH_DBG("[STEAM] ISteamNetworkingMessages missing - lobbies and invites will "
               "work, peer-to-peer will not");
    }
    return 1;
#endif
}

void ShSteam_Shutdown(void)
{
#if defined(_WIN32)
    if (!s_ready)
    {
        return;
    }
    ShSteam_LeaveLobby();
    if (s_ClearRichPresence && s_iFriends)
    {
        s_ClearRichPresence(s_iFriends);
    }
    if (s_ShutdownFn)
    {
        s_ShutdownFn();
    }
    if (s_dll)
    {
        FreeLibrary(s_dll);
        s_dll = NULL;
    }
    s_ready = 0;
#endif
}

int ShSteam_Available(void)
{
    return s_ready;
}

int ShSteam_Interfaces(void)
{
    return s_interfaces;
}

unsigned long long ShSteam_SelfId(void)
{
    return s_selfId;
}

const char* ShSteam_PersonaName(void)
{
    return s_personaName;
}

const char* ShSteam_NameOf(unsigned long long steamId)
{
#if defined(_WIN32)
    if (!s_ready || !s_GetFriendName || !s_iFriends)
    {
        return "";
    }
    if (steamId == s_selfId)
    {
        return s_personaName;
    }
    {
        const char* n = s_GetFriendName(s_iFriends, steamId);
        if (!n || !n[0])
        {
            /* Steam has no cached persona for this account yet; ask for one so
             * the NEXT frame has a name instead of a number forever. */
            if (s_RequestUserInfo)
            {
                s_RequestUserInfo(s_iFriends, steamId, 1);
            }
            return "";
        }
        return n;
    }
#else
    (void)steamId;
    return "";
#endif
}

/* ------------------------------------------------------------------ */
/* Lobby                                                               */
/* ------------------------------------------------------------------ */

int ShSteam_LobbyState(void)
{
    return s_lobbyState;
}

unsigned long long ShSteam_LobbyId(void)
{
    return s_lobbyId;
}

unsigned long long ShSteam_LobbyOwner(void)
{
#if defined(_WIN32)
    if (!s_ready || !s_LobbyOwner || !s_lobbyId)
    {
        return 0;
    }
    return s_LobbyOwner(s_iMatchmaking, s_lobbyId);
#else
    return 0;
#endif
}

int ShSteam_IsLobbyOwner(void)
{
    unsigned long long o = ShSteam_LobbyOwner();
    return o != 0 && o == s_selfId;
}

int ShSteam_MemberCount(void)
{
    return s_memberCount;
}

unsigned long long ShSteam_Member(int i)
{
    return (i >= 0 && i < s_memberCount) ? s_members[i] : 0;
}

#if defined(_WIN32)
static void ShSteam_RefreshMembers(void)
{
    int n, i;

    s_memberCount = 0;
    if (!s_ready || !s_NumMembers || !s_MemberByIndex || !s_lobbyId)
    {
        return;
    }
    n = s_NumMembers(s_iMatchmaking, s_lobbyId);
    if (n > SHSTEAM_MAX_MEMBERS)
    {
        n = SHSTEAM_MAX_MEMBERS;
    }
    for (i = 0; i < n; i++)
    {
        s_members[s_memberCount++] = s_MemberByIndex(s_iMatchmaking, s_lobbyId, i);
    }
}
#endif

void ShSteam_CreateLobby(int lobbyType, int maxMembers)
{
#if defined(_WIN32)
    if (!s_ready || !(s_interfaces & SHSTEAM_IF_MATCHMAKE))
    {
        return;
    }
    if (s_lobbyState == SHSTEAM_LOBBY_CREATING || s_lobbyState == SHSTEAM_LOBBY_JOINING)
    {
        return;
    }
    ShSteam_LeaveLobby();
    if (maxMembers < 2)  maxMembers = 2;
    if (maxMembers > SHSTEAM_MAX_MEMBERS) maxMembers = SHSTEAM_MAX_MEMBERS;

    s_pendingCreateCall = s_CreateLobby(s_iMatchmaking, lobbyType, maxMembers);
    s_lobbyState        = SHSTEAM_LOBBY_CREATING;
    SH_DBG("[STEAM] creating a lobby (type %d, up to %d)", lobbyType, maxMembers);
#else
    (void)lobbyType; (void)maxMembers;
#endif
}

void ShSteam_JoinLobby(unsigned long long lobbyId)
{
#if defined(_WIN32)
    if (!s_ready || !(s_interfaces & SHSTEAM_IF_MATCHMAKE) || !lobbyId)
    {
        return;
    }
    if (s_lobbyId == lobbyId && s_lobbyState == SHSTEAM_LOBBY_IN)
    {
        return;
    }
    ShSteam_LeaveLobby();
    s_pendingJoinCall = s_JoinLobby(s_iMatchmaking, lobbyId);
    s_lobbyState      = SHSTEAM_LOBBY_JOINING;
    SH_DBG("[STEAM] joining lobby %llu", (unsigned long long)lobbyId);
#else
    (void)lobbyId;
#endif
}

void ShSteam_LeaveLobby(void)
{
#if defined(_WIN32)
    if (s_ready && s_LeaveLobby && s_lobbyId)
    {
        s_LeaveLobby(s_iMatchmaking, s_lobbyId);
        SH_DBG("[STEAM] left lobby %llu", (unsigned long long)s_lobbyId);
    }
#endif
    s_lobbyId     = 0;
    s_lobbyState  = SHSTEAM_LOBBY_NONE;
    s_memberCount = 0;
}

void ShSteam_SetLobbyData(const char* key, const char* value)
{
#if defined(_WIN32)
    if (s_ready && s_SetLobbyData && s_lobbyId && key && value)
    {
        s_SetLobbyData(s_iMatchmaking, s_lobbyId, key, value);
    }
#else
    (void)key; (void)value;
#endif
}

const char* ShSteam_GetLobbyData(const char* key)
{
#if defined(_WIN32)
    if (s_ready && s_GetLobbyData && s_lobbyId && key)
    {
        const char* v = s_GetLobbyData(s_iMatchmaking, s_lobbyId, key);
        return v ? v : "";
    }
#else
    (void)key;
#endif
    return "";
}

void ShSteam_OpenInviteOverlay(void)
{
#if defined(_WIN32)
    if (s_ready && s_InviteDialog && s_lobbyId)
    {
        s_InviteDialog(s_iFriends, s_lobbyId);
        SH_DBG("[STEAM] opened the invite overlay for lobby %llu",
               (unsigned long long)s_lobbyId);
    }
    else
    {
        SH_DBG("[STEAM] invite overlay unavailable (in a lobby: %s, overlay export: %s)",
               s_lobbyId ? "yes" : "no", s_InviteDialog ? "yes" : "no");
    }
#endif
}

unsigned long long ShSteam_TakePendingJoin(void)
{
    unsigned long long id = s_pendingJoinLobby;
    s_pendingJoinLobby = 0;
    return id;
}

void ShSteam_SetCommandLineLobby(unsigned long long lobbyId)
{
    s_cmdLineLobby     = lobbyId;
    s_pendingJoinLobby = lobbyId;
}

void ShSteam_SetRichPresence(const char* key, const char* value)
{
#if defined(_WIN32)
    if (!s_ready || !s_iFriends)
    {
        return;
    }
    if (!key)
    {
        if (s_ClearRichPresence)
        {
            s_ClearRichPresence(s_iFriends);
        }
        return;
    }
    if (s_SetRichPresence)
    {
        s_SetRichPresence(s_iFriends, key, value ? value : "");
    }
#else
    (void)key; (void)value;
#endif
}

/* ------------------------------------------------------------------ */
/* Peer to peer                                                        */
/* ------------------------------------------------------------------ */

void ShSteam_AcceptSession(unsigned long long steamId)
{
#if defined(_WIN32)
    ShSteamIdentity id;
    if (!s_ready || !s_AcceptSession || !steamId)
    {
        return;
    }
    ShSteamIdentity_Set(&id, steamId);
    s_AcceptSession(s_iMessages, id.raw);
#else
    (void)steamId;
#endif
}

int ShSteam_Send(unsigned long long steamId, const void* buf, int len, int reliable)
{
#if defined(_WIN32)
    ShSteamIdentity id;
    int             r;

    if (!s_ready || !(s_interfaces & SHSTEAM_IF_MESSAGES) || !steamId || !buf || len <= 0)
    {
        return 0;
    }
    ShSteamIdentity_Set(&id, steamId);
    r = s_SendToUser(s_iMessages, id.raw, buf, (u32_)len,
                     reliable ? SEND_RELIABLE : SEND_UNRELIABLE, SHSTEAM_CHANNEL);
    return r == ERESULT_OK;
#else
    (void)steamId; (void)buf; (void)len; (void)reliable;
    return 0;
#endif
}

int ShSteam_Recv(unsigned long long* outFrom, void* buf, int cap)
{
#if defined(_WIN32)
    void* msg = NULL;
    int   got;
    int   n = 0;

    if (!s_ready || !(s_interfaces & SHSTEAM_IF_MESSAGES) || !buf || cap <= 0)
    {
        return 0;
    }

    /* One at a time: the caller drains in a loop anyway, and asking for one
     * keeps the release path a single object rather than an array we would
     * have to unwind on a short copy. */
    got = s_ReceiveOnChannel(s_iMessages, SHSTEAM_CHANNEL, &msg, 1);
    if (got <= 0 || !msg)
    {
        return 0;
    }

    {
        const u8_* m    = (const u8_*)msg;
        void*      data = NULL;
        s32_       size = 0;

        memcpy(&data, m + MSG_OFS_DATA, sizeof(data));
        memcpy(&size, m + MSG_OFS_SIZE, sizeof(size));

        if (data && size > 0)
        {
            n = (size > cap) ? cap : (int)size;
            memcpy(buf, data, (size_t)n);
        }
        if (outFrom)
        {
            *outFrom = ShSteamIdentity_Get(m + MSG_OFS_IDENTITY);
        }
    }
    s_MsgRelease(msg);
    return n;
#else
    (void)outFrom; (void)buf; (void)cap;
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Callback dispatch                                                   */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

/* An asynchronous Steam call answers with a "call result" rather than a plain
 * callback: the dispatch loop hands it back tagged with the SteamAPICall_t we
 * were given at request time, and we match it against the one we are waiting
 * for. */
static void ShSteam_HandleCallResult(const ShSteamCallbackMsg* msg)
{
    u64_ call = 0;
    if (msg->cubParam >= (int)sizeof(u64_))
    {
        memcpy(&call, msg->pubParam, sizeof(call));
    }
    (void)call;
}

static void ShSteam_OnLobbyCreated(const u8_* p, int cb)
{
    s32_ result = 0;
    u64_ lobby  = 0;

    if (cb >= 16)
    {
        memcpy(&result, p + 0, sizeof(result));
        memcpy(&lobby,  p + 8, sizeof(lobby));
    }
    if (result == ERESULT_OK && lobby)
    {
        s_lobbyId    = lobby;
        s_lobbyState = SHSTEAM_LOBBY_IN;
        ShSteam_RefreshMembers();
        SH_DBG("[STEAM] lobby %llu created, %d member(s)",
               (unsigned long long)lobby, s_memberCount);
    }
    else
    {
        s_lobbyState = SHSTEAM_LOBBY_FAILED;
        SH_DBG("[STEAM] lobby creation failed (EResult %d)", (int)result);
    }
}

static void ShSteam_OnLobbyEnter(const u8_* p, int cb)
{
    u64_ lobby    = 0;
    u32_ response = 0;

    if (cb >= 20)
    {
        memcpy(&lobby,    p + 0,  sizeof(lobby));
        memcpy(&response, p + 16, sizeof(response));
    }
    /* k_EChatRoomEnterResponseSuccess == 1. */
    if (response == 1 && lobby)
    {
        s_lobbyId    = lobby;
        s_lobbyState = SHSTEAM_LOBBY_IN;
        ShSteam_RefreshMembers();
        SH_DBG("[STEAM] entered lobby %llu, %d member(s), owner %s",
               (unsigned long long)lobby, s_memberCount,
               ShSteam_IsLobbyOwner() ? "me" : "someone else");
    }
    else
    {
        s_lobbyState = SHSTEAM_LOBBY_FAILED;
        SH_DBG("[STEAM] could not enter lobby (response %u)", (unsigned)response);
    }
}

static void ShSteam_OnLobbyChatUpdate(const u8_* p, int cb)
{
    u64_ changed = 0;
    if (cb >= 16)
    {
        memcpy(&changed, p + 8, sizeof(changed));
    }
    ShSteam_RefreshMembers();
    SH_DBG("[STEAM] lobby membership changed (%llu) - %d member(s) now",
           (unsigned long long)changed, s_memberCount);
}

static void ShSteam_OnJoinRequested(const u8_* p, int cb)
{
    u64_ lobby = 0;
    if (cb >= 8)
    {
        memcpy(&lobby, p + 0, sizeof(lobby));
    }
    if (lobby)
    {
        s_pendingJoinLobby = lobby;
        SH_DBG("[STEAM] a friend invited us to lobby %llu", (unsigned long long)lobby);
    }
}

static void ShSteam_OnSessionRequest(const u8_* p, int cb)
{
    u64_ from;
    int  i;

    if (cb < SNI_SIZE)
    {
        return;
    }
    from = ShSteamIdentity_Get(p);

    /* Only lobby members. Without this check anyone who learns the SteamID
     * could open a session and start feeding the protocol parser. */
    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i] == from)
        {
            ShSteam_AcceptSession(from);
            SH_DBG("[STEAM] accepted a peer session from %llu (%s)",
                   (unsigned long long)from, ShSteam_NameOf(from));
            return;
        }
    }
    SH_DBG("[STEAM] refused a peer session from %llu: not in our lobby",
           (unsigned long long)from);
}

#endif /* _WIN32 */

void ShSteam_RunCallbacks(void)
{
#if defined(_WIN32)
    ShSteamCallbackMsg msg;

    if (!s_ready)
    {
        return;
    }

    s_MdRunFrame(s_pipe);

    while (s_MdNext(s_pipe, &msg))
    {
        switch (msg.iCallback)
        {
        case CB_LobbyCreated:
            ShSteam_OnLobbyCreated(msg.pubParam, msg.cubParam);
            break;
        case CB_LobbyEnter:
            ShSteam_OnLobbyEnter(msg.pubParam, msg.cubParam);
            break;
        case CB_LobbyChatUpdate:
            ShSteam_OnLobbyChatUpdate(msg.pubParam, msg.cubParam);
            break;
        case CB_LobbyDataUpdate:
            ShSteam_RefreshMembers();
            break;
        case CB_GameLobbyJoinRequested:
            ShSteam_OnJoinRequested(msg.pubParam, msg.cubParam);
            break;
        case CB_MessagesSessionRequest:
            ShSteam_OnSessionRequest(msg.pubParam, msg.cubParam);
            break;
        case CB_MessagesSessionFailed:
            SH_DBG("[STEAM] a peer session failed");
            break;
        default:
            break;
        }
        s_MdFree(s_pipe);
    }

    /* A lobby created or joined through the CALL RESULT path reports through
     * the same queue above on every SDK this supports, so nothing else is
     * needed here; the pending handles are kept only so a future version that
     * needs GetAPICallResult has them. */
    (void)s_pendingCreateCall;
    (void)s_pendingJoinCall;
    (void)s_MdResult;
    (void)ShSteam_HandleCallResult;
#endif
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

void ShSteam_StatusLine(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    if (!s_ready)
    {
        snprintf(out, (size_t)cap, "Steam: off");
        return;
    }
    switch (s_lobbyState)
    {
    case SHSTEAM_LOBBY_CREATING:
        snprintf(out, (size_t)cap, "Steam: creating a session...");
        break;
    case SHSTEAM_LOBBY_JOINING:
        snprintf(out, (size_t)cap, "Steam: joining a session...");
        break;
    case SHSTEAM_LOBBY_IN:
        snprintf(out, (size_t)cap, "Steam: %d in session, %s",
                 s_memberCount, ShSteam_IsLobbyOwner() ? "hosting" : "guest");
        break;
    case SHSTEAM_LOBBY_FAILED:
        snprintf(out, (size_t)cap, "Steam: session failed");
        break;
    default:
        snprintf(out, (size_t)cap, "Steam: %s, no session",
                 s_personaName[0] ? s_personaName : "signed in");
        break;
    }
    (void)s_cmdLineLobby;
    memcpy(s_statusLine, out, (size_t)((cap < (int)sizeof(s_statusLine)) ? cap : (int)sizeof(s_statusLine)));
    s_statusLine[sizeof(s_statusLine) - 1] = '\0';
}
