# Controls and binding configuration

> **Status — Supporting reference.** Current default controls, global/graphics keys, and debug gates are canonical in [Console, controls, and debug reference](Console_And_Debug_Reference.md). Capability and setting status is in the [feature catalog](../../features.md); see also the [documentation index](README.md).

## Input model

Gameplay actions still enter the original PSX button path. PC code adds camera look, global actions, graphics controls, console input, mouse UI, and gated developer tools.

Two configurable schemes serve four styles:

| Style | Scheme | Status | Behavior |
|---|---|---|---|
| Classic | Classic | I; default | Fixed-camera tank movement and original scene cameras. |
| TPS | Alternate | E | Third-person orbit camera and shooter-style movement. |
| OTS | Alternate | E | TPS behavior with shoulder offset and a shoulder-swap action. |
| FPS | Alternate | E | First-person view, separate FOV/beam values, and optional head tracking. |

F9 or the configured pad button cycles Classic → TPS → OTS → FPS during settled gameplay and saves `control_style`. This is not debug-gated. Menus temporarily use Classic bindings; returning to gameplay restores the selected scheme. `control_2d = 1` adds experimental screen-relative movement to Classic/TPS/OTS, not FPS.

## Default action bindings

Each action supports primary and secondary keyboard-or-mouse and controller binds. Secondary binds are unbound except the alternate scheme's Action/Fire alternatives.

| Action / config stem | Classic keyboard · pad | Alternate keyboard · pad |
|---|---|---|
| Up / Down (`up`, `down`) | Up / Down · stick/D-pad | W / S · stick/D-pad |
| Left / Right (`left`, `right`) | Left / Right · stick/D-pad | Left / Right · stick/D-pad |
| Action/Fire (`cross`) | C · A | Mouse1 · RT; secondary E · A |
| Flashlight/Cancel (`circle`) | V · B | F · B |
| Map (`triangle`) | Z · Y | Tab · Y |
| Run (`square`) | X · X | Left Shift · LB |
| Sidestep (`l1`, `r1`) | A / D · LB / RB | A / D · unbound |
| View (`l2`) | Right Shift · LT | unbound |
| Aim (`r2`) | Left Shift · RT | Mouse2 · LT |
| Stick clicks (`l3`, `r3`) | unbound · LS / RS | unbound · LS / RS |
| Start / Select | Return / Space · Start / Back | Return / Space · Start / Back |

Base keys such as `key_cross` and `pad_cross` configure Classic. Add `_altcam` for TPS/OTS/FPS and `_2` before that suffix for a secondary bind: for example, `key_cross_2_altcam`. `NONE` unbinds an action. Keyboard values use SDL key names or `Mouse1` through `Mouse5`; controller values use SDL game-controller names. Controller movement directions remain fixed; `controller_movement = analog|dpad|both` selects their source.

The launcher edits both schemes for displayed movement and ten PSX action buttons; L3/R3 remain config-only. `movement_original = 1` keeps the original PSX lower-body movement; `0` selects the legacy PC shim. `altcam_button_sprint = 0` allows a near-full stick push to sprint; `1` requires the Run action.

## Global bindings

| Config key | Default | Scope |
|---|---|---|
| `key_quicksave` | F6 | Open original save screen during settled gameplay. |
| `key_quickload` | F8 | Open original load screen during settled gameplay. |
| `key_change_cam` / `pad_change_cam` | F9 / rightstick | Cycle and save the four styles during settled gameplay. |
| `key_swap_shoulder` | Mouse3 | Swap OTS shoulder during gameplay. |
| `key_console` | `` ` `` | Toggle overlay+input; requires `allow_debug_controls = 1`. |
| `key_gfx_cycle` | `\` | Select the next enabled live effect value. |
| `key_gfx_prev` / `key_gfx_next` | `[` / `]` | Lower/raise the selected effect. Mouse-wheel names are also accepted. |
| `key_exit_game` | Escape | Quit at title; otherwise return to title. |

F6/F8 are rebindable, edge-triggered, and not debug-gated. They work only in settled gameplay with console input closed. They load the normal save/load assets and enter the original screens; neither is a snapshot operation.

Backspace is a fixed settled-gameplay toggle for the alternate-style crosshair setting; it changes only the live session value. FMV skip uses fixed Return, Escape, Space, or the pad skip action after the inputs have first been released.

## Mouse cursor quality of life

`mouse_cursor = 1` by default. It operates only while the OS pointer is free:

- hover/click on title and difficulty menus;
- hover/click/wheel/right-click in Options and PC Options;
- save/load list, scroll, button, and confirmation interaction;
- cursor puzzles such as piano, plate, door panels, alert door, and map pan.

TPS/OTS/FPS normally capture the mouse for camera look, so menu cursor input is inert there. Menus release capture, and a cursor puzzle temporarily releases it regardless of style. The mouse injects the same controller flags the original screen reads, preserving each screen's stock state and sound logic.

## Developer-control status

`allow_debug_controls = 0` by default. Enabling it gates the console, cheats, collision panel, free camera, FPS eye tuner, and animation inspector. It does not gate F1–F4 graphics controls, F6/F8 save/load, F9 style cycling, or normal camera behavior.

Do not use the built-in `debug` pages as a binding reference: their Num2, Num0, Num/, normal-camera-nudge, and bracket-marker lines are stale. The current debug tables and disabled bindings are maintained only in the [canonical operational reference](Console_And_Debug_Reference.md#debug-and-cheat-keys); console commands are also documented only there.

## Source pointers

- Schemes/defaults/parser: `pc_port/src/pc_config.c`
- Style cycle, menu scheme, capture, shoulder: `pc_port/src/control_style.c`
- Save/load screen hotkeys: `pc_port/src/pc_quicksave.c`
- Mouse UI and puzzles: `pc_port/src/pc_mouse_cursor.c`, `pc_port/include/pc_mouse_cursor.h`
- Debug and camera tools: `src/bodyprog/sys/game_main.c`
