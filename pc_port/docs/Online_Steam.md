# Steam sessions

Steam lobbies, friend invites, the overlay, rich presence, and peer-to-peer
that needs no port forwarding.

This is the **second** of the two online systems, and it is separate from the
master server on purpose:

| | master server (`Online.md`) | Steam session |
|---|---|---|
| who | strangers | people you invite |
| joining | type an address | click Join in the overlay |
| needs | someone to run `sh_master` | Steam, and nothing else |
| carries | ghosts, messages, death markers | the co-op link |
| accounts | none | Steam |

They coexist. You can be wandering a master server's fogged town and in a
two-person Steam session at the same time; the two share only the datagram
format.

---

## Getting it running

**1. Put `steam_api64.dll` next to `SilentHillPC.exe`.** It comes from the
Steamworks SDK (`redistributable_bin/win64/steam_api64.dll`). It is Valve's
redistributable, so it is not in this repository and never will be — the port
loads whatever copy you supply, at runtime, and stays dormant when there is
none. Any copy from SDK 1.47 (2020) or newer works.

**2. Turn it on.** In the launcher's **Online** page, or in `config.cfg`:

```
online_steam = 1
online_steam_appid = 480
online_steam_max_players = 4
online_steam_public = 0
online_steam_autohost = 0
```

**3. Check it.** The launcher's **Check Steam** button, or:

```
online_server\steam_probe.exe
```

That loads the DLL, resolves every Steam function the port asks for, creates a
throwaway lobby, sends itself a datagram, and leaves. If something is wrong it
names it.

**4. Host and invite.** In the console: `steam host`, then `steam invite`. The
Steam overlay opens on your friends list. When they accept, their game either
joins immediately or is launched with `+connect_lobby <id>`, which the port
handles either way.

---

## Why app id 480

480 is **Spacewar**, Valve's public test application. Every Steam account owns
it, so an unshipped game can use lobbies, P2P and the overlay without an AppID
of its own. It is what fan ports and recompilation projects do.

The consequences are worth knowing:

- Friends see you playing "Spacewar", not Silent Hill.
- Any Spacewar lobby is visible to any other Spacewar user, which is why the
  default lobby type is **friends only** and why the port stamps
  `game=silenthill-online` into the lobby metadata.
- If the project ever gets a real AppID, `online_steam_appid` is the only line
  that changes.

---

## Console

```
steam                connection, session, and the round-trip time to each member
steam host           open a lobby (friends only unless online_steam_public)
steam invite         Steam overlay friend invite
steam join <id>      join a lobby by id
steam leave
```

`F11` shows the session block under the master-server roster.

---

## How it is built, and why

**No SDK in the tree, no build dependency.** `sh_net_steam.c` resolves
`steam_api64.dll` with `GetProcAddress` and declares the **flat C API** itself.
The Steamworks SDK is not redistributable in a GPL-3 source tree, and a hard
link against it would mean the port stops building for anyone who has not
fetched it. Every failure is soft: no DLL, Steam not running, a DLL too old —
each logs one line and leaves the layer dormant.

**Manual callback dispatch.** The SDK's normal callback mechanism is C++
template machinery that registers itself from a constructor, unreachable from
C. `SteamAPI_ManualDispatch_*` exists for exactly this case, and is what
`ShSteam_RunCallbacks` drives. It is the same route every non-C++ binding
takes.

**One thread.** Steam is called from the network worker and nowhere else. The
game thread asks for things by setting flags the worker services, the same
arrangement the rest of the client already uses.

**The cost, stated plainly.** A handful of Steam struct layouts are asserted
rather than taken from a header. They are all in one block in `sh_net_steam.c`
with their offsets spelled out, only the *front* of each is ever read, and
`steam_probe` verifies every one of them against a real DLL.

| file | job |
|---|---|
| `pc_port/src/net/sh_net_steam.c` | the loader, dispatch, lobbies, invites, presence, P2P |
| `pc_port/src/net/sh_net_session.c` | session lifecycle and the peer link |
| `online_server/steam_probe.c` | validates the above against a real DLL |

---

## What is verified

Against a live Steam client and a current SDK DLL (09.60.44.10), through the
probe and then again inside the running game:

- the DLL loads and all five interfaces resolve — `ISteamUser` v023,
  `ISteamFriends` v018, `ISteamMatchmaking` v009, `ISteamUtils` v010,
  `ISteamNetworkingMessages` v002
- `SteamAPI_InitFlat` with app id 480 succeeds
- identity and persona name come back
- **a real lobby is created on Valve's servers**, ownership and member count
  are right, and metadata round-trips
- **manual dispatch delivers `LobbyCreated_t` and `LobbyEnter_t`** — which is
  what proves the callback struct offsets
- **a P2P datagram round-trips** with the sender identity decoded correctly —
  which is what proves the `SteamNetworkingIdentity` and
  `SteamNetworkingMessage_t` layouts
- rich presence is set
- the same sequence runs on the worker thread inside the game

## What is not

- **Two accounts have never been in one lobby.** Everything above was done with
  one Steam account, so the invite flow, `+connect_lobby`, the member list with
  more than one person in it, and the ping between two machines are all written
  and unexercised.
- **There is no co-op yet.** A session is a lobby with a live link and a
  round-trip measurement on it. Nothing about the game is synchronised. That is
  the next piece of work, and the link is what it plugs into.

## Adding co-op onto this

The session already carries `sh_net_proto.h` datagrams between lobby members on
a dedicated channel, with a message-type range (`SHNET_MSG_S_*`, 0x50 and up)
reserved for it. A co-op message is:

1. a new type in that range in `sh_net_proto.h`
2. a `ShSteam_Send` where `ShSession_Tick` already sends its pings
3. a case in the switch in `ShSession_Receive`

Nothing about the transport, the lobby, the invites or the identity handling
needs to change.

## Expected noise

Steam prints a couple of lines of its own on startup, including
`src\common\pipes.cpp (733) : bExpectedThisThread && ...`. That is Steam's
internal diagnostic around manual dispatch, it appears before any of the port's
own code runs, and everything works through it.
