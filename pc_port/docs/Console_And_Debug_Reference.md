# Console, controls, and debug reference

> **Status — Current guide.** This is the canonical operational reference for the current PC port. See the [feature catalog](../../features.md) for capability/settings status and the [documentation index](README.md) for supporting and historical records.

**Status:** **I** = implemented; **E** = experimental, partial, or restricted; **D** = disabled or superseded.

## Windows command line

| Status | Syntax | Effect |
|---|---|---|
| I | `SilentHillPC.exe -data <directory>` | Change only the directory searched for disc images. A direct image path does not work; config, mods, logs, and saves remain relative to the working directory. |
| I | `SilentHillPC.exe -h` / `--help` | Print usage and exit. |
| D | Other arguments | Unknown options and a trailing `-data` are ignored. |

## Normal controls

Classic is the default fixed-camera/tank style. TPS, OTS, and FPS are experimental and share the alternate scheme. Menus temporarily use Classic bindings. Primary/secondary keyboard-or-mouse and controller actions are configurable; movement comes from the selected stick/D-pad source and is not action-rebindable.

| Action | Classic keyboard / pad | TPS, OTS, FPS keyboard / pad |
|---|---|---|
| Move forward/back | Up/Down · stick/D-pad | W/S · stick/D-pad |
| Turn left/right | Left/Right · stick/D-pad | Left/Right · stick/D-pad |
| Strafe left/right | A/D · LB/RB | A/D · unbound |
| Camera look | Scene-controlled | Mouse motion · right stick |
| Action/fire (`Cross`) | C · A | Mouse1 or E · RT or A |
| Flashlight (`Circle`) | V · B | F · B |
| Map (`Triangle`) | Z · Y | Tab · Y |
| Run (`Square`) | X · X | Left Shift · LB |
| View (`L2`) | Right Shift · LT | unbound |
| Aim (`R2`) | Left Shift · RT | Mouse2 · LT |
| L3 / R3 | unbound · LS/RS | unbound · LS/RS |
| Start / Select | Return/Space · Start/Back | Return/Space · Start/Back |

`control_2d = 1` changes every style except FPS to screen-relative movement; it is experimental and defaults off. TPS/OTS/FPS capture the pointer for look during gameplay, then release it in menus, the console, and cursor puzzles. Full binding details are in [controls.md](controls.md).

### Global controls

