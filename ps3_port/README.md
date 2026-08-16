# Silent Hill 1 — PlayStation 3 (PSL1GHT / RSX) port

Native PS3 port of the Silent Hill 1 decompilation. Branch: `ps3-port`, cut from
`xbox360-port` (not `pc-port`). Only files under `C:\Claude\silenthill-ps3` are
edited; the PC / Original-Xbox / 360 / iOS / Android trees are read-only
references.

PS3 behaviour goes behind `#ifdef SH_PS3_PORT`, layered the same way the 360 port
layers `SH_XBOX360_PORT` on top of `SH_XBOX_PORT` and `SH_PC_PORT`.

## Status

| Milestone | State |
|---|---|
| 0. Toolchain + bootable-artifact chain | **done** — `EBOOT.BIN` builds and boots |
| 1. Compile the tree for 64-bit BE PowerPC | **done — 201/201, zero errors** (`ppu_gate.sh`) |
| 2. Link `EBOOT.BIN` against the full object set | **done** — 3.9 MB ELF, 808 KB EBOOT |
| 3. Endian swappers for the formats that are not walked | TIM done; the ~496-site type-pun sweep is the open one |
| 4. RSX backend + one textured triangle | not started (`gpu_rsx.c` is a counting no-op) |
| 5. Load `map0_s00`, render one frame | game reaches its render loop and parses 1320 prims/frame |

The port **boots and runs**: 2800+ frames under RPCS3 at ~63 fps, heap flat at
4097 KB, loading real files off a real disc image, with no fault. Verified live
rather than assumed:

```
[GTE]  RotTransPers v(64,32,512) -> sx=352 sy=256   (expected values)
[CD]   PVD id='CD001'            -- sector size, payload offset, BCD maths
[TIM]  FONT16 mode=8 rect=(0,496 256x16)  -- the endian swapper works
[PAD]  ioPadInit ok        [AUD] lv2 port up, 2 ch, 8 blocks
[OTS]  prims=1320 per frame, unk=0
```

### What is left before something is on screen

1. **The type-pun sweep**, ~496 sites (259 of them touching x/y/u/v directly).
   These are the stores described under "The real work" below, e.g.
   `*(s32*)&(*poly)->u1 = (v1 << 8) + 0x2B0000 + u1`, composing three fields in
   little-endian order. They corrupt UVs and tpages on big-endian. Each site
   needs classifying first: a *composition* has to be fixed, a straight
   *copy* between two identically-laid-out structs is already fine.
2. **The RSX backend.** Until it exists nothing can be looked at, and some
   questions cannot be settled without looking. The OT census currently reports
   a screen-space bounding box of x 0..1280 against a 640-wide framebuffer while
   y stays a tight -17..497. That is either off-screen geometry that a real
   renderer would clip, or a residual coordinate problem; the counting backend
   cannot tell the two apart, so it is recorded rather than diagnosed.

## Why `xbox360-port` is the base

Big-endian PowerPC is the dominant risk and the dominant work, and `xbox360-port`
is the only tree that has started it. Everything it built is CPU-architecture
work, not console work, so it applies to the Cell PPU verbatim: the compile gate,
the `SH_STORE_BARRIER()`/`SH_CYCLES()` abstractions in `sh_hwperf.h`, the endian
hazard census, and the console-shaped HAL it inherited from `xbox-port` (no SDL,
no dynamic loading, static map overlays, memcard emulation, CD/BIN reading).

The renderer is also already split the right way: `gpu_xbox.c` (1856 lines of OT
walking and PSX primitive decode) is hardware-agnostic and compiles for the PPU
unchanged, with only a thin backend behind the `GpuNv2a_*` interface to replace.

Cost of this choice: `xbox360-port` is 506 commits behind `pc-port` (187 ahead).
That delta is a **separate, later** merge, the same deferral the 360 port made.
It is game-code polish, not boot-blocking.

## Where the PS3 diverges from the 360, and it matters

**The PPU is 64-bit. libXenon is 32-bit.** Measured, not assumed:

```
$ ppu-gcc -c probe.c && file probe.o
probe.o: ELF 64-bit MSB relocatable, 64-bit PowerPC
```

`void*` and `long` are both 8 bytes — LP64, where the PC port's Windows build is
LLP64 (`long` is 4). So the rule for this port is:

