# Silent Hill PC Port — Controls

Two layers of input:

1. **PSX controller emulation** (`pc_port/PsyCross/src/PsyX_main.cpp`,
   `PsyX_Sys_InitialiseInput`). Maps SDL keyboard scancodes to PSX
   controller buttons, then game code reads PSX buttons via
   `g_Controller0->btnsHeld_C` — same code path as the original game.
2. **PC-only direct polling** (`src/bodyprog/sys/game_main.c` debug
   block, `src/bodyprog/player_control.c` PC port shim). Reads SDL
   keyboard / mouse state directly without going through the PSX
   button layer. Used for debug toys and the third-person camera mode.

---

## Gameplay (PSX controller emulation)

Mapped at `PsyX_main.cpp:243-262`, then routed to in-game functions
via the controller config table at `src/bodyprog/sys/settings_reset.c`
(USA default config 0).

| Keyboard | PSX Button | In-Game Function |
|----------|-----------|------------------|
| **C** | Cross | Action / Fire / Confirm |
| **V** | Circle | Cancel / Flashlight |
| **X** | Square | Cancel / Run |
| **Z** | Triangle | Cancel / Map screen |
| **A** | L1 | Sidestep Left |
| **D** | R1 | Sidestep Right |
| **RSHIFT** | L2 | View (camera) |
| **LSHIFT** | R2 | Aim |
| **[** | L3 | (unused) |
| **]** | R3 | (unused) |
| **↑ ↓ ← →** | D-Pad | Movement |
| **SPACE** | Select | Inventory menu |
| **ENTER** | Start | Pause / Skip / Confirm |

### Why aim is on LSHIFT, not RCTRL

`SDL_SCANCODE_RCTRL` never reached `btnsHeld_C` on the test Win11
machine even though `SDL_SCANCODE_C` worked fine. Sidestep got moved
off LSHIFT/RSHIFT to A/D so the SHIFT pair could take over the
L2/R2 slots. See `pc-port` commit history for the diagnostic trail.

---

## Third-person camera mode (TPS)

Toggle: **Numpad 2** captures/releases the mouse.

When TPS mode is active, `game_main.c` direct-polls these in addition
to the PSX-controller path:

| Input | Function |
|-------|----------|
| **W / A / S / D** | Forward / strafe-left / back / strafe-right (body-relative to camera yaw) |
| **LSHIFT** | Run *(also = aim — no conflict in practice; aim shim ignores run)* |
| **Mouse motion** | Camera yaw + pitch |
| **RMB** | Aim *(same effect as LSHIFT)* |
| **LMB** | Fire *(same effect as C)* |

Outside TPS mode, mouse buttons are ignored.

Top-row **6** logs a `[TPS-SNAP]` entry capturing camera + Harry pose
(only valid in TPS mode). Top-row **7/8/9/0** log preset-pose entries
for tuning the body-tracks-camera math.

---

## PC-only debug keys

Live in `game_main.c` debug-input block. No effect on stock-PSX
behavior.

| Key | Function |
|-----|----------|
| **`** (backtick) | Console toggle |
| **Top-row 1** | Kill Harry (set `health = -Q12(1.0)`) |
| **Top-row 4** | Tag camera coords to log as `BAD CAMERA POSITION` |
| **Top-row 5** | Tag camera coords to log as `GOOD CAMERA POSITION` |
| **Top-row 6** | TPS camera state snapshot *(TPS mode only)* |
| **Top-row 7/8/9/0** | TPS preset-pose loggers |

Top-row 4/5 work in **all camera modes** (normal, debug, TPS) and dump
position + lookAt + yaw + pitch to the log so you can compare a wrong
camera position against a corrected one.

---

## Cameras

There are **three** camera modes; the active one depends on which
toggles you've pressed:

1. **Normal scene camera** — the game's road-data / cutscene camera.
   This is what runs by default.
2. **Free-fly debug camera** — toggled by Numpad `*`. Disconnects
   from scene logic so you can fly around. Keeps Harry stationary.
3. **TPS follow camera** — toggled by Numpad `2`. Spherical orbit
   tracking Harry. Mouse-look + WASD body-relative movement.

### Common bindings

These work in **normal cam** and **debug cam** (both use the same
keys with parallel meaning):

| Numpad | Normal cam (nudge) | Debug cam (free-fly) |
|--------|--------------------|--------------------|
| **8 / 5** | Forward / back (cam-relative XZ) | Forward / back |
| **4 / 6** | Strafe left / right | Strafe left / right |
| **7 / 9** | Turn left / right (yaw) | Turn left / right (yaw) |
| **+ / -** | Tilt up / down (pitch) | Tilt up / down (pitch) |
| **/** | Print camera coords to log | Print camera coords to log |
| **PageUp / PageDown** | Vertical Y (up / down) | Vertical Y (up / down) |

### Normal-cam-specific

| Key | Function |
|-----|----------|
| **Numpad 3** | Reset nudge accumulator — snaps the camera back to the scene's default position |

The normal cam "nudge" system *adds* offsets on top of whatever the
game's camera logic produces. Pressing Numpad 3 zeroes the nudges and
the cam returns to its scene-driven position.

### Debug-cam-specific

| Key | Function |
|-----|----------|
| **Numpad `*`** | Toggle free-fly debug cam on/off |
| **Numpad 1** | Toggle wall collision |
| **Numpad .** | Toggle fog on/off (only when debug cam is active) |
| **Numpad 0** | Cycle to next map overlay (DLL maps) |

Wall collision and fog toggles are scoped to debug cam so they don't
interfere with normal play.

### What's *not* available

- **No pitch dial in TPS mode** — pitch is mouse-only there. The
  numpad keys are claimed by the normal-cam nudge or the debug cam.
- **No roll** in any cam — Silent Hill never rolls.
- **No FOV adjust** — bound to the scene's projection matrix.

---

## File pointers

- Keyboard → PSX-button mapping: `pc_port/PsyCross/src/PsyX_main.cpp` `PsyX_Sys_InitialiseInput()`
- PSX-button → in-game function: `src/bodyprog/sys/settings_reset.c` `Settings_RestoreControlDefaults()`
- TPS direct-poll (WASD / mouse): `src/bodyprog/sys/game_main.c` (search for `g_DebugThirdPersonCam`)
- TPS aim/fire mouse polling: `src/bodyprog/player_control.c` (search for `SDL_BUTTON_RIGHT`)
- Normal-cam nudge state + handlers: `src/bodyprog/sys/game_main.c` (search for `g_PcCamNudge`)
- Debug key handlers: `src/bodyprog/sys/game_main.c` (search for `[DEBUG]`)
