/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * steam_probe.c - exercise the port's Steam layer without launching the game.
 *
 * It links sh_net_steam.c ITSELF, not a copy, so what it proves is what the
 * game will do: that steam_api64.dll loads, that every flat-API name the port
 * asks for resolves against this particular DLL, that manual dispatch delivers
 * callbacks, and that a lobby can actually be created and left.
 *
 * That last part matters more than it sounds. Interface accessor names carry a
 * version suffix (SteamAPI_SteamFriends_v017 and so on) that moves between SDK
 * releases, and a player's steam_api64.dll came from whichever game they
 * copied it out of. This turns "Steam features do not work" into a line
 * naming the interface that did not resolve.
 *
 *   gcc -O2 -I../pc_port/include steam_probe.c ../pc_port/src/net/sh_net_steam.c -o steam_probe.exe
 *
 * Needs steam_api64.dll beside it and Steam running and signed in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define PROBE_SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define PROBE_SLEEP(ms) usleep((ms) * 1000)
#endif

#include "sh_net_steam.h"

/* sh_net_steam.c logs through SH_DBG, which writes to this handle. Pointing it
 * at stdout is what makes the probe's output the layer's own reporting rather
 * than a parallel commentary that could disagree with it. */
FILE* g_ShDebugLog;
int   g_ShDebugEchoStdout;
void (*g_ShOverlayPushLine)(const char*);
void (*g_ShOverlayToastLine)(const char*);

static int g_fail;

#define CHECK(cond, ...)                                                 \
    do {                                                                 \
        if (cond) { printf("  ok   " __VA_ARGS__); printf("\n"); }        \
        else      { printf("  FAIL " __VA_ARGS__); printf("\n"); g_fail++; } \
    } while (0)

static void PumpFor(int ms)
{
    int spent = 0;
    while (spent < ms)
    {
        ShSteam_RunCallbacks();
        PROBE_SLEEP(20);
        spent += 20;
    }
}

