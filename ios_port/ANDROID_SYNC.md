# Porting the iOS fixes to Android

## READ FIRST: the pause hang (PsyCross `d51c5b0`)

If pause appears to crash, it is not crashing — it is **hanging**, and the fix is
one commit.

`git merge` PsyCross `ios-port` (at or past `d51c5b0`). The gate is
`#if defined(RENDERER_OGLES)`, which Android already satisfies, so **no
Android-side edit is needed** — merging is the whole fix.

### Why it happens, so it can be recognised again

The pause screen does not redraw the world. It re-presents a captured still, and
`GR_PresentLastFrame()` opens with `if (!g_freezeFrameValid) return;`.

The capture had **always failed on both phones** — that is what the fog-grey
pause and map screens were. So that function had never once executed on a GLES
target. Fixing the capture (the ES3 RGB8/RGBA8 format matching, which Android
took from this branch) lit up a code path that had never run, and it wedges on
the first pause.

The wedge is `glBlitFramebuffer` **into** the default framebuffer. The fix uses
`GR_DrawFullscreenTexture` instead on any GLES target, MSAA or not — not a new
path, it is what the MSAA case always used, and `GR_InitPostProcess` runs
unconditionally so `g_postShader` exists even with `post_process = 0`. Desktop
keeps the blit.

**A fix that makes a previously-dead path live is a fix that ships an untested
path.** That is the general lesson.

### How to tell a hang from a crash here

`PsyX_Log_Flush()` is the **last statement of `PsyX_BeginScene`**, and
`GR_PresentLastFrame()` is called a few lines above it. So anything that wedges
inside it means nothing after that frame's last line ever reaches disk — the log
stops mid-frame with no error. On iOS the confirmation was that there was **no
crash report and no jetsam report at all**; check for the absence as hard as you
check the contents. (iOS `.ips` files named `cpu_resource` are not crash reports:
`Action taken: none`, and every game trips the CPU limit.)

### Still open after this fix

The frozen backdrop **contains the touch overlay**. The capture happens at
EndScene, after `Pc_Touch_Draw` has put its OT prims into the frame, so the still
has the gameplay buttons baked in and pause draws its own on top — two buttons
stacked in the corner, plus ghost Aim/Item/Map rings. The proper fix is the
project's standing rule: in-game UI is a GL overlay drawn *after* the capture,
never OT prims. Not done on either branch yet.

---

Most of this session's work is in shared code (`src/`, `pc_port/src/`), so Android
wants nearly all of it. The quickest route is a merge; the table is here so the
Android session knows what it is getting and which handful of hunks to skip.

```
git merge fork/ios-port
```

`ios-port` is `android-port` plus these commits plus a merge of `pc-port`, so the
merge is mostly fast-forward material. Resolve conflicts by keeping BOTH sides
wherever one side simply added something the other did not touch — that has been
the shape of every conflict between these two branches so far.

## Take these — shared code, Android has the same bug

| Commit | What it fixes | Android relevance |
|---|---|---|
| `cd43e3862` | Black sky, and the cafe culling its own walls | **Both apply.** The sky half gates the cutscene black-clear to `map3_s02` only. The culling half is the bigger one: the interior cell gate decided a neighbour cell's room from its **centre alone**, while the room table it mirrors samples five points and says outright that one is not enough. Small rooms spill into a cell whose centre lands in the dead gap, so the gate rejected the cell holding that room's own walls. Shows at wide aspects — any tablet or 20:9 phone. |
| `ba8240b39` | Fire button, One Button Combat, pause glyph | All shared. `TB_BACK` shares `TB_START`'s position and was being drawn in gameplay too, so the back mark and the pause bars rendered inside one ring — Android draws that same ring. |
| `71e33461d` | Running starts sooner, stops dropping | Pure tuning in `pc_touch.c`. One threshold was doing two jobs; now engage 0.680 / release 0.480. |
| `c50dae299` | Back out of puzzles and brightness | Free-cursor puzzles returned `TC_MODE_OFF`, which correctly suppresses the confirm but also empties the corner escape slot — unescapable with no pad. Now `TC_MODE_BACK`. |
| `172a09599` | Tap skips the boot logos and warning screen | `BootSkip_Pressed` / `Warn_SkipPressed` read raw SDL keyboard and pad only. |
| `a6bc9e27d` (part) | Memory card | **Check Android first.** There is no `.MCD` anywhere in the tree, `MemCardFormat` is an unimplemented stub, and `MemCardExist` just `fopen`s `"<chan>.MCD"` against the CWD — so saving hangs on "checking the memory card" wherever no card happens to exist. The PsyCross half (NULL guards in `MemCardAccept`/`MemCardOpen`, which dereferenced the failed `fopen`) is worth taking regardless. The card-creation half is in `ios_port/src/ios_bootstrap.m`; Android needs its own equivalent in `SilentHillActivity`, writing 128 KB with `'M','C'` in the first two bytes. |

## The pc-port merge: one trap that WILL hit Android

`ios-port` has now merged `pc-port`, which moved four desktop-vs-GLES decisions
from compile-time `#if` to a runtime capability probe (`g_grCaps`). That is the
right call for ANGLE — one binary serving either a native GL or an ES context —
but it assumes glad declares the whole enum table no matter what context is
live. Android, like iOS, compiles against **real GLES headers**, where those
tokens do not exist at all. A runtime test cannot rescue an identifier the
preprocessor never saw, so the file simply fails to compile:

```
error: use of undeclared identifier 'GL_TEXTURE_WIDTH'
error: use of undeclared identifier 'GL_CLAMP_TO_BORDER'
error: use of undeclared identifier 'GL_TEXTURE_BORDER_COLOR'
error: use of undeclared identifier 'glDrawBuffer'
```

The fix is in PsyCross `ios-port` and is not iOS-specific despite the guard
name — widen `PSYX_IOS` to include `__ANDROID__` when you take it:

- The three **enums** are defined to their registry values in `PsyX_render.h`
  purely so the desktop half of each branch parses. Every one sits behind a
  capability flag that is false on any GLES context, so none ever reaches the
  driver.
- `glDrawBuffer` is a **function**, not an enum — there is no symbol to link on
  GLES, so the capability test cannot be the only guard. It keeps a compile-time
  branch to the plural `glDrawBuffers` that ES 3.0 does have.

Take `GR_ScreenFBO()` / `GR_ScreenReadFBO()` as they are on `pc-port`, though.
They are pc-port's indirection for the borderless internal render target, and
they already replaced 24 of the bare `glBindFramebuffer(.., 0)` sites — which
means they are also the single place to redirect "the screen" on a platform
where 0 is not it. Android does not need that redirect (its EGL surface really
is FBO 0), so the fallback is a compile-time `0` there and nothing changes.

## Skip these — iOS-only

`57b1a11ef` (Info.plist bundle keys), `70b6d31e9` (framebuffer 0 is not the screen
on iOS — Android's EGL surface genuinely **is** FBO 0, so do not apply it),
`427899f25` / `130067afe` (points-vs-pixels; Android has no high-DPI split),
`3c1de6d00` (iOS Options naming), `e73c6d809` (Lua off because `loslib` needs
`system()`), and the RetroAchievements sign-in work below.

**RetroAchievements** is iOS-only for now, but only for a missing piece Android
could supply. RA sat in the desktop-only feature bucket purely because
`pc_ra_http.c` has exactly two backends — WinHTTP and libcurl — and neither
exists on a phone. rcheevos itself is libc-only. iOS added
`ios_port/src/ios_ra_http.m`, ~110 lines implementing the same two entry points
(`Pc_RaHttpRequest`, `Pc_RaSymbolLookup`) over NSURLSession, and flipped the
CMake default on. Android needs the equivalent — an `HttpURLConnection` call
through JNI, or bundling curl — plus:

- `Pc_Ra_BeginPasswordLogin` in `pc_retroachievements.c` is already shared and
  platform-neutral: it exchanges a password for a connect token, stores the
  token via `PcConfig_SaveKeyValue`, and continues into the normal
  disc-hash-and-load path. Nothing about it is iOS-specific.
- The sign-in UI is not. iOS opens a `UIAlertController` because the game's own
  2D screens have no text input, no keyboard and no masking; Android would want
  an `AlertDialog` with two `EditText` fields, called the same fire-and-forget
  way (SDL runs the game loop on the main thread, so blocking for the button
  would stop the loop that has to service the dialog).
- The options row is `PCK_RALOGIN`, gated `#if defined(SH_IOS)` — widen the
  guard, but **recount the page first**: it is the twelfth row on System, one
  past what every other page carries.
- Mach-O needed `-Wl,-export_dynamic` so `dlsym` can still find the decomp's
  globals for the address map. ELF gets this from `ENABLE_EXPORTS`/`-rdynamic`,
  which Android already sets, so nothing to do there.

## Two conflicts to expect

- **`options.c` row budget.** Pages cap at **11 rows including navigation**. The
  mobile layout now is: Controls drops the two mouse rows and gains the touch
  rows; Graphics drops Resolution and Window_Mode and gains Bullet_Decals back.
  Android already compiles out its own set — recount after merging rather than
  assuming, or a page runs off the bottom of the screen.
- **`game_main.c`.** iOS took only the radio hunk from Android's delta, not the
  frame profiler, because the profiler references `g_PsyX_DrawCalls` /
  `MsParse` / `MsSubmit` / `OtNodes`, which live in Android's own uncommitted
  PsyCross. Android keeps its profiler; just do not let the merge delete it.

## One still open

Map and pause screens intermittently clear to fog grey with the text on top,
instead of showing the world underneath. It tracks the pause **freeze-frame
capture** failing: when the blit succeeds you see the world, when it fails the
fog clear becomes the whole image. Android's own commit already made that path
report itself rather than claiming success — so reproduce it and read the log for
`[FREEZE] capture ... FAILED err=`, which names the reason instead of guessing.

On iOS one concrete cause has since been found and fixed, and it is worth ruling
out on Android before chasing anything subtler: after the pc-port merge the
capture reads through `GR_ScreenReadFBO()`, which returned `g_internalFBO` raw.
That is 0 with no internal target — and 0 is not the screen on iOS, so the blit
read a framebuffer that does not exist and produced nothing, which is exactly a
flat grey field. Android's FBO 0 **is** its EGL surface, so this particular
cause cannot apply there; if the grey survives the merge on Android, it is a
different one.
