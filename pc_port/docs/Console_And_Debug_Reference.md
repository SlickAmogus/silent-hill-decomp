# Console & Debug Reference (PC Port)

A reference for the in-game developer console commands, cheat/debug keys, and the
graphics hotkeys. Source of truth: `pc_port/src/pc_console_cmd.c` (commands),
`pc_port/src/dbg_overlay.c` (console input + F-keys), and
`src/bodyprog/sys/game_main.c` (`DebugCamera_Update`, debug/cheat keys).

Most debug features require `allow_debug_controls = 1` in `config.cfg` (or the
launcher's dev-controls checkbox). The graphics hotkeys (F1–F4, `[ ] \`) are
user-facing and work regardless. Some console commands persist to `config.cfg`;
those are marked **(saved)**. Everything else is a live, session-only change.

---

## Opening the console

- **`` ` ``** (backtick / tilde, rebindable via `key_console`) — **tap** to
  open/close the console, **hold** to type a command. Requires
  `allow_debug_controls = 1`.
- Commands are case-insensitive; they're shown here in lowercase.
- While the console is typing, the game **freezes** (world time is zeroed) so the
  scene pauses on the exact view you were looking at.
- The console minus key types `_`; commands that take a negative number accept a
  leading `_` as `-` (e.g. `invcary _10`).

Type `help`, `help give`, `help give 2`, `debug`, or `debug 2` in-game for the
built-in quick lists.

---

## Session / navigation

| Command | Description |
|---|---|
| `help [give]` | Command list (or the give-item list; `help give 2` for page 2). |
| `debug [2]` | Debug/cheat key reference (page 1 = cheats, page 2 = camera). |
| `quit` | Exit the game immediately. |
| `map` | List all map names. |
| `map <name>` | Set the New-Game start map (loads on next New Game; doesn't warp mid-session). |
| `about` / `credits` | PC port credits: build id plus the same block the staff roll appends. |
| `pccredits [0\|1]` | Append the PC Port Credits block to the end of the staff roll (toggles if no arg). The roll's scroll rate is its length divided by its line count, so the extra lines tighten the step slightly and the roll still ends on the music. Config key `pc_port_credits`. **(saved)** |

## Selecting things in the scene

With the console open the pointer is live over the picture. **Left-click a character**
below the console panel to select it; **right-click** anywhere to deselect. Both print
to the console, and a selection carries the NPC slot so you can tell two of the same
monster apart:

```
selected npc[3] GROANER  hp 1.0  pos (12.3, 0.0, -4.5)  scale 1.00
deselected npc[3] GROANER
```

The panel covers the top of the screen, so **hold TAB** to hide it while keeping the
pointer live: the whole picture becomes clickable, and releasing TAB brings the panel
back exactly as it was.

Characters and **props** can both be picked. A prop here means a world object: the map, flashlight, radio and knife pickups, map-event objects and cutscene props. Furniture baked into the room mesh is not a separate object to the engine, so it cannot be picked individually. Only things the game is
currently drawing are pickable, and the nearest one wins
when two overlap; a character beats a prop at the same depth. Harry is pickable like anything else.

| Command | Description |
|---|---|
| `select` | Show the current selection. |
| `select clear` | Drop it (same as right-click). |
| `select player` | Select Harry without clicking, for the cameras that hide him. |
| `select nearest` | Select the nearest live enemy to Harry. |
| `scale <f>` | Resize the selected character or prop, `0.05`..`20`. Bare `scale` reports the current value. For a character the hit volume scales with it, so a giant Groaner is hittable over the body you can see and is blocked by walls at its own size. The model grows about its root bone rather than its feet. Per NPC slot, forgotten when that slot is recycled into a different monster; props are remembered by placement, up to 32 at a time. |
| `health [n]` | Show the selection's health, or set it to `n`. With nothing selected it acts on Harry (capped at 100). Use `kill` rather than `health 0`. |
| `heal` | Refill the selection's health: Harry to 100, an enemy to the highest health it has had since it appeared (enemies set their own health on their first frame and have no max-health field). |
| `info` | Name, character id, health, position, facing, scale, and for an enemy its AI state and whether it is frozen. Harry when nothing is selected; a prop reports its placement and scale. |
| `goto` | Move Harry next to the selected enemy or prop, on his side of it and facing it, dropped onto the floor there. |
| `bring` | Move the selected enemy two units in front of Harry, facing him. Props cannot be moved. Unfreeze a frozen enemy first: it keeps drawing its last pose until it runs again. |
| `freeze [0\|1]` | Hold the selected enemy still: its AI does not run, so it neither moves nor attacks, and damage dealt to it lands when it is unfrozen (`kill` unfreezes it). Bare `freeze` toggles. `freeze all` / `freeze none` act on every enemy in the room; `unfreeze` is `freeze 0`. |
| `nocol [0\|1]` / `nocollide` | Take the selection out of collision except for the ground: it walks through walls and through other characters, they walk through it, and attacks pass straight through. The ground still holds it up, so it does not fall through floors. Bare `nocol` toggles, and with nothing selected it acts on Harry. Walking outside the map leaves the area with no collision data under you, where Harry drops as he would off any edge, so come back the way you went. |

## Custom key binds

`bind` runs console commands from a key. It is a **separate system** from the control
binds in Options: a key already driving Harry can carry a bind, it is the same key in
every camera mode, and editing or clearing the control binds never touches it.

```
bind k kill
bind f give shotgun;give shotgunammo;spawn groaner;spawn groaner
```

Commands are separated by `;` and run in order on the key press. Binds are written to
`config.cfg` in their own section, as the lines you typed, so a set can be copied out,
pasted into a message, and pasted back:

```
# --- Custom key binds (console: bind / unbind / unbindall) ---
bind K KILL
bind F GIVE SHOTGUN;SPAWN GROANER
```

| Command | Description |
|---|---|
| `bind` | Usage. |
| `bind <key> <cmd>[;<cmd>...]` | Bind a key, replacing any bind already on it. **(saved)** |
| `bind list` | Show every bind. |
| `unbind <key>` | Clear one. **(saved)** |
| `unbindall` | Clear them all. **(saved)** |

The command list may be wrapped in quotes if you prefer (`bind k "kill;spawn groaner"`); they are stripped. The console types `; : ' " , < / ? [ { ] } \ |` as well as letters, digits, space, `-`, `=` and `.`, so the separator can actually be entered.

Refused: the console key, Escape, Enter, Backspace, Tab and the modifiers, and a bind
may not contain `bind`/`unbind` (or pressing a key could silently rewrite your other
binds). Up to 48 binds. They need `allow_debug_controls` on, same as the console, so a
shared config cannot hand someone cheats they never switched on; `bind list` says so
when it is off.

## Cheats / items / flags

| Command | Description |
|---|---|
| `give <item>` | Give a weapon/ammo/recovery/story item. See `help give` / `help give 2`. `give allweapons` = all melee + guns + ammo + gas. |
| `kill` | Kill the current selection: a selected enemy takes lethal damage and runs its own death path. With nothing selected (or Harry selected) it kills Harry, as before. |
| `killall` | Kill all enemies within ~50 units of Harry. |
| `spawn list` | List monsters loaded in the current map. |
| `spawn <name> [state]` | Spawn a monster in front of Harry. |
| `unlimited [0\|1]` | Raise the concurrent-enemy cap to the PC max (toggles if no arg). |
| `noclip` | Walk through walls (floor collision stays on). |
| `infammo [0\|1]` | Fire without spending ammo, so clips never empty and guns never need reloading. Affects only the guns you already carry: it suppresses the ammo decrement rather than granting rounds, so nothing is ever added to or removed from your inventory. The automatic reload-on-empty is held off while it is on, since that is the one path that would move rounds out of the inventory. Off restores normal ammo use exactly. Also a Quick Options > Cheats row. |
| `notarget [0\|1]` | Enemies ignore Harry. |
| `freecam [0\|1]` | Free camera: mouse look, W/A/S/D, Space/C up/down, Shift fast, Ctrl slow. |
| `collvis [0\|1]` | Collision visualizer panel. |
| `fastforward [0\|1]` / `ff` | Speed the game up (the sticky toggle, not the Ctrl+F5 hold). |
| `wireframe [0\|1]` | Wireframe rendering. |
| `notex [0\|1]` | Disable textures. |
| `getflags` | Show ending flags. |
| `setflag <n> <0\|1>` | Set any event flag by index. |
| `setending <bad\|bad+\|good\|good+>` | Set the ending flags. |
| `clearflags` | Clear event flags. |

## Flashlight & shadows

| Command | Description |
|---|---|
| `flmode <0-3\|classic\|classicshadows\|modern\|modernshadows>` | Flashlight mode: Classic (PSX per-vertex), Classic + Shadows (per-pixel PSX-calibrated + shadows), Modern (stylized per-pixel spotlight), Modern + Shadows. Switching styles swaps the beam intensity/size between the styles' calibrated defaults unless customized. **(saved)** |
| `shadows [0\|1]` | Real-time flashlight shadows on/off within the current mode (Classic + Shadows without shadows is just Classic, so this moves between modes 1<->0 / 3<->2; toggles if no arg). **(saved)** |
| `shadowstrength <f>` | How dark shadows get: `1.0` = full/default, lower = softer. |
| `shadowfade <f>` | Contact-fade distance in view units. `0` = off (default, plain hard shadow); `>0` fades a shadow out that many units behind its object (contact-shadow look). |
| `shadownormal <f>` | Self-shadow-acne receiver offset (moves the sample toward the light along the light ray). `0` = off (default; receiver-plane depth correction handles most acne). |
| `shadowbias <f>` | Depth-compare bias (default `0.0018`). |
| `shadowfpsdrop <f>` | First-person shadow-light drop so FPS shadows aren't self-cancelled. |
| `flashlight <color>` / `fl <color>` | Tint Harry's flashlight (red/green/blue/yellow/cyan/purple/orange/pink/white; `default`/`off` to clear). |
| `worldlight <color>` / `wl <color>` | Same, for the world/ambient light. |
| `flintensity <0..3>` / `flint` | Flashlight cone brightness (FPS mode has its own value; per-style defaults: Modern 2.10, Classic + Shadows 1.20). **(saved)** |

## Rendering / graphics tuning

| Command | Description |
|---|---|
| `pgxp [0\|1]` | Perspective-correct textures (PGXP) on/off (WIP). **(saved)** Also F1. |
| `pgxpedge <f>` | PGXP off-screen position clamp, psx-units (higher = less edge warp; default `8192`). |
| `pgxpdepth [0\|1]` | PGXP unquantized-depth W (distance-seam fix). |
| `weld <f>` | PGXP seam-weld radius in px (`0` = off). |
| `weldw <f>` | PGXP weld depth ratio. |
| `vfov <f>` | World vertical FOV scale (`1.0` = off; ~`0.872` matches DuckStation). Config key `world_vscale`; the Quick Options *View & Aspect* page saves it. |
| `hfov <f>` | World horizontal scale, Hor+ only (`1.0` = off; >1 wider, <1 narrower). Config key `world_hscale`; same page saves it. |
| `crtaspect <f>` | Trim on the 4:3 CRT picture, `0.5`..`1.5`. Below 1.0 = taller, thinner figures. Default `0.9`. Config key `crt_aspect_trim`; same page saves it. |
| `par <f>` | Pixel aspect, read by `display_aspect = raw` only (`35/32` = 1.09375). Config key `pixel_aspect`; same page saves it. |
| `vshift <f>` | World vertical view shift, psx-units (+ = view up; `0` = off). |
| `msgshift <n>` | Message-box up-shift, psx-units. |
| `bary <n>` | Cutscene letterbox bar Y (raise until bars hit the screen edges). |
| `fogstr <f>` | World fog density (`1.0` = native PC fog; >1 deepens toward the PSX look). |
| `alpha [0\|1]` | Slope-alpha invisible-wall fix (`1` = capped/fixed, `0` = original). |
| `add <0\|1\|2>` | Debug the additive render layer (`0` = skip, `1` = normal, `2` = depth-tested). |
| `postintensity <0..1>` / `postint` | Post-process effect mix. **(saved)** |
| `tmintensity <0..1>` / `tmint` | Tone-map mix. **(saved)** |

## Inventory presentation

| Command | Description |
|---|---|
| `invaspect [0\|1]` | Inventory item proportions: PSX-faithful vs square (true). |
| `invscale <50..200>` | Inventory item vertical scale (% of square; default `125`). |
| `textsize <100..150>` | Size of subtitles, memos and other in-game messages, in percent (saved as `text_size`; also Options > HUD). Menus keep their size; a message that would leave the screen is drawn as large as fits. |
| `invcary <n>` | Carousel item Y offset (+ down). |
| `inveqy <n>` | Equipped item Y offset (+ down). |
| `invdim <0..100>` | Off-center carousel dim strength (%). |

## Collision / movement

| Command | Description |
|---|---|
| `obst <0\|1>` | Round-obstacle (ptr_18) collision on/off (`0` = sprint-through). |
| `collscope <0\|1>` | Preload-collision cell scope (`1` = vanilla window, `0` = all chunks). |

## Audio / animation / FMV

| Command | Description |
|---|---|
| `xavolume <0..100>` / `xavol` | XA (FMV/voice) volume. **(saved)** |
| `adsr [0\|1]` | SPU ADSR envelope for looping-SFX ring-out (WIP). |
| `kf [n]` / `keyframe [n]` | Animation keyframe inspector: set/show frame (`K` toggles view, `,` `.` scrub). |
| `fmv` | List FMV movies (numbered). |
| `fmv <name\|#>` | Play a movie (also aliases `intro1-2`, `end1-5`). |

---

## Debug & cheat keys

Require `allow_debug_controls = 1`. (In-game references: `debug` / `debug 2`.)

**Cheats & tools**

| Key | Action |
|---|---|
| `Esc` | Warm reset to the title screen. (Works without dev controls.) |
| `0` | Noclip toggle (walk through walls). |
| `4` / `5` | New-Game start-map prev / next. |
| `6` | Kill nearby enemies. |
| `7` | Invincibility toggle. |
| `8` | +15 handgun bullets. |
| `9` | No-target toggle (enemies ignore Harry). |
| `-` | Give Hunting Rifle + 30 shells. |
| `=` | Give Shotgun + 30 shells. |
| `'` | Collision visualizer panel. |
| `K` / `,` `.` | Keyframe inspector; scrub (hold = faster). |
| `L` | Log the FPS-camera eye offset (for baking `g_PcFpsOffset`). |

**Free camera** (also the quick options menu's Cheats page, no dev controls needed there)

| Key | Action |
|---|---|
| `Num *` | Free camera on/off. Harry stays where he was; his input is ignored while it is on. |
| Mouse | Look (mouse sensitivity / invert follow the control settings). |
| `W/A/S/D` | Fly forward / left / back / right, along the view direction. |
| `Space` / `C` | Move up / down. |
| `Shift` / `Ctrl` | Fast / slow. |
| `Num 2` | Third-person chase cam (mouse look). |
| `Num 3` | Reset cam nudge / in-game rescue teleport. |
| `Num 0` | Raw cam mode (zero all nudges). |
| `Num .` | Log Harry position. |

With the free camera **off**, the numpad keys nudge the normal game camera
(live camera-tuning aid).

---

## Graphics hotkeys (no dev controls needed)

| Key | Action |
|---|---|
| `F1` | Toggle PGXP. **(saved)** |
| `F2` | Cycle post-process look (Off / CRT / Scanlines / Vignette / Color Grade / Film Grain / Sharpen / PSX Retro / Cinematic). **(saved)** |
| `F3` | Cycle tone mapping (Off / Reinhard / ACES / Filmic). **(saved)** |
| `F4` | Cycle flashlight mode: Classic -> Classic + Shadows -> Modern -> Modern + Shadows. **(saved)** |
| `[` / `]` | Lower / raise the selected effect's intensity (rebindable `key_gfx_prev` / `key_gfx_next`). |
| `\` | Cycle which enabled effect `[` `]` adjusts (rebindable `key_gfx_cycle`). |

## Other configurable hotkeys

Set in `config.cfg` (defaults shown), active in every camera mode:

| Config key | Default | Action |
|---|---|---|
| `key_quicksave` | `F6` | Quick save. |
| `key_quickload` | `F8` | Quick load. |
| `key_change_cam` | `F9` | Cycle control style (Classic / TPS / OTS). |
| `key_quick_options` | `F10` | Quick options overlay (in-game): live graphics / HUD / audio settings. |
| `key_swap_shoulder` | `Mouse3` | Swap the OTS shoulder side. |
| `key_console` | `` ` `` | Console open/close (tap) / type (hold). |

---

## See also

Many of these also exist as `config.cfg` keys and/or launcher options
(resolution, vsync, filtering, PGXP, MSAA, post-process, tone-map, flashlight +
shadows, FPS cap, control style, sensitivities, volumes). The in-game **PC
Options** menu (5 pages) exposes the common ones live, and the **F10 quick
options overlay** exposes the most-used ones without leaving gameplay.
