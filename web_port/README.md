# Silent Hill PC Port: browser build

The same `pc_port` game compiled to WebAssembly with Emscripten. It plays in a
desktop browser with keyboard, mouse and controller. On a phone or tablet it
plays with the same touch controls as the Android and iOS builds.

No game data ships with it. The page asks the player for their own `.bin` disc
image (USA, PAL or NTSC-J). The disc is read locally and never uploaded.

## Build

Needs [emsdk](https://github.com/emscripten-core/emsdk) (default location
`C:/Claude/emsdk`, override with `EMSDK=`), CMake, Ninja and Python 3.

```
bash web_port/build_web.sh          # Release
bash web_port/build_web.sh RelWithDebInfo
```

The site is staged in `web_port/dist/`. Serve that folder over HTTP. A
`file://` URL will not load the `.wasm` file.

| File | What it is |
|---|---|
| `index.html`, `shell.js` | launcher page: disc picker, persistence, fullscreen, crash card |
| `SilentHillPC.js/.wasm` | the game |
| `SilentHillPC.data` | the port's own shipped files from `pc_port/assets/` (fonts, lang packs, UI sounds) |

## How it fits together

- **One thread, Asyncify.** The game keeps its PSX-style blocking main loop.
  PsyCross yields to the browser with `emscripten_sleep()` in its vsync wait,
  and `SDL_Delay` yields too. The vblank and RCnt2 clocks that a thread drives
  on native builds run from a page timer here (`emIntrPump` in
  `PsyCross/src/PsyX_main.cpp`). There are no pthreads, so hosting needs no
  COOP/COEP headers.
- **WebGL 2** is GLES 3.0, so it shares the phones' ES3 shader path.
- **Audio** is the software SPU into SDL's Web Audio sink, the same path
  Android and iOS use (`SH_NO_OPENAL`).
- **Maps** are statically linked (`SH_STATIC_MAPS`). The wasm `llvm-objcopy`
  cannot rename symbols, so `tools/prefix_map_syms.py --ar` rewrites the wasm
  objects' symbol tables itself.
- **Filesystem.** `/game` is the working directory and lives in IndexedDB
  (config, memory cards). `/disc` holds the disc image in memory only.
  `/port` holds the preloaded port assets, which are copied into `/game` on
  each start. `/tmp` holds the session log.
- **Touch** follows the Android/iOS `Automatic` setting. The page tells the
  game whether the browser has a coarse pointer (`SH_WEB_TOUCH`). A desktop
  browser shows no touch controls unless a finger actually touches the screen.

## RetroAchievements

Signed in from Web Options > Achievements (the page's own dialog; the game's
screens have no text entry). The pieces that differ from desktop:

- **Transport.** No threads, so each rcheevos job is a `fetch()` (`web_ra.c`)
  that completes into the same queue `Pc_Ra_Update` drains.
- **Relay.** retroachievements.org sends no CORS headers, so API calls go to
  `/ra/dorequest.php` on the site itself. `web_port/firebase/functions` is a
  Cloud Function that forwards them; the site's `firebase.json` needs:

  ```json
  "hosting":   { "rewrites": [ { "source": "/ra/**", "function": { "functionId": "raRelay", "region": "us-central1" } } ] },
  "functions": [ { "source": "functions", "codebase": "default" } ]
  ```

  Badge art comes straight from media.retroachievements.org, which allows it.
- **Symbols.** No `dlsym`, so `tools/gen_ra_symtab.py` compiles the address
  map's name lookup into the build from `assets/gamedata/ra/sym.*.txt`.

## Wasm-only bug classes

Native ABIs tolerate these and wasm does not, so they show up here first:

- **Calling with no prototype in scope.** C assumes `int f()`, and wasm rejects
  the call against a `void` definition. The fix is a real prototype (see the
  `SH_PC_PORT` block at the end of `include/bodyprog/bodyprog.h`).
- **Calling through a pointer of the wrong function type** traps at runtime.
  The web build re-enables `-Wincompatible-function-pointer-types` and
  `-Wcast-function-type-mismatch` so these show up in the build log. An
  explicit cast hides the mismatch from both; a one-off build with
  `-Wcast-function-type-strict` lists every cast to check by hand.
- **Libc names defined by the PSX API** (`open`, `close`, `lseek`, ...). These
  would capture libc's own internal calls in a static link, so `libapi.h`
  renames them on Emscripten.

## Local testing

`?disc=<url>`, `?touch=1`, `?log=1` and `?map=<name>` work on `localhost` only.
They load a disc from the dev server, force the phone path, turn on the debug
log, and start a new game straight on one map (`?map=map0_s01` is the cafe).
`window.SHWeb.logText()` and `SHWeb.dumpFiles()` return the log and the files
the game wrote.