> **PS3 takes `pc_port`'s 64-bit variants, not `xbox_port`'s 32-bit ones.**

The gate already contains one concrete instance. `map7_s03_boss_motion.c` exists
in two generated forms because its `sh_scr` is `{s32, ptr, s32}` — 12 bytes on
ILP32, 24 on LP64 — and each form bakes that stride into its pool-offset aliases.
The Xbox copy is the i386 one and its `_Static_assert(sizeof(sh_scr) == 12)`
fires on the PPU; the PPU wants the x64 copy `pc_port` generates.

### The 360's stated reason for choosing `xbox-port` does not actually hold

`xbox360_port/README.md` says the `SH_XBOX_PORT` gates select "the NATIVE 32-BIT
PSX struct layout in the reformat walkers", and that starting from `pc-port`
would mean redoing that. Reading the walkers, that is not what those gates do.

Every `SH_XBOX_PORT` block in `lm_reformat.c`, `ipd_reformat.c`, `dms_reformat.c`
and `as_rodata_reformat.c` is a **64 MB heap-leak fix** (`LmTrack`, `IpdTrack`,
the deep DMS free) plus log suppression. There is no 32-bit layout switch and no
second parse path — both ports run the same parser.

That parser reads disc data exclusively through explicit little-endian byte
readers:

```c
static inline u32 rd32(const u8* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
```

which is endian-neutral on any host of any width. **So the four hardest formats —
LM, IPD, DMS, AS rodata — are already correct on a big-endian target, for free.**
That is the single biggest piece of good news in this port, and it means the
64-bitness of the PPU costs nothing here.

The leak fixes are still wanted on their own merit: the PS3 gives homebrew
~213 MB of XDR, far closer to the Xbox's 64 MB than to PC's gigabytes. So the
gate keeps `-DSH_XBOX_PORT` — but for memory reasons, not layout ones.

## The real work: big-endian

Two hazards survive, both shared with the 360 and neither yet solved in either
tree.

**1. Bitfield allocation order flips inside the storage unit.** Measured with the
same struct on both compilers — `{a:4=2, b:4=1, c:8=0x43, d:16=0xabcd}`, then a
`u16` and a `u32`:

```
x86-64 LE   12 43 cd ab   22 11   66 55 44 33
PS3 ppc64   21 43 ab cd   11 22   33 44 55 66
```

`sizeof` is 12 on both, so `STATIC_ASSERT_SIZEOF` still passes and the struct
reads plausible garbage. A byteswap alone does not fix it. The standard remedy
(BSD `ip.h`) applies: reverse the member order of every disc-overlaying bitfield
struct under big-endian, then byteswap the storage unit.

The surface is small and auditable — bitfield members cluster in six headers,
led by `lib_unk.h` (30) and `maps/shared.h` (10). The audit must be driven off
the struct definitions, not off observed bugs, because the failure is silent.

**2. Type-punned multi-field stores.** The decomp is faithful to MIPS, so it
inherits the PSX trick of writing several sub-word fields with one wide store:

```c
*(s32*)&(*poly)->u1 = (((ptr->field_164 << 5) + ptr->field_16C) << 8) + ...;
```

That composes `u1`, `v1` and a packed halfword in **little-endian field order**.
On big-endian every byte lands in the wrong field — garbage UVs and geometry, no
crash. Census on this tree: **430 `*(u32*)&`-class and 66 `*(u16*)&`-class stores
in `src/`**, ~496 sites.

They are tractable because they are uniform — every one composes its value
assuming LE field order — so one macro plus a mechanical sweep fixes the class:

```c
#define PSX_STORE32(p, v)  (*(u32*)(p) = SH_BSWAP32(v))   /* BE only */
```

The sweep has to be **complete**, because a missed site is invisible until
something renders wrong.

### Formats still needing swappers

`src/main/fileinfo.c` `g_FileExts[]` enumerates every asset type the game loads:

```
.TIM .VAB .BIN .DMS .ANM .PLM .IPD .ILM .TMD .DAT .KDT .CMP
```

DMS, IPD and the LM/ILM path are already covered by the endian-neutral walkers
above. The rest are consumed by direct overlay and need a swap at the load
chokepoint, per format, in place — keeping vanilla little-endian discs so users
bring their own rip exactly as on PC and Xbox. Format specs already exist in-tree
from the modding tooling (a complete ANM byte-layout spec, TMD/ILM/OBJ
converters, TIM extraction covering 995/996 files).