| Status | Default | Gate/state | Behavior |
|---|---|---|---|
| I | Escape | Always; rebindable as `key_exit_game` | Quit at the title; otherwise warm-reset to it. Suppressed during console input. |
| I | F6 / F8 | Settled gameplay; rebindable | Open the original save/load screens through their normal flows. These are not snapshot saves. |
| I | F9 / right-stick click | Settled gameplay; rebindable | Cycle and save Classic → TPS → OTS → FPS. No debug gate. |
| I | Mouse3 | OTS gameplay; rebindable | Swap shoulder side for the session. |
| I | Backspace | Settled gameplay; fixed | Toggle the alternate-style crosshair setting for the session. |
| I | Console key (`` ` ``) | `allow_debug_controls = 1`; rebindable | One press opens overlay and input; the next closes both. |
| I | Return, Escape, Space, or pad skip | FMV; fixed | Skip after all skip inputs have first been released. |

### Graphics controls

These are user controls and do not require the debug gate.

| Status | Default | Behavior |
|---|---|---|
| E | F1 | Toggle/save runtime PGXP; default off. |
| I | F2 | Cycle/save Off, CRT, Scanlines, Vignette, Color Grade, Film Grain, Sharpen, PSX Retro, Cinematic. |
| I | F3 | Cycle/save Off, Reinhard, ACES, Filmic tone mapping. |
| I | F4 | Cycle/save Classic, Classic+Shadows, Modern, Modern+Shadows flashlight modes. |
| I | `\` | Rebindable: select the next enabled flashlight intensity, post mix, tone mix, or flashlight size. FPS uses separate beam values. |
| I | `[` / `]` | Rebindable: lower/raise the selected value by `0.05`; hold to repeat, save on release. |

## Console operation

Set `allow_debug_controls = 1`; it defaults off. `key_console` defaults to backtick. A single press opens the overlay with the prompt active and normally freezes world time and controller input; the next press closes and unfreezes it. There is no hold/tap split.

`show_console` controls only the external console window: `1` or `3` shows it; `0` or legacy `2` does not. It neither opens nor gates the overlay.

- Enter runs a command, clears the prompt, unfreezes for a 175 ms apply window, then returns to frozen input without closing the overlay.
- Up/Down browse eight history entries. Backspace repeats while held.
- Page Up/Page Down or the mouse wheel moves through 256-line scrollback; End returns to the newest output.
- Drag with Mouse1 to select; Ctrl+C copies the selection and Ctrl+V appends the clipboard's first line. Letters are normalized to uppercase.
- The input accepts spaces, `-`/`_`, `=`/`+`, and decimal points. Arguments shown in brackets are optional.

The built-in `help` page is abbreviated. The built-in `debug` pages contain stale Num2, Num0, Num/, normal-camera-nudge, and bracket-marker lines; use this guide instead.

## Console command catalog

The dispatcher accepts exactly **55 canonical commands plus 7 aliases: 62 unique names**. Each canonical name has one row; aliases share its row. **Config** writes `config.cfg` immediately. **Session** does not. Live inventory/event changes can enter a later save if the player saves normally.

### Session, maps, flags, player, and NPCs

| Status | Canonical command | Alias | Syntax and behavior | Persistence |
|---|---|---|---|---|
| I | `quit` | — | Exit immediately. | — |
| I | `help` | — | `help` or `help give [2]`: show the abbreviated command list or two item pages. | Session |
| I | `getflags` | — | Report ending flags, including Cybil `449` and Good path `391`. | Session |
| I | `setflag` | — | `setflag <0..1663> <0\|1>`: change a live event flag and remember up to 24 entries for reapplication after a New Game reset. | Live save/session |
| I | `setending` | — | `setending <bad\|bad+\|badplus\|good\|good+\|goodplus>`: set flags `449` and `391`; use before the ending trigger. | Live save/session |
| I | `clearflags` | — | Forget only pending console-set flags. Live flags remain; load a save to reset them. | Session |
| I | `debug` | — | `debug [2]`: show the stale built-in key pages; use this guide for current bindings. | Session |
| I | `map` | — | `map`: list 43 registry names. `map <name>` selects the next-New-Game start map for this run; it never warps the active game. Missing modules load metadata-only stubs. | Session |
| I | `give` | — | `give <item>`: add an item; stack ammo/recovery, avoid duplicate unique items, and include ammunition with guns. `health`, `ammo`, and `allweapons` are bundles. | Live save |
| I | `kill` | — | Kill Harry through the death path. | Session |
| I | `killall` | — | Apply lethal damage to living non-Harry NPCs within ±50 world units on X/Z. | Session |
| E | `spawn` | — | `spawn list` reports ready resident/pool models and missing animation/AI. `spawn <name> [state]` uses a free slot four units ahead; map-bound types can be `[no-ai]` statues and foreign SFX are limited. | Session |
| E | `unlimited` | — | `unlimited [0\|1]`: toggle/set the raised natural-spawn cap of 32; default off. | Session |
| I | `noclip` | — | Toggle wall collision; floor collision remains active. | Session |
| I | `god` | — | `god [0\|1]`: toggle/set damage immunity and hold Harry at full health. | Session |

`give` item names: `knife`, `pipe`, `rockdrill`, `hammer`, `chainsaw`, `katana`, `axe`, `handgun`, `rifle`, `shotgun`, `hyperblaster`, `handgunammo`, `rifleammo`, `shotgunammo`, `gasoline`/`gas`, `healthdrink`, `firstaid`, `ampoule`, `flauros`, `channelingstone`, `plasticbottle`, `aglaophotis`, `kaufmannkey`, `ringofcontract`, `stoneoftime`, `amulet`, `crestofmercury`, `ankh`, `dagger`, `disk`, `goldmedallion`, `silvermedallion`, `lighter`, `videotape`, `camera`, `chemical`, and `bloodpack`.

`map` covers built-in `map0_s00` plus 42 optional story-map modules. `chara_global` is a separate AI pseudo-map and is not selectable.

### Inventory and collision tuning

| Status | Canonical command | Alias | Syntax and behavior | Persistence |
|---|---|---|---|---|
| I | `invaspect` | — | `invaspect [0\|1]`: toggle/set PSX-faithful (`0`) or square/true (`1`) item proportions. | Session |
| I | `invscale` | — | `invscale <50..200>`: set item vertical scale percent; default `125`. | Session |
| I | `invcary` | — | `invcary [int]`: show/set carousel Y offset; positive is down. | Session |
| I | `inveqy` | — | `inveqy [int]`: show/set equipped-item Y offset; positive is down. | Session |
| I | `invdim` | — | `invdim <0..100>`: set off-center carousel dim percent. | Session |
| E | `obst` | — | `obst [0\|1]`: show/set round-obstacle (`ptr_18`) collision. | Session |
| E | `collscope` | — | `collscope [0\|1]`: show/set preload collision scope: vanilla local cell (`1`) or all chunks (`0`). | Session |
| E | `alpha` | — | `alpha [0\|1]`: show/set the capped slope-alpha invisible-wall fix. | Session |

### Rendering and camera tuning

| Status | Canonical command | Alias | Syntax and behavior | Persistence |
|---|---|---|---|---|
| E | `vfov` | — | `vfov [float]`: world vertical-FOV scale; `1` is neutral. | Session |
| E | `hfov` | — | `hfov [float]`: Hor+ horizontal scale; `1` is neutral, larger widens models. | Session |
| E | `vshift` | — | `vshift [float]`: world vertical shift in PSX units; positive moves the view up. | Session |
| E | `msgshift` | — | `msgshift [int]`: message-box upward shift in PSX units; default `0`. | Session |
| E | `bary` | — | `bary [int]`: letterbox outer Y; inner Y becomes outer minus 16. | Session |
| E | `fogstr` | — | `fogstr [float]`: fog-density multiplier; `1` is native PC fog. | Session |
| E | `weld` | — | `weld [float]`: PGXP seam-weld radius in pixels; `0` disables it. | Session |
| E | `weldw` | — | `weldw [float]`: PGXP weld-depth ratio. | Session |
| E | `pgxpedge` | — | `pgxpedge [float]`: precise-position off-screen clamp; default `8192` PSX units. | Session |
| E | `pgxpdepth` | — | `pgxpdepth [0\|1]`: toggle/set unquantized per-vertex W; default on within PGXP. | Session |
| E | `pgxpnearclip` | — | `pgxpnearclip [0\|1]`: toggle/set near-plane clipping; default on within PGXP. | Session |
| E | `pgxpnearz` | — | `pgxpnearz [float]`: show/set near-clip depth, clamped to at least `1`; default `16`. | Session |
| E | `add` | — | `add [int]`: additive-layer diagnostic; intended modes are `0` skip, `1` normal, `2` depth-tested. | Session |
| E | `pgxp` | — | `pgxp [0\|1]`: toggle/set runtime PGXP; effective, experimental, default off. | Config |
| I | `fov` | — | `fov [55..110\|default]`: show/set FPS FOV; `default` restores `67.4°`. | Config |

`USE_PGXP=0` is vestigial: current GTE, GPU, and shader paths are built unconditionally and controlled at runtime.

### Lighting, effects, audio, media, and animation

| Status | Canonical command | Alias | Syntax and behavior | Persistence |
|---|---|---|---|---|
| I | `fmv` | — | `fmv`: list 21 video streams. `fmv <1-based #\|filename\|intro1..2\|end1..5>` fades out, plays the movie, then fades back. | Session |
| I | `flmode` | — | `flmode <0..3\|classic\|classicshadows\|modern\|modernshadows>`: select the flashlight mode; default Classic. | Config |
| I | `shadows` | — | `shadows [0\|1]`: toggle/set shadows within the active classic/modern family. | Config |
| E | `shadowbias` | — | `shadowbias [float]`: show/set shadow depth bias. | Session |
| E | `shadowstrength` | — | `shadowstrength [float]`: show/set shadow opacity; `1` is full/default. | Session |
| E | `shadowfade` | — | `shadowfade [float]`: show/set contact-fade distance; `0` is off. | Session |
| E | `shadownormal` | — | `shadownormal [float]`: show/set shadow receiver offset; `0` is off. | Session |
| E | `shadowfpsdrop` | — | `shadowfpsdrop [float]`: show/set FPS shadow-light drop. | Session |
| E | `flashlight` | `fl` | `flashlight [color]`: override flashlight color; no argument, `default`, or `off` clears it. | Session |
| E | `worldlight` | `wl` | `worldlight [color]`: override world-light color; no argument, `default`, or `off` clears it. | Session |
| I | `adsr` | — | `adsr [0\|1]`: toggle/set sequenced-BGM instrument envelopes; default on. | Session |
| I | `revscale` | — | `revscale [0..8]`: show/set reverb depth-to-wet scaling; `0` uses engine mapping. Edit `reverb_scale` to persist it. | Session |
| I | `kf` | `keyframe` | `kf [nonnegative frame]`: report inspector state or select an absolute frame and enable it. | Session |
| I | `flintensity` | `flint` | `flintensity [0..3]`: show/set active-camera beam intensity; FPS has a separate value. | Config |
| I | `postintensity` | `postint` | `postintensity [0..1]`: show/set post-process mix. | Config |
| I | `tmintensity` | `tmint` | `tmintensity [0..1]`: show/set tone-map mix. | Config |
| I | `xavolume` | `xavol` | `xavolume [0..100]`: show/set XA voice volume. FMV movie volume is separate. | Config |

Color values are `red`, `green`, `blue`, `yellow`, `cyan`, `purple`/`magenta`, `orange`, `pink`, and `white`. AVI/BIN behavior and the 30-stream table are in [fmv_files.md](fmv_files.md).

## Debug and cheat keys

Unless noted, these require `allow_debug_controls = 1`, active gameplay, and closed console input. Escape, F1–F4 graphics controls, F6/F8 save/load, F9 style cycling, and normal camera behavior are not debug keys.

| Key | Current behavior |
|---|---|
| `0` | Toggle wall noclip; floor remains active. |
| `4` / `5` | Select/save the previous/next map; applies on the next New Game. |
| `6` | Apply lethal damage to nearby non-Harry NPCs. |
| `7` | Toggle invincibility. |
| `8` | Add 15 handgun bullets. |
| `9` | Toggle no-target; enemies ignore Harry. |
| `-` | Add the Hunting Rifle if absent and 30 rifle shells. |
| `=` | Add the Shotgun if absent and 30 shotgun shells. |
| `'` | Toggle the collision visualizer; only the debug gate is required. |
| `Num3` | Outside FPS, restore room-entry safe Y and push Harry away from a fall-through point. |
| `Num.` | Log Harry/camera/collision state; with free camera active, also toggle fog. |
| L | Log the FPS eye baseline, or the current debug/normal-camera local offset. |

### Animation inspector

| Key | Behavior |
|---|---|
| K | Toggle skeleton keyframe view. |
| `,` / `.` | Select previous/next absolute frame; hold to accelerate and stop loop playback. |
| `/` | Select the next equipped-weapon animation start, or any start when unarmed. |
| N | Cycle Harry and loaded NPC targets; reset frame and playback. |
| P | Loop/stop the animation containing the selected frame. |

### Free camera

| Key | Behavior |
|---|---|
| `Num*` | Toggle; capture the current view and keep Harry at his original position. |
| `Num8/5` | Move forward/back. |
| `Num4/6` | Strafe left/right. |
| `Num7/9` | Yaw left/right. |
| Page Up/Page Down | Move up/down. |
| `Num+/-` | Pitch up/down. |
| Left Ctrl | Quarter movement/turn speed while held. |

Streaming remains centered on Harry; flying far away can expose unloaded geometry or textures.

### FPS eye tuner

Requires the debug gate, FPS style, and free camera off.

| Key | Behavior |
|---|---|
| `Num8/2` | Move eye forward/back. |
| `Num6/4` | Move eye right/left. |
| `Num9/7` | Move eye up/down, coarse. |
| `Num+/-` | Move eye up/down, fine. |
| `Num5` | Log eye and head-reference offsets. |

### Disabled and stale bindings

**D:** top-row `1` kill was removed; `Num1` is unbound; `Num0` live map switching is compiled out; the old `Num2` chase toggle is superseded by the four-style camera cycle; `Num/` has an empty handler; normal-camera numpad nudging is absent; and brackets are graphics controls, not position markers. There is no verified standalone `R` reload hotkey.

## Defaults and persistence

| Setting | Default / rule |
|---|---|
| Debug gate | `allow_debug_controls = 0`; required for overlay console and fixed debug tools. |
| Console | `key_console` defaults to backtick; one press toggles overlay+input. `show_console = 0` controls only the external window. |
| Camera style | Classic; F9/right-stick cycles and saves all four styles. |
| Save/load keys | F6/F8; gameplay-only, edge-triggered, ungated, rebindable, and routed to the original screens. |
| PGXP | Experimental, runtime-effective, off; console, F1, and PC Options save it. |
| Graphics keys | F1–F4 save on activation; intensity changes save on release. |
| Console-saved values | PGXP, flashlight mode/shadows, FPS FOV, active flashlight intensity, post/tone intensity, and XA voice volume. |
| Map | `map0_s00`. Console selection lasts for the run; debug keys, PC Options, and launcher selection save. All apply on the next New Game. |
| Global pool | `global_chara_pool = 1`; native gameplay wins, portable AI has documented exclusions. |
| Mouse cursor | `mouse_cursor = 1`; active only while the OS pointer is free. |

All other feature and setting defaults are cataloged in [features.md](../../features.md).
