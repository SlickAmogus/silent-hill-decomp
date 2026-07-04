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

## Cheats / items / flags

| Command | Description |
|---|---|
| `give <item>` | Give a weapon/ammo/recovery/story item. See `help give` / `help give 2`. `give allweapons` = all melee + guns + ammo + gas. |
| `kill` | Kill Harry (plays the death animation). |
| `killall` | Kill all enemies within ~50 units of Harry. |
| `spawn list` | List monsters loaded in the current map. |
| `spawn <name> [state]` | Spawn a monster in front of Harry. |
| `unlimited [0\|1]` | Raise the concurrent-enemy cap to the PC max (toggles if no arg). |
| `noclip` | Walk through walls (floor collision stays on). Same as debug key `0`. |
| `getflags` | Show ending flags. |
| `setflag <n> <0\|1>` | Set any event flag by index. |
| `setending <bad\|bad+\|good\|good+>` | Set the ending flags. |
| `clearflags` | Clear event flags. |

## Flashlight & shadows

| Command | Description |
|---|---|
| `shadows [0\|1]` | Real-time flashlight shadows on/off (toggles if no arg). **(saved)** Only visible with the per-pixel flashlight on. |
| `shadowstrength <f>` | How dark shadows get: `1.0` = full/default, lower = softer. |
| `shadowfade <f>` | Contact-fade distance in view units. `0` = off (default, plain hard shadow); `>0` fades a shadow out that many units behind its object (contact-shadow look). |
| `shadownormal <f>` | Self-shadow-acne normal offset. `0` = off (default). |
| `shadowbias <f>` | Depth-compare bias (default `0.0018`). |
| `shadowfpsdrop <f>` | First-person shadow-light drop so FPS shadows aren't self-cancelled. |
| `flashlight <color>` / `fl <color>` | Tint Harry's flashlight (red/green/blue/yellow/cyan/purple/orange/pink/white; `default`/`off` to clear). |
| `worldlight <color>` / `wl <color>` | Same, for the world/ambient light. |
| `flintensity <0..3>` / `flint` | Flashlight cone brightness (FPS mode has its own value). **(saved)** |

## Rendering / graphics tuning

| Command | Description |
|---|---|
| `pgxp [0\|1]` | Perspective-correct textures (PGXP) on/off (WIP). **(saved)** Also F1. |
| `pgxpedge <f>` | PGXP off-screen position clamp, psx-units (higher = less edge warp; default `8192`). |
| `pgxpdepth [0\|1]` | PGXP unquantized-depth W (distance-seam fix). |
| `weld <f>` | PGXP seam-weld radius in px (`0` = off). |
| `weldw <f>` | PGXP weld depth ratio. |
| `vfov <f>` | World vertical FOV scale (`1.0` = off; ~`0.872` matches DuckStation). |
| `hfov <f>` | World horizontal scale, Hor+ only (`1.0` = off; >1 wider, <1 narrower). |
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

**Debug camera** (numpad)

| Key | Action |
|---|---|
| `Num *` | Free debug camera on/off. |
| `Num 2` | Third-person chase cam (mouse look). |
| `Num 8/5/4/6` | Fly forward / back / strafe left / right. |
| `Num 7 / 9` | Turn left / right. |
| `Num + / -` | Tilt up / down. |
| `PgUp / PgDn` | Move up / down. |
| `Num /` | Print camera coordinates to the log. |
| `Num 3` | Reset cam nudge / in-game rescue teleport. |
| `Num 0` | Raw cam mode (zero all nudges). |
| `Num .` | Log Harry position. |

With the debug cam **off**, the same numpad keys nudge the normal game camera
(live camera-tuning aid).

---

## Graphics hotkeys (no dev controls needed)

| Key | Action |
|---|---|
| `F1` | Toggle PGXP. **(saved)** |
| `F2` | Cycle post-process look (Off / CRT / Scanlines / Vignette / Color Grade / Film Grain / Sharpen / PSX Retro / Cinematic). **(saved)** |
| `F3` | Cycle tone mapping (Off / Reinhard / ACES / Filmic). **(saved)** |
| `F4` | Toggle per-pixel flashlight. **(saved)** |
| `[` / `]` | Lower / raise the selected effect's intensity (rebindable `key_gfx_prev` / `key_gfx_next`). |
| `\` | Cycle which enabled effect `[` `]` adjusts (rebindable `key_gfx_cycle`). |

## Other configurable hotkeys

Set in `config.cfg` (defaults shown), active in every camera mode:

| Config key | Default | Action |
|---|---|---|
| `key_quicksave` | `F6` | Quick save. |
| `key_quickload` | `F8` | Quick load. |
| `key_change_cam` | `F9` | Cycle control style (Classic / TPS / OTS). |
| `key_swap_shoulder` | `Mouse3` | Swap the OTS shoulder side. |
| `key_console` | `` ` `` | Console open/close (tap) / type (hold). |

---

## See also

Many of these also exist as `config.cfg` keys and/or launcher options
(resolution, vsync, filtering, PGXP, MSAA, post-process, tone-map, flashlight +
shadows, FPS cap, control style, sensitivities, volumes). The in-game **PC
Options** menu (3 pages) exposes the common ones live.
