# Silent Hill Online — what works, and what comes next

## The two systems, and why they are different

**The living world.** Ghosts, messages and death markers from strangers, over a
UDP master server. It is a *picture* of somebody else's game painted into
yours: nothing they do reaches you and nothing you do reaches them. It is meant
to be **seamless** — you never connect to it, you are just in it. There is no
lobby, no waiting, and no banner unless something is genuinely broken and stays
broken.

**A co-op session.** Steam lobbies, friend invites, P2P over Valve's relay.
Another player is *in* your world, or you are in theirs. The moment that is
true, your local clock is shared and the game may no longer be stopped.

`g_ShNetCoopActive` is the one flag that separates them. The ghost world must
never set it, however many ghosts are on screen.

---

## What works today

| | state |
|---|---|
| Master server, ghosts, memos, death markers | **working**, two real clients verified |
| Ghost outlines, fogged and wall-occluded | **working** |
| **Chat** -- global (whole server) + game (same map) | **working**, relay + scoping verified |
| Modeled ghosts (real character, opt-in) | **experimental**, runs + stable; fidelity needs two machines |
| Templated messages (`M`), reading them | working, untested with a second player |
| Player list, event feed (`F11`) | working |
| Console no longer pauses a live world | **working** |
| Steam init, identity, rich presence | **working**, verified on live Steam |
| Steam lobby create / join / leave / metadata | **working**, real lobby on Valve's servers |
| Steam invites, `+connect_lobby` | written, **needs two accounts to prove** |
| Steam P2P send/receive | **working**, loopback round-trip verified |
| Session member list + per-member ping | written, **needs two accounts to prove** |
| Co-op flag blocks pausing | mechanism **working**, no gameplay trigger yet |
| Two players sharing one world | **not started** |

---

## Distributing a test build: what a player needs

The test build is the ordinary game plus the online code. Two ways to play with
someone, and they are independent.

### The master server (the persistent "lobby" world -- ghosts + chat)

This is the one to test first. One person runs `sh_master`; everyone else points
`online_server` at that machine.

- **The HOST forwards one UDP port** (default 27888) to the machine running
  `sh_master`, or everyone is on the same LAN / the same VPN (Hamachi, Tailscale,
  ZeroTier all work -- then use the VPN IP and no forwarding at all).
- **Players forward nothing.** They set `online_server = <host ip>` and play.
- Everyone connected sees each other's ghosts on the same map, can leave
  messages, and can chat: **global** reaches everyone on the server, **game**
  reaches everyone on your map.
- This is the "overarching world everyone connects to" idea. It is one server;
  everyone on it is in the same world.

### Steam (friends, no forwarding)

- **No port forwarding, ever** -- Valve's relay does the NAT traversal.
- **No master server needed.** Steam lobbies + invites are their own thing.
- Needs `steam_api64.dll` beside the exe and Steam running. `online_steam = 1`.
- Today this gives you a lobby, the overlay invite, and a live peer link between
  members. It does NOT yet carry ghosts or gameplay -- that is the co-op work
  below. So for THIS release, ghosts + chat come from the master server; Steam
  is the plumbing co-op will use.

### So do I need a master server with Steam?

For **ghosts and chat right now: yes**, run a master server -- that is what
carries them. Steam alone is the co-op transport and co-op is not built yet. The
end state is: the master server stays the ambient world everyone shares, and
Steam sessions layer real co-op on top for friends who want to be in one game.

---

## Next, in order

### 1. Modeled ghosts: finish them

**Where they are:** implemented and opt-in (`online_ghost_model`), off by
default. A ghost draws as the real character model at its world position, posed
to the sender's own keyframe, through the engine's own actor-draw path. It runs
and is stable; what has not been seen is a cleanly separated ghost on two
machines with real movement.

**What is left:** confirm the pose and placement read right at a distance, then
promote it to the default. Two known gaps to close after that: a Harry ghost
uses Harry's base ANM, so a map-specific animation (a scripted pose) will not
map cleanly and clamps to the nearest base keyframe; and the model does not yet
carry the flashlight or held weapon. Neither blocks the visual.

