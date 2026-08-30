# Splitscreen co-op — where it stands, and what it would take

Nothing here is built. This is the design note for the option that was on the
table alongside the online work, written down so the reasoning survives.

## Why online ghosts came first

Duke 3D's splitscreen was mostly viewport plumbing because Build already had
multiplayer in 1996: two player structs, two views, and a network layer all
existed before anyone asked for a second screen.

Silent Hill 1 has none of that heritage. There is exactly one player —
`g_SysWork.playerWork.player`, one `s_SubCharacter` — and everything is written
against it: the camera, collision, combat targeting, doors, triggers, the
inventory, the save format, cutscene staging, map transitions, the flashlight
cone, and the fog anchor. Adding a second **controllable** player is an engine
change, not a rendering change, and it touches the systems most likely to break
in ways that are hard to attribute.

The online ghost framework, by contrast, is strictly additive: one file reads
`g_SysWork` and nothing writes it, so the single-player game is bit-identical
with the feature off. It also builds the protocol, the master server and the
launcher plumbing that real co-op would need later anyway. Splitscreen needs
none of that and would have consumed the session with nothing runnable at the
end.

## What the port already has that makes it reachable

This is the part worth recording, because it is more than it looks:

| piece | what it gives a second player |
|---|---|
| `pc_playas.c` | 14 characters already driven by **Harry's own animation tree**, via `CHARA_FILE_INFOS` retargeting. Player 2 can be Cybil without new animation data. |
| `pc_chara_pool.c` | any character resident in any map, with its textures in dedicated pool slots that survive map loads |
| `g_SysWork.npcs[]` | an array of the same `s_SubCharacter` type the player is. A slot fed pad input instead of AI is a second body with working collision, damage and rendering. |
| `DebugCamera` / alt cams | a second view matrix is already something the port knows how to build and drive |
| per-scheme control binds | `ControlScheme` is already duplicated (classic / altcam); a second pad's binds are the same shape |
| enemy cap 6 → 32 | the per-room NPC budget already has headroom for a persistent extra body |

## The four real problems

**1. One camera, one world-screen matrix.** `GsWSMATRIX` is global, and
everything that projects — geometry, characters, the decals, the online
ghosts — reads it. Two views means building the OT twice per frame with a
different matrix, or building one OT and drawing it twice, which does not work
because screen coordinates are baked at submit time. It is the first, and it
doubles the GTE work and the packet arena usage. The arena is 2MB and the
whole-town mode already budgets 12MB against it (`PC_WM_PACKET_BUDGET`), so
this needs a real accounting pass, not an optimistic one.

**2. Single-player-scoped systems.** Inventory, the save format, item pickups,
door triggers, the map screen, and the radio are all written against "the
player". Each needs a decision: shared (one inventory between both players),
or duplicated (which changes the save format). Shared is far less work and is
probably the better game.

**3. Cutscenes and map transitions.** DMS scenes stage the player at authored
positions. A second player standing in shot has to be hidden, parked, or
staged too. Transitions need a rule for what happens when one player walks
through a door: both move, or the second is pulled along.

**4. The flashlight and fog.** Both are anchored on the player position and
feed the shader as single values. Two light sources is a shader change; two fog
anchors is not meaningful, so the fog would follow one player or the midpoint.

## A phased path, if it gets picked up

1. **Second body, one view.** An NPC slot driven by pad 2, wearing a play-as
   skin, sharing the existing camera. Not co-op yet, but it proves the body,
   the animation retarget, the collision and the damage path in one step, and
   it is testable on one screen.
2. **Second camera, one view.** Add the second camera work and let a key swap
   which player the single view follows. This is where the `GsWSMATRIX`
   assumptions get flushed out with nothing else changing.
3. **Two viewports.** Build the OT twice, `glViewport` each half. The packet
   arena accounting lands here.
4. **The scoped systems**, in the order they hurt: transitions, then cutscene
   staging, then inventory.

Steps 1 and 2 are worth doing on their own even if 3 never happens: a
second controllable body is what a "possess an NPC" mode or a real online co-op
would also need.

## Which branch

**`xbox-port`**, as originally suggested. Its fixed target makes the viewport
and performance work a bounded problem — one GPU, one resolution, one
controller topology — where on PC every one of those is a variable. Once it
works there, the PC branch inherits the engine changes and only has to deal
with arbitrary aspect ratios.