### What is *not* an endian problem

- **Map overlays** — compiled to native PPC code, not data.
- **Extracted map data** — emitted as C arrays and compiled for the target.
- **Runtime-only structs** — written and read by our own code, self-consistent.
- **PGXP** — compiled out on console (`-DUSE_PGXP=0`).

Danger zone is the overlap: structs that are *both* runtime state *and* disc
overlay. Those need the bitfield reversal even though nothing byteswaps them.

## Toolchain

`scrapes/ps3toolchain-minimal` (Docker), which carries ps3toolchain + PSL1GHT v2:
`ppu-gcc` 7.2.0 (`powerpc64-ps3-elf`), newlib, `librsx`/`libgcm_sys`, `liblv2`,
`libaudio`, `libio`, plus `cgcomp`, `make_self`, `sprxlinker` and `ps3load`.

Every other published image is a dead end today, which is why this one is pinned:
`psl1ght/psl1ght` is 11 years old and ships a v1 manifest that containerd >= 2.1
refuses; `zeldin/ps3dev-docker` has no linux/amd64 entry; `wargio/ps3sdk` and
`ps3dev/ps3dev` do not exist as published repositories. `ps3dev.dockerfile` in
this directory rebuilds an equivalent from source as a fallback if the pinned
image disappears.

```sh
# compile gate
docker run --rm -v C:\Claude\silenthill-ps3\silent-hill-decomp:/work -w /work \
    scrapes/ps3toolchain-minimal:latest bash -lc 'bash ps3_port/ppu_gate.sh'
```

Build chain to a bootable artifact, verified end to end:

```sh
ppu-gcc -O2 -mcpu=cell -D__PS3__ ... -o sh.elf -llv2 -lsysmodule -lsysutil
sprxlinker sh.elf
make_self sh.elf EBOOT.BIN
```

### PSL1GHT and decomp headers may never meet in one TU

