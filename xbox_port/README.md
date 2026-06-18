# Silent Hill 1 — Original Xbox (NXDK / NV2A) port

Native Original Xbox port of the Silent Hill 1 decompilation, built on top of the
PC port (`pc_port/`). The PC port's HAL is **PsyCross** (SDL2 + OpenGL + OpenAL);
this directory replaces it with NXDK equivalents (NV2A via pbkit, Xbox hardware
audio, USB controller, HDD file I/O). Game code in `../src/` is **shared** with the
PC port — Xbox behaviour goes behind `#ifdef SH_XBOX_PORT`.

Branch: `xbox-port` (off `pc-port`). Only files under `C:\Claude\silenthill-xbox`
are edited; the PC/Duke/Star Fox trees are read-only references.

## Layout

```
xbox_port/
  nxdk/            submodule: github.com/SlickAmogus/nxdk (fork w/ APU/S-PDIF audio), pinned 2849792e
  Makefile.nxdk    GNU-make build -> default.xbe
  build_xbox.sh    build wrapper (sets NXDK_DIR + PATH, scrubs host MSVC INCLUDE/LIB)
  src/
    main_xbox.c    entry point (mirror of pc_port/src/main_pc.c)
    sh_log_xbox.c  SH_DBG -> D:\silenthill.log (implements the shared sh_log.h contract)
```

## Build

```sh
./build_xbox.sh           # -> default.xbe
./build_xbox.sh clean
./build_xbox.sh iso       # -> SilentHill.iso
```

Requires LLVM (clang/lld) at `C:\Program Files\LLVM` and msys2 make. The first build
also compiles the nxdk libraries (slow). Output `default.xbe` boots in xemu or on
hardware; logs to `D:\silenthill.log`.

## Reuse / replace (see memory `architecture-hal-split`)

**Reuse from `pc_port/` (platform-agnostic C):** software GTE (`include/gpu_gte_pc.h`),
PSX RAM/scratchpad (`src/psx_memory.c`), PSY-Q header bridge (`include/psyq/`), the
libgs/libsnd/libds stubs (`src/stubs/`, incl. the libgs scene-graph renderer that
runs the software GTE and emits PSX prims into OTs), zero-stub extraction tooling,
and all "not-PSX" game-code fixes (fixed-point, FPS/keyframe, IPD reformat).

**Replace (PsyCross → NXDK):** the PSX **libgpu** OT/prim rasterizer (`DrawOTag` →
NV2A/pbkit register combiners), SPU mixing → Xbox audio, libpad → USB controller,
libcd/file I/O → BIN on HDD (reused `xa_player.c` sector reads).

## Decisions (locked 2026-06-18)

- **GPU:** pbkit + NV2A register combiners (Star Fox/Duke pattern), not pbgl/D3D8.
- **Assets:** BIN disc image on the Xbox HDD, sector reads via reused `xa_player.c`.
- **Map overlays:** custom runtime overlay loader (Xbox has no DLLs); milestones 1–3
  static-link only `map0_s00`. Static-link-all rejected (500+ symbol collisions).

## Milestones

1. **Scaffold + boot to black screen with logging.** ← current
2. Software GTE + one textured triangle via NV2A.
3. Boot far enough to load `map0_s00` and render one frame.
Then iterate: boot → crashes → rendering → audio → per-map (as the PC port did).
