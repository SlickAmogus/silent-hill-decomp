# Silent Hill 1 — Xbox 360 (libXenon / Xenos) port

Native Xbox 360 port of the Silent Hill 1 decompilation. Branch: `xbox360-port`,
cut from `xbox-port` (not `pc-port`). Only files under `C:\Claude\silenthill-xbox360`
are edited; the PC / Original-Xbox / iOS / Android trees are read-only references.

Xbox 360 behaviour goes behind `#ifdef SH_XBOX360_PORT`, layered the same way the
Original Xbox port layers `SH_XBOX_PORT` on top of `SH_PC_PORT`.

## Why `xbox-port` is the base

The Original Xbox port already did the work that separates "a PC game" from "a
console game", and every bit of it applies here:

| Problem | `pc-port` | `xbox-port` | 360 needs |
|---|---|---|---|
| PsyCross (SDL2 + OpenGL + OpenAL) | required | already stripped, bare-metal HAL | bare metal |
| Map overlays | Windows DLLs | static link + objcopy symbol renaming | no dynamic loading |
| PSX 32-bit struct layout | widened to 64-bit at load (`lm_reformat.c`, `ipd_reformat.c`) | native 32-bit path already gated `#ifdef SH_XBOX_PORT` | native 32-bit |
| File I/O / audio / pad / crash / log | SDL + host OS | 36-file console HAL | console HAL |

libXenon is 32-bit PowerPC, so the `SH_XBOX_PORT` 32-bit-native paths in the
reformat walkers are exactly what we want. Starting from `pc-port` would mean
redoing all four rows.

Cost of this choice: `xbox-port` is 473 commits behind `pc-port` (and 178 ahead).
That delta is a **separate, later** merge, using the existing `sync/pc-merge-dryrun`
process. It is game-code polish, not boot-blocking.

## Toolchain: libXenon, not the Microsoft XDK

**libXenon** (Free60), GCC/newlib bare-metal, output `xenon.elf`.

The decomp is **GPL-3.0-or-later**. A binary linked against the proprietary
Microsoft Xbox 360 XDK cannot be distributed under that licence, which is the same
reason the Original Xbox port chose nxdk over the Microsoft XDK. libXenon exists
precisely so the resulting binaries are free code.

Launch path on a retail console, all software, no hardware mod:

```
ABadAvatar (BadUpdate) -> FreeMyXe -> XeLL -> xenon.elf   (USB root)
```

**Accepted cost:** Xenia cannot run libXenon ELFs (it loads XEX; homebrew ELFs
fail with C0000225). We lose emulator iteration. The XDK path would have kept it.
See "Testing without an emulator" below for the mitigation, which covers the part
of the port that actually carries the risk.

### Compiler

Host clang 18 at `C:\Program Files\LLVM` (already installed for the Xbox port)
targets `powerpc-unknown-none-elf` and emits `elf32-powerpc` **today**, with no
cross-toolchain build:

```sh
clang --target=powerpc-unknown-none-elf -c foo.c    # verified, emits elf32-powerpc
```

So the endianness work — the bulk of this port — can start immediately on Windows.
libXenon's newlib/crt/drivers still need a Linux build of `build-xenon-toolchain`
(Docker Desktop is installed; the repo already ships a `Dockerfile`), but that is
only needed to *link*, not to compile-and-fix.

## The real work: big-endian

Xenon is big-endian. Every byte of Silent Hill's on-disc data is little-endian.
There is no way out: the retail hypervisor runs BE, and MSR[LE] under a
non-persistent 30%-success exploit is not a foundation.

**Two distinct hazards, both verified with a compile probe.** Same struct, same
compiler, one 32-bit storage unit of bitfields plus a `u16` and a `u32`:

```
LE (i686)     12 43 cd ab   22 11 00 00   66 55 44 33
BE (ppc)      12 34 ab cd   11 22 00 00   33 44 55 66
```

1. **Byte order** flips, as expected (`cd ab` -> `ab cd`).
2. **Bitfield allocation order flips inside the word** (`0x43` -> `0x34`). This one
   bites quietly: sizes still match, `STATIC_ASSERT_SIZEOF` still passes, and the
   struct reads plausible garbage.

A byteswap alone does **not** fix hazard 2. The fix is the standard one from
network-protocol headers (BSD `ip.h`): **reverse the member order of every
disc-overlaying bitfield struct under `#if BIG_ENDIAN`, then byteswap the storage
unit.** With both applied the fields land where the LE data put them.

3. **Type-punned multi-field stores, and this is the big one.** The decomp is
   faithful to MIPS, so it inherits the PSX trick of writing several sub-word
   fields with one wide store:

   ```c
   *(s32*)&(*poly)->u1 = (((ptr->field_164 << 5) + ptr->field_16C) << 8)
                       + 0x2B0000 + ((ptr->field_150 << 5) + ptr->field_158);
   ```

   That composes `u1`, `v1` and a packed halfword in **little-endian field order**
   and drops them in with one word. On big-endian every byte lands in the wrong
   field. Census: **548 `*(u32*)&`-class stores and 120 `*(u16*)&`-class**, ~668
   sites. Far larger than the bitfield surface, and it fails silently — garbage
   geometry and UVs, not a crash.

   These are tractable precisely *because* they are uniform. Every one composes
   its value assuming LE field order, so a single macro fixes the whole class:

   ```c
   #define PSX_STORE32(p, v)  (*(u32*)(p) = SH_BSWAP32(v))   /* BE only */
   ```

   On BE, storing the byte-reversed word puts the LSB at byte 0, which is exactly
   where the LE layout wanted it. So this is one macro plus a mechanical sweep,
   not 668 individual judgement calls — but the sweep has to be **complete**,
   because a missed site is invisible until something renders wrong.

