# TASK: "2D control" (screen-relative movement) as an alt-cam control style

> **Status — Historical/superseded task plan.** Experimental 2D screen-relative controls are implemented, default off, and exposed in PC Options. See the [feature catalog](../../features.md), [current controls](Console_And_Debug_Reference.md), and [documentation index](README.md). The design below is retained as rationale.

Prepared 2026-07-02 for a fresh-context session. Goal: add a Silent-Hill-2-mod-style
**2D control mode** (screen/camera-relative 8-way movement) as an *optional* control
style, and analyze whether our existing camera styles should optionally behave like
that mod.

## 1. What the SH2 cam mod actually does
Ref: https://github.com/zealottormunds/sh2cammod (v1.0). It's a SH2 DLL wrapper +
memory-patch mod. Feature list:
- **OTS camera**, **First-person camera**, **Free camera** (F3 free / F4 reset to player).
- **"2D controls are FORCED when the mod is enabled."** This is the crux: movement
  becomes **screen-relative** — directional input aligns with camera orientation, not
  character facing. Press "up" → move toward screen-up (away from camera) regardless of
  where the character faces; the character turns to face the movement direction.
- Camera smoothing toggle, X/Y axis inversion, fog-disable, manual aiming.
- Menu: pause (ESC) then F1; F2 toggles the mod; WASD navigates; external
  `SH2CameraConfig.exe`.

**We already have equivalents** for OTS / first-person / free(debug) cameras
(`project_fps_camera`, `project_camera_status`, `project_controls_per_camera_schemes`).
The genuinely NEW thing is the **2D (screen-relative) movement model**. The SH2 source
is irrelevant — the concept is what we port. This is essentially the classic
fixed-camera "2D control type" (SH2/3/HD "2D" option): with a fixed cinematic camera,
D-pad/stick maps to screen directions and Harry walks that way.

## 2. Our current movement models (as of 60e9afded)
Two schemes selected at runtime by `g_DebugThirdPersonCam` via
`Pc_ApplyActiveControlScheme` (`pc_port/src/control_style.c`); config holds `classic`
and `altcam` `ControlScheme` structs (`pc_config.c`).

- **Classic (tank), `g_DebugThirdPersonCam==0`:** fixed PSX camera. Up = forward in
  Harry's OWN frame; Left/Right = ROTATE Harry. Native PSX behavior. Movement machine:
  `Player_LowerBodyUpdate` when `movementOriginal` (player_control.c:1443).
- **TPS/OTS camera-follow, `g_DebugThirdPersonCam==1`:** PC shim at
  `player_control.c:1460-1600`. Every frame `player->rotation.vy = g_TpsCamYaw`
  (body SNAPS to camera yaw, line 1477). WASD/dpad/stick → forward/back/**strafe** in
  Harry's (== camera) frame (lines 1490-1500). Mouse/right-stick rotate the camera.
  This is RE4-style OTS: character always faces where the camera points; you cannot
  walk "sideways relative to the screen" without the whole view turning.

There is **no** screen-relative ("2D") movement flag anywhere
(`grep cameraRelative|screenRelative|modernMovement` = none). This is the new work.

## 3. The difference to implement
| | Classic (tank) | TPS/OTS (camera-follow) | **2D (new)** |
|---|---|---|---|
| Camera | fixed PSX | orbits behind Harry | **fixed PSX (or any)** |
| Up input | Harry-forward | camera-forward (body faces cam) | **screen-away-from-camera** |
| L/R input | rotate Harry | strafe | **screen left/right (move, don't rotate)** |
| Harry facing | steered by L/R | = camera yaw | **turns to face the move direction** |

**2D model:** build a world-space move vector from input + the ACTIVE camera's yaw:
`moveDir_world = Rotate_Y(camYaw) · (inputX, inputZ)` where input is the 8-way stick/dpad
(x=strafe axis, z=forward axis). Then set Harry's target heading to `atan2(moveDir)` and
drive him forward along it (reuse the existing run/walk speed + the lower-body anim).
Key subtlety unique to fixed cameras: **camYaw changes when you cross a camera cut**
(new room = new fixed camera angle). Real 2D-control games either (a) re-derive input
from the new camera immediately (can cause a direction flip on the cut) or (b) briefly
keep the old camera basis until the stick is released (SH/RE "hold direction on cut").
Decide which; (b) is friendlier. The fixed-camera yaw source is the vc camera
(`vc_main.c` / `g_SysWork.cameraAngleY`), NOT `g_TpsCamYaw` (that's the orbit cam).

## 4. Suggested implementation plan
1. **Config + style registry.** Add a control-style entry (e.g. `control_style = "2d"`
   → a new `g_PcConfig.controlStyle` value; see the `tps/ots/fps` mapping in
   `pc_config.c:402-411`). Expose as an alt-cam control style in the launcher +
   in-game (per `project_controls_per_camera_schemes`). Keep it OPTIONAL/off by default.
2. **Movement branch.** In `player_control.c` add a `g_Pc2dControl` path parallel to the
   `g_DebugThirdPersonCam` shim (~line 1460). Do NOT snap `rotation.vy` to a camera; instead
   compute `moveDir_world` from input rotated by the fixed-camera yaw, set
   `player->headingAngle`/target rotation toward it, and feed the existing forward-move +
   run/walk logic. Only move when input is non-zero; idle = no rotation.
3. **Camera-cut handling.** Capture the camera yaw basis; on a room/camera change, hold the
   previous basis until the stick returns to neutral (option b). Watch the existing
   room-transition path (`room_transitions`, `Game_NpcRoomInitSpawn` area).
4. **Aiming.** With a fixed camera + 2D move, aiming should stay classic (tank-aim) OR add a
   screen-relative aim — start with classic aim, since the mod's manual-aim is a separate
   feature.
5. **Works with which cameras?** Primary target: 2D move + the FIXED classic camera (the
   authentic "2D control" combo). Optionally allow 2D move under TPS/OTS too (then it's
   twin-stick: move screen-relative, camera independent) — evaluate after the fixed-cam
   version feels right.

## 5. Gotchas / prior art in this repo
- Keyboard binds become PSX buttons merged into `g_Controller0`; any "pad-only" read must
  hit SDL directly (`project_controls_per_camera_schemes`). The 8-way input here can reuse
  the same held/dpad/stick reads used at player_control.c:1490-1500.
- FPS-independent turn: reuse `TIMESTEP_SCALE_30_FPS(g_DeltaTime, ...)` (line 1466) for the
  face-toward-move-dir rotation so it doesn't vary with framerate.
- The launcher rewrites build/config.cfg on launch and the USER builds the launcher — wire
  the new style into the launcher's control-style dropdown + `ControlScheme` handling.
- Movement machine: `movementOriginal` picks `Player_LowerBodyUpdate` (native) vs the PC
  shim. The 2D path likely lives in/near the PC shim; verify interaction with
  `movementOriginal`.

## 6. Open question for the user
"2D controls" in the mod are screen-relative under an OTS/free camera. For OUR port the
most authentic + useful combo is **2D move + fixed classic camera** (the real SH "2D
control type"). Confirm whether they want 2D move available ONLY with the fixed camera,
or also selectable under TPS/OTS (twin-stick style).