PSL1GHT's `<ppu-types.h>` — pulled in transitively by essentially every lv2/sys/
rsx header — typedefs `u8..u64` and `s8..s64`, and the decomp's
`include/decomp/types.h` defines the same names. They are not compatible: LP64
makes `uint64_t` `unsigned long` while the decomp's `u64` is `unsigned long
long`. Same width, different type identity, so it cannot be cast away.

`ps3_port/include/ps3_hal.h` is the seam. Shared game code includes that (plain C
types only); only sources under `ps3_port/src` that include **no** decomp headers
may include PSL1GHT. Every HAL entry point crosses the boundary as plain C.
`Ps3_TimebaseFreq()` is the first instance and sets the pattern.

## GPU: model the RSX backend on NV2A, not Xenos

The RSX is an NV47/G70 derivative; the Xbox's NV2A is an NV20/25 derivative. Both
are NVIDIA FIFO-pushbuffer parts programmed by writing class methods into a
command buffer, so `gpu_nv2a.c` (1016 lines) is a far closer template for
`gpu_rsx.c` than the 360's `gpu_xenos.c` is. The `pb_begin`/`pb_push`/`pb_end`
sequences map onto `rsxSetTransferData`-era `librsx` calls conceptually
one-for-one; method numbers and shader authoring are what actually differ.

Two things the 360 listed as open risks are closed here:

- **Shader authoring.** `cgcomp` ships in the toolchain and compiles Cg to RSX
  microcode, so there is no `fxc`-shaped hole. PSL1GHT's own build rules already
  wire it up.
- **Hardware iteration.** See below.

The renderer split is favourable in the same way it was for the 360:
`gpu_xbox.c` and `psx_vram.c` carry over as the hardware-agnostic half, and only
the backend is genuinely PS3.

Memory is roomy: ~213 MB XDR for the game plus 256 MB GDDR3 behind the RSX,
against the Xbox's 64 MB total. The Xbox port's memory-pressure work (resident
chunk textures, low-memory minimap) carries over harmlessly but stops being
load-bearing.

## Testing without a console

This is where the PS3 is in far better shape than the 360, and it is the single
biggest practical argument for doing this port next.

**RPCS3 runs PSL1GHT homebrew.** Verified in this session: a `make_self`-produced
`EBOOT.BIN` booted under `rpcs3.exe --headless`, which reported
`Title: "EBOOT.BIN" (emulation is running)`. The 360 had to accept losing
emulator iteration entirely, because Xenia cannot load libXenon ELFs. Here the
whole boot/crash/render loop can run on the desktop.

Caveat found while proving it: `--headless` aborts with "Headless mode can not be
used with this music handler. Current handler: Qt". Pass a dedicated config via
RPCS3's `--config` flag with the music handler set to Null rather than editing
the global `config.yml`, which is shared with the user's real games.

Additional layers, in the order they cost least:

1. **Compile-all gate** (`ppu_gate.sh`) — real objects, never `-fsyntax-only`,
   because a missing header makes `-fsyntax-only` report a file clean, which is
   how the iOS port lost a day.
2. **RPCS3** for boot, logic, load paths and eventually rendering.
3. **`ps3load`** pushes builds to a real console over the network, so hardware
   testing does not mean copying to USB. Nothing like BadUpdate's ~30% success
   rate stands in the way.
4. **Physical PS3** last, for RSX behaviour RPCS3 papers over and for final
   validation.

## Reuse / replace

**Reuse from `xbox_port/`** (compiled straight out of `xbox_port/src`, not
copied, so the console ports cannot drift): the OT walker and PSX primitive
decode (`gpu_xbox.c`), PSX VRAM emulation (`psx_vram.c`), `psx_libgpu_xbox.c`,
libc compat, quicksave, static map overlay machinery, memcard emulation,
`xbox_respool.c`.

**Reuse from `pc_port/`:** software GTE, PSX RAM/scratchpad, PSY-Q header bridge,
libgs/libsnd/libds stubs, the four reformat walkers, and — per the rule above —
the x64 generated sources wherever a 32-bit and 64-bit variant both exist.

**Replace (PSL1GHT):** `gpu_nv2a.c` -> `gpu_rsx.c` on `librsx`; audio ->
`libaudio`; pad -> `libio`; filesystem and CD/BIN reading -> lv2 `sysFs`;
log sink, crash handler, main.

## Bugs this port found in shared code

Both were latent in code the Xbox ports run every day, and both took booting to
find. They are recorded here because the pattern generalises: **PS3 is the first
64-bit consumer of the console renderer, and the second big-endian one.**

1. **The OT walk ran off the end of every ordering table.** `termPrim` points a
   node's `addr` at `prim_terminator`, whose own `addr` is `(uintptr_t)-1`, but
   the walk tested `next == (uintptr_t)0xffffffff`. On ILP32 those are the same
   value; on the 64-bit PPU `0x00000000FFFFFFFF` does not match
   `0xFFFFFFFFFFFFFFFF`, so the walk stepped onto the terminator, took its addr
   as the next node, and read `getlen` at `-1 + 8 == 0x7`. Fixed in
   `gpu_xbox.c`; harmless on 32-bit, where the added test is redundant.

2. **The primitive command byte moves on big-endian.** See `PSX_PRIM_CMD` in
   PsyCross's `libgpu.h`. `DrawOTag` dispatches on `P_TAG.code`, which the
   structs place at the last byte of the first data word -- only the PSX command
   byte on little-endian. The `DR_*` family builds that word as a `u_int`, so on
   big-endian its command sits at the FIRST byte and the two families disagreed.
   Symptom: a stream of `[PRIM?] code=0x1c`, which is a font tpage, not a
   command. **This one applies to the 360 exactly as much as to the PS3.**

3. **`main` never applied the console config overrides.** `XboxConfig_ApplyOverrides()`
   is called right after `PcConfig_Load` on Xbox and was missing here, so the
   frame cap, PGXP disable and log gate were all skipped. `main_xbox360.c` is
   missing it too.

## Open risks

- **The ~496-site type-pun sweep is silent on failure.** It must be driven
  mechanically off the store sites, not off observed rendering bugs.
- **Bitfield audit completeness**, same reason.
- **RPCS3 fidelity.** It will happily run code that a real RSX rejects,
  especially around command-buffer and memory alignment. `ps3load` to hardware
  should happen early enough that the two do not diverge for long.
- **`ppu-gcc` is 7.2.0.** Older than the 360's toolchain, and the decomp leans on
  gcc-permissive diagnostics; the gate's suppression set is inherited from the
  Xbox ports and may need widening as more of the tree comes in.
