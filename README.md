MOVED TO ITS OWN REPO: https://github.com/SlickAmogus/silenthill-online/
# Silent Hill Online

The multiplayer branch of the Silent Hill native PC port. It adds online play on top of the regular port without changing how single-player works: start a normal game and nothing here touches it. This page assumes you already know the base port (installation, options, controls) and covers only what is online.

There are two online systems, and they are independent:

- **Co-op over Steam** is the active focus and the part you can play today. You host a session, invite a friend through Steam, and play the campaign together. No port forwarding, no IP address, no master server.
- **The living world** is the ambient system: outlines of other players and the messages they leave, served by a shared master server. It is being folded into this branch and is configured in the launcher, but it is not the co-op mode.

Project website: https://sh1pc.com/ <br/>
Discord: https://discord.gg/JWuNzVsQbr

## Playing co-op (Steam)

You need `steam_api64.dll` next to `SilentHillPC.exe`, and Steam running and signed in. Everyone in the session runs the same build, and each player supplies their own game disc, the same as single-player.

Hosting and joining happen from a **Multiplayer** entry on the main menu (also opened with **M**), drawn in the same clean UI as the quick menu and achievements:

- **Host Game** sets the session up: 2 to 4 players, private (friends only) or public, an optional 30 or 60 FPS lock, and whether to start a fresh New Game or load a co-op save. **Open Lobby** creates the Steam lobby, **Invite Friend** opens the Steam overlay, and **Start Game** drops you into the world and pulls the guests in behind you.
- **Join** is driven by Steam: accept a friend's invite in the overlay and you are put into their lobby. When the host starts, you boot into their map on your own.

Co-op has its own save system, separate from your single-player saves. Files live in `gamedata/coopsaves/` under your own names.

### In a co-op game

- **M** opens the multiplayer menu over the live game: Resume, Save, Save & Exit, Nameplates, Players, Leave to Title. It does not freeze the world.
- **The game never pauses.** Pausing a world someone else is standing in is the one thing co-op must not do, so the pause button is ignored and the quick menu is replaced by the M menu. The in-game console still works, and does not pause; `status` reports the session (members, ping, and state).
- You see the other players in the world as their real character, moving live, with a nameplate above them.

## What works now, and what is still coming

Co-op is being built in slices.

**Working**
- See each other in the same world as your own character, posed to how you are actually moving.
- Guests boot into the host's map automatically when the host starts, and follow the host's map changes.
- **Shared item pickups.** When anyone picks something up, everyone gets it, each into their own inventory. Key items are shared, and ammo gives every player that many rounds without pooling.

**In progress**
- Host-authoritative enemy sync (today each player has their own monsters).
- Shared world progress: doors, puzzles and event flags (only picked-up items transfer so far).
- No-pause inventory: the item screen keeping the world live and dangerous underneath it.
- A pregame lobby screen with music and the minimap.
- Co-op text chat (the chat box currently runs on the living-world server, not the Steam link).
- Folding the living world in as a second mode alongside co-op.

## Online settings (launcher)

The launcher's **Online** button opens the online settings. Co-op comes up on its own, so the useful options there are your **name** (it floats over your head to the others), a **nameplates** toggle, and the Steam host defaults: max players, public or private, App ID, and a **Check Steam** button that verifies `steam_api64.dll`, lobbies and invites actually work. The living-world and master-server options in the same window belong to that system and are marked as coming. Everything is written as `online_*` keys in `config.cfg`.

The default Steam App ID is **480** (Spacewar, Valve's public test app), which every Steam account owns, so the port can use lobbies and invites without an App ID of its own.

## Building

Same as the base port: MSYS2 / MinGW64, CMake + Ninja, and the PsyCross submodule. See the port's own build steps for the full procedure. The only online-specific note is that the Steam and networking layer come up automatically in this build, and `steam_api64.dll` (Steamworks SDK, `redistributable_bin/win64`) must sit next to the game exe for co-op to work. It fails gracefully without it, and the Multiplayer menu simply reports Steam as unavailable.

<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