### Strategy: swap at the load chokepoint, per format

Keep vanilla little-endian discs. Swap on load, in place, format-aware. No
converted data pack, so users bring their own rip exactly as on PC and Xbox.

The set is closed and small. `src/main/fileinfo.c` `g_FileExts[]` enumerates every
asset type the game loads:

```
.TIM .VAB .BIN .DMS .ANM .PLM .IPD .ILM .TMD .DAT .KDT .CMP
```

Twelve formats, and **the project already has a field-by-field walker for the hard
ones**. `lm_reformat.c`, `ipd_reformat.c`, `dms_reformat.c` and `as_rodata_reformat.c`
exist because the 64-bit PC port had to widen PSX 32-bit structs, so they already
traverse these formats correctly and are already debugged. On 360 there is no
widening to do (32-bit host, PSX layout is native) — those same walkers become the
byteswap pass. The traversal logic, which is the knowledge-intensive part, is
written.

Beyond that, format specs already exist in-tree from the modding tooling: a complete
ANM byte-layout spec, TMD/ILM/OBJ converters, TIM extraction covering 995/996 files.

### What is *not* an endian problem

- **Map overlays.** Compiled to native code (DLLs on PC, symbol-prefixed static
  objects on Xbox and iOS), so map *code* is PPC code, not data.
- **Extracted map data.** Emitted as C arrays and compiled for the target.
- **Runtime-only structs.** Written and read by our own code, so self-consistent.
- **PGXP.** Compiled out on console (`-DUSE_PGXP=0`).

Danger zone is the overlap: structs that are *both* runtime state *and* disc
overlay. Those need the bitfield reversal even though nothing ever byteswaps them.

## Testing without an emulator

The endianness work is ~all of the risk and ~none of it is graphical, so it does
not need a console:

1. **Compile-all gate.** Build every TU for `powerpc-unknown-none-elf` and hold it
   at zero errors, the way the iOS port used real Apple clang. Surfaces ABI,
   bitfield and type breakage early. (iOS lesson: `-fsyntax-only` reports a file
   clean when a header is missing — compile to objects, not syntax-only.)
2. **QEMU big-endian harness.** Run the asset loaders and game logic under
   `qemu-ppc` user-mode on the PC and diff loaded structures against the known-good
   x86 build. Every swapper is validated on the desktop before hardware ever runs.

Only the Xenos backend genuinely needs a 360.

## Reuse / replace

**Reuse from `xbox_port/` (platform-agnostic or trivially portable):** the OT walker
and PSX primitive decode (`gpu_xbox.c`), PSX VRAM emulation (`psx_vram.c`),
`psx_libgpu_xbox.c`, libc compat, crash handler, log sink, quicksave, static map
overlay machinery, memcard emulation.

**Reuse from `pc_port/`:** software GTE (`gpu_gte_pc.h`), PSX RAM/scratchpad
(`psx_memory.c`), PSY-Q header bridge, libgs/libsnd/libds stubs, the reformat
walkers (repurposed as swappers).

**Replace (nxdk -> libXenon):** `gpu_nv2a.c` (~1000 lines of NV2A pushbuffer) ->
`gpu_xenos.c` on libXenon's Xenos driver; Xbox hardware audio -> `xenon_sound`;
USB pad -> libXenon USB HID; HDD I/O -> libXenon FAT/USB.

The GPU split is favourable: `gpu_xbox.c` (1856 lines, OT walking and prim decode)
is hardware-agnostic and carries over; `gpu_nv2a.c` (1016 lines) is the only part
that is really NV2A.

360 is also far roomier than Xbox: 512 MB vs 64 MB. The Xbox port's memory-pressure
work (resident chunk textures, low-memory minimap) carries over harmlessly but
stops being load-bearing.

## Milestones

1. **Compile the tree for PPC BE.** ✅ **167/167, zero errors** (`./ppc_gate.sh`).
   Covers `src/main`, `src/bodyprog`, `src/screens`, the reused `pc_port/src`, the
   PSX HAL stubs and `map0_s00`. Excludes what `xbox_port/Makefile.nxdk` excludes:
   MIPS-asm bodies (`main.c`, `memcpy.c`) and `PCPORT_HAL_EXCLUDE`, the pc_port
   sources a console replaces wholesale. The other 400 map TUs are still out.

   What it took: three x86 `rdtsc` sites re-pointed at libXenon's `mftb()`, one
   `case 35:` label that gcc 9 rejects at the end of a compound statement, one
   local named `_P` colliding with newlib's `<ctype.h>` character-class macro, and
   a `libgs_stub.c` timer branch that assumed nxdk's `xboxkrnl.h`.
2. **libXenon toolchain + link + boot to a logged black screen** on hardware.
3. **Endian swappers** for the 12 formats, validated under QEMU against the x86 build.
4. **Software GTE + one textured triangle** through the Xenos driver.
5. **Load `map0_s00` and render one frame.**

Then iterate boot -> crashes -> rendering -> audio -> per-map, as PC and Xbox did.

## Open risks

- **Xenos shader authoring.** libXenon consumes precompiled shader microcode. The
  PSX renderer's needs are modest (textured/gouraud tris, four PSX blend modes),
  but the toolchain for producing that microcode without the XDK's `fxc` needs to
  be pinned down before milestone 4.
- **Hardware iteration loop.** BadUpdate is ~30% success and can take 20 minutes to
  trigger, and it is non-persistent. Every hardware test is expensive. Worth
  checking early whether XeLL Reloaded's network loader can push builds over LAN.
- **Bitfield audit completeness.** Hazard 2 is silent. The audit must be driven off
  the struct definitions, not off observed bugs.