### 2. The world handshake

Two session members agree they are in one world. A `SHNET_MSG_S_JOINWORLD` /
ack pair in the range already reserved, and then
`ShNet_SetCoopActive(1, "guest joined")` — the one line `sh_net_session.c`
already marks the spot for. Pause suppression is built and waiting for it.

At this point two friends can be in each other's games, see each other properly
(step 1), and neither can freeze the other. Nothing is synchronised yet, which
is fine and is worth playing with.

### 3. What "shared" means

The first real design decision, and it should be made with steps 1 and 2 in
front of you rather than now:

- **Host authority over enemies.** The host simulates; the guest receives
  positions and states. This is what makes it co-op rather than two people
  walking through parallel houses.
- **Doors, pickups and triggers.** Shared or per-player. Shared is more
  interesting and much harder.
- **Map transitions.** Both move, or the guest is pulled along.
- **Cutscenes.** Staged for one player; the second has to be hidden, parked,
  or staged too.
- **Inventory, saves.** Almost certainly the host's, with the guest as a
  visitor.

### 4. A second controllable body

Needed by real co-op and by splitscreen alike, and the reason
`Splitscreen_Coop.md` exists: an NPC slot fed pad input instead of AI is a
second body with working collision, damage and rendering. Steps 1 to 3 do not
need it — a remote player rendered from network state is not a local
`s_SubCharacter` — but the moment anything the guest does must affect the
host's simulation, it does.

### Later, and deliberately vague

**Invasions.** Joining someone's world as a creature or a boss. This is step 3
with a hostile guest and a different body, and it is genuinely the most
interesting thing on this list — but it needs host authority over enemies to
exist first, because an invader IS an enemy the host's world has to simulate
against.

---

## Testing recipes

Everything below runs on one machine.

### The living world, with two players

```
pc_port\build\online_server\sh_master.exe -n "The Fog" -v
```

Set `skip_intros = 2` in `config.cfg` (both instances need to be IN GAME — a
client sitting on the main menu publishes no position, which is correct and is
why an earlier test looked dead). Then launch `SilentHillPC.exe` twice.

Each log should say:

```
[NET] connected to "The Fog" as id 1 (tick 100ms, flags 0xF)
[NET] ghost texture generated (64x128, slot 509, clut 0x9C3D)
[NET] player 2 appeared on this map at (-7.800, 160.500) - 1 ghost(s) here
```

Then, in either window: press **Y**, type, **Enter** -- the line appears in
both. **U** switches the channel between global and game. To try modeled
ghosts, open the console (`~`) and run `net model 1` in both, or set
`online_ghost_model = 1` before launching.

### The protocol, without the game

```
pc_port\build\online_server\sh_master.exe -p 27899 -d selftest.db
pc_port\build\online_server\shnet_selftest.exe 127.0.0.1 27899
```

Two synthetic players: handshake, ping, position fan-out with map isolation,
roster paging, marker round-trip, death de-duplication, and the security cases.
Expect `PASSED (0 failures)`. If a change to `sh_net_proto.h`,
`sh_net_client.c` or `sh_master.c` does not keep this passing, the change is
wrong.

### Steam, without the game

```
pc_port\build\online_server\steam_probe.exe
```

Needs `steam_api64.dll` beside it and Steam running. Loads the DLL, resolves
every Steam function the port asks for, creates a real lobby, sends itself a
datagram, and leaves. Names whatever did not work. The launcher's **Check
Steam** button runs this and shows the output.

### The generated art

```
pc_port\build\online_server\ghost_art_dump.exe
```

Writes `contact.png` — the silhouette at the sizes it is actually drawn at,
over fog and over a dark interior. Look at it.

### Co-op pause suppression

In the console (`~`):

```
net coop 1
```

Press the pause button: nothing happens, and the log says

```
[COOP] another player is in this world (console) - pausing is blocked
```

`net coop 0` gives it back. Pausing first and *then* running `net coop 1` also
works — the paused state leaves itself.
