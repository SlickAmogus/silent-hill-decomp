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

---

## PC-only debug keys

Live in `game_main.c` debug-input block. No effect on stock-PSX
behavior.

| Key | Function |
|-----|----------|
| **`** (backtick) | Console toggle *(currently doesn't render — reserved)* |
| **Top-row 1** | Kill Harry (set `health = -Q12(1.0)`) |
| **Top-row 4** | Mark "incorrect" camera position to log |
| **Top-row 5** | Mark "corrected" camera position + full debug-cam state to log |
| **Top-row 6** | TPS camera state snapshot *(TPS mode only)* |

### Free-fly debug camera

Toggle: **Numpad ***  (asterisk).

| Numpad | Function |
|--------|----------|
| **8 / 5** | Forward / back |
| **4 / 6** | Strafe left / right |
| **7 / 9** | Turn left / right |
| **+ / -** | Up / down |
| **/** | Print current camera coords to log |
| **.** | Toggle fog on/off |
| **1** | Toggle wall collision |

---

## File pointers

- Keyboard → PSX-button mapping: `pc_port/PsyCross/src/PsyX_main.cpp` `PsyX_Sys_InitialiseInput()`
- PSX-button → in-game function: `src/bodyprog/sys/settings_reset.c` `Settings_RestoreControlDefaults()`
- TPS direct-poll (WASD / mouse): `src/bodyprog/sys/game_main.c` (search for `g_DebugThirdPersonCam`)
- TPS aim/fire mouse polling: `src/bodyprog/player_control.c` (search for `SDL_BUTTON_RIGHT`)
- Debug key handlers: `src/bodyprog/sys/game_main.c` (search for `[DEBUG]`)