int main(int argc, char** argv)
{
    unsigned int appId = (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 10)
                                    : SHSTEAM_DEFAULT_APPID;
    int          ifaces;
    int          i;

    g_ShDebugLog = stdout;

    printf("Silent Hill Online - Steam layer probe (app id %u)\n\n", appId);

    printf("init\n");
    if (!ShSteam_Init(appId))
    {
        printf("\n  Steam did not come up. The lines above say which step failed;\n"
               "  the usual causes are no steam_api64.dll beside this exe, or\n"
               "  Steam not running.\n");
        return 1;
    }
    CHECK(ShSteam_Available(), "the layer reports itself available");

    printf("\ninterfaces\n");
    ifaces = ShSteam_Interfaces();
    CHECK(ifaces & SHSTEAM_IF_USER,      "ISteamUser");
    CHECK(ifaces & SHSTEAM_IF_FRIENDS,   "ISteamFriends");
    CHECK(ifaces & SHSTEAM_IF_MATCHMAKE, "ISteamMatchmaking");
    CHECK(ifaces & SHSTEAM_IF_UTILS,     "ISteamUtils");
    CHECK(ifaces & SHSTEAM_IF_MESSAGES,  "ISteamNetworkingMessages (peer to peer)");

    printf("\nidentity\n");
    CHECK(ShSteam_SelfId() != 0, "SteamID is %llu", (unsigned long long)ShSteam_SelfId());
    CHECK(ShSteam_PersonaName()[0] != '\0', "persona name is \"%s\"", ShSteam_PersonaName());

    printf("\nrich presence\n");
    ShSteam_SetRichPresence("status", "Probing Silent Hill");
    ShSteam_SetRichPresence("steam_display", "#Status");
    printf("  set   status = \"Probing Silent Hill\" (visible to friends while this runs)\n");

    if (!(ifaces & SHSTEAM_IF_MATCHMAKE))
    {
        printf("\nlobby: skipped, ISteamMatchmaking did not resolve\n");
    }
    else
    {
        printf("\nlobby\n");
        ShSteam_CreateLobby(SHSTEAM_LOBBY_FRIENDSONLY, 4);
        CHECK(ShSteam_LobbyState() == SHSTEAM_LOBBY_CREATING,
              "creation started");

        /* Lobby creation is a round trip to Steam's servers. Five seconds is
         * generous; it usually lands in well under one. */
        for (i = 0; i < 250 && ShSteam_LobbyState() == SHSTEAM_LOBBY_CREATING; i++)
        {
            ShSteam_RunCallbacks();
            PROBE_SLEEP(20);
        }

        CHECK(ShSteam_LobbyState() == SHSTEAM_LOBBY_IN,
              "the callback arrived and we are in a lobby");
        if (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN)
        {
            CHECK(ShSteam_LobbyId() != 0, "lobby id is %llu",
                  (unsigned long long)ShSteam_LobbyId());
            CHECK(ShSteam_IsLobbyOwner(), "we own it");
            CHECK(ShSteam_MemberCount() == 1, "it has %d member", ShSteam_MemberCount());

            ShSteam_SetLobbyData("game", "silenthill-online");
            ShSteam_SetLobbyData("proto", "1");
            PumpFor(300);
            CHECK(strcmp(ShSteam_GetLobbyData("game"), "silenthill-online") == 0,
                  "lobby metadata round-trips (game=%s)", ShSteam_GetLobbyData("game"));

            printf("\n  To test an invite: run this with -invite while a friend is\n"
                   "  online, and the Steam overlay will open. Joining that lobby\n"
                   "  launches the game with +connect_lobby %llu.\n",
                   (unsigned long long)ShSteam_LobbyId());

            if (argc > 2 && strcmp(argv[2], "-invite") == 0)
            {
                ShSteam_OpenInviteOverlay();
                printf("\n  overlay requested; holding the lobby open for 60s...\n");
                PumpFor(60000);
            }

            printf("\nleaving\n");
            ShSteam_LeaveLobby();
            PumpFor(300);
            CHECK(ShSteam_LobbyState() == SHSTEAM_LOBBY_NONE, "left cleanly");
        }
    }

    /* Peer to peer, against ourselves. Two accounts are needed for a real
     * session test, but a loopback send exercises the parts that are pure
     * assumption: the SteamNetworkingIdentity layout going in, and the
     * SteamNetworkingMessage_t offsets coming back. Steam may legitimately
     * refuse to route a message to the sending account, so a failure here is
     * reported as UNPROVEN rather than counted against the build. */
    if (ifaces & SHSTEAM_IF_MESSAGES)
    {
        static const char payload[] = "SHO1-loopback";
        char               got[64];
        unsigned long long from = 0;
        int                n = 0;

        printf("\npeer to peer (loopback)\n");
        ShSteam_AcceptSession(ShSteam_SelfId());
        if (!ShSteam_Send(ShSteam_SelfId(), payload, (int)sizeof(payload), 1))
        {
            printf("  ..     Steam would not route a message to our own account.\n"
                   "         Expected; this path needs two accounts to prove.\n");
        }
        else
        {
            for (i = 0; i < 100 && n == 0; i++)
            {
                ShSteam_RunCallbacks();
                n = ShSteam_Recv(&from, got, (int)sizeof(got));
                if (n == 0) PROBE_SLEEP(20);
            }
            if (n <= 0)
            {
                printf("  ..     sent, but nothing came back. Steam does not always\n"
                       "         loop a message to the sending account; UNPROVEN.\n");
            }
            else
            {
                CHECK(n == (int)sizeof(payload) && memcmp(got, payload, sizeof(payload)) == 0,
                      "the payload survived the round trip (%d bytes)", n);
                CHECK(from == ShSteam_SelfId(),
                      "the sender identity decoded to %llu", (unsigned long long)from);
            }
        }
    }

    {
        char line[128];
        ShSteam_StatusLine(line, (int)sizeof(line));
        printf("\nstatus line: %s\n", line);
    }

    ShSteam_Shutdown();
    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
