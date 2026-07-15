# Modding & Asset Extraction Guide

How the *Silent Hill* disc is unpacked into the loose `disc_extract/` tree, how the
game's file container works, how to get at the audio inside `.VAB` sound banks, and how
to feed modified assets back into the PC port.

This is the umbrella document. Two neighbouring guides cover specific asset classes in
more depth and are the authoritative reference for those:

- **FMV / video replacement** → [`fmv_files.md`](fmv_files.md)
- **Textures (loose PNG/TIM + DuckStation packs)** → [`Texture_Residency_And_Custom_Textures_Task.md`](Texture_Residency_And_Custom_Textures_Task.md)

---

## 1. Why jPSXdec only shows `SILENT.` and `HILL.`

The disc has almost no real ISO filesystem for its game data. At the ISO level there are
only a handful of files:

```
SLUS_007.07     the game executable (PSX-EXE)
SYSTEM.CNF      boot descriptor
SILENT.         data archive  — everything except XA audio
HILL.           data archive  — XA streaming audio (voices, cutscene/movie sound)
```

`SILENT.` and `HILL.` are **flat container blobs**. The individual assets (TIM textures,
VAB sound banks, BIN overlays, IPD/PLM maps, TMD meshes, ANM/ILM/DMS animation & cutscene
data, KDT audio metadata, …) are packed end-to-end inside them with **no directory and no
names**. Any generic ISO tool — jPSXdec, IsoBuster, 7-Zip — therefore only sees the two
archives. It cannot see inside them because the index that describes their contents does
not live on the disc's filesystem.

**The file table is embedded in the executable.** `SLUS_007.07` contains a table of 2074
entries (USA 1.1). Each 12-byte entry packs:

- **start LBA** (19 bits) — sector offset of the file inside its archive
- **block count** (12 bits) — size in 256-byte units
- **name** — 6-bit packed ASCII (`chr(32 + 6bits)` per character, up to 8 chars)
- **directory index** (4 bits) — into a per-release folder list (`1ST`, `ANIM`, `BG`,
  `CHARA`, `ITEM`, `MISC`, `SND`, `TEST`, `TIM`, `VIN`, `XA`, …)
- **type index** (4 bits) — into a per-release type list (`TIM`, `VAB`, `BIN`, `DMS`,
  `ANM`, `PLM`, `IPD`, `ILM`, `TMD`, `DAT`, `KDT`, `CMP`, `TXT`, …; empty = XA track)

This is the same structure the decomp keeps as `include/main/fileinfo.h` (`s_FileInfo`)
plus the per-region `src/main/filetable.c.<REGION>.inc`. Those `.inc` files are *generated*
by the extractor (see below) — they are the C form of the exact same on-disc table.

To unpack the real assets you must read that table, then slice bytes out of `SILENT.` /
`HILL.` at each entry's LBA. That is exactly what the two-stage extraction does.

---

## 2. How the disc was extracted (the two stages)

The extraction that produced `disc_extract/` is driven by `make extract` in the decomp
repo. It runs two tools in sequence.

### Stage 1 — split the ISO: `dumpsxiso`

`tools/psxiso/dumpsxiso` reads the raw disc image and writes out the ISO-level files plus a
`layout.xml`:

```
dumpsxiso -x rom/USA -s rom/USA/layout.xml  rom/image/SLUS-00707.bin
```

Output: `SLUS_007.07`, `SYSTEM.CNF`, `SILENT.`, `HILL.`. **This is the step that gives you
the two archives** — the same two files jPSXdec shows. Nothing here is unpacked yet.

### Stage 2 — unpack the archives: `silentassets/extract.py`

`tools/silentassets/extract.py` is the tool that turns `SILENT.`/`HILL.` into the loose,
named, per-folder tree. It:

1. CRC32s the first 4096 bytes of the executable to auto-detect the exact release (every
   retail/demo/prototype build is enumerated in `RELEASES` with its TOC offset and file
   count).
2. Seeks to that release's TOC offset in the exe and parses all N entries.
3. For each entry, seeks to `LBA × sectorSize` inside the right archive and reads the
   file's bytes — `SILENT.` uses **2048-byte** sectors, `HILL.` (XA) uses **2336-byte**
   sectors.
4. Writes each file to `<out>/<FOLDER>/<NAME>.<TYPE>` and regenerates
   `filetable.c.inc` + `fileenum.h.inc`.

It also transparently handles the disc's few oddities: encrypted `1ST/*.BIN` overlays
(XOR keystream), the pointlessly LZSS-compressed `HP_SAFE1`/`S__SAFE2` safes, and `.CMP`
files (writes a `.dec` alongside).

Invocation (from the Makefile, USA):

```
python tools/silentassets/extract.py \
    -exe rom/USA/SLUS_007.07 \
    -fs  rom/USA/SILENT. \
    -fh  rom/USA/HILL. \
    assets/USA
```

Or just: `make extract GAME_VERSION=USA` (which runs both stages).

> `-c` / `--exeChecksum` prints the CRC32 of an executable so you can confirm which
> release you have before extracting.

### Where `disc_extract/` comes from

`make extract` writes to `assets/<REGION>/` **inside** the repo. The
`C:/Claude/silenthill/disc_extract/` tree is a copy of that output placed one level above
the repo, used as the port's dev-time reference for the original bytes. Hundreds of
hand-extracted data tables in `pc_port/src/*_anim_infos.c` cite their provenance as
`disc_extract/VIN/<MAP>.BIN` at a PSX address — that folder is where those bytes were read
from. It is **not** a runtime asset directory and it is **not** tracked by git; the game
never reads it (see §5).

---

## 3. Audio in the disc: two very different systems

There are two unrelated audio containers, and the extraction handles them differently:

| | Where | What | Extract with |
|---|---|---|---|
| **VAB sound banks** | `SND/*.VAB`, `1ST/*.VAB` | SPU-ADPCM samples: SFX, footsteps, weapon sounds, sequenced BGM instruments | vgmstream (§4) |
| **XA streams** | `XA/` (from `HILL.`) | CD-XA ADPCM: cutscene voices, movie audio, some ambience | already raw sectors; see below |

`.KDT` files (`KDT1` magic) sit next to the VABs — they are the **sequence / metadata**
companions (which samples play, note/timing data for the music engine). A `.VAB` is the
instrument bank; the `.KDT` is the "score". To *listen* to the raw samples you only need
the VAB; to reproduce actual in-game music you need both plus the engine.

**XA** is stored in `HILL.` and extracted into `disc_extract/XA/` as pre-stripped 2336-byte
sectors. The port streams these **directly from the disc image at runtime** by seeking to
`(fileLoc + K) × 2352 + 16` — it never reads the loose `XA/` folder as a runtime path (see
`pc_port/src/xa_player.c`). To audition an XA track, vgmstream also reads `.xa`.

---

## 4. Extracting & repacking VAB audio

### 4.1 What a VAB is

A `.VAB` here is a **standard, self-contained PlayStation VAB** — header and body in one
file. First bytes are the magic `pBAV` (`"VABp"`), version 7. Layout:

```
0x00  "VABp"  magic
0x04  version (7)
0x08  VAB id
0x0C  total file size   (e.g. MAP000.VAB → 0x00024600 = 148992, the exact file length)
0x10  reserved / counts (programs, tones, VAGs, master vol/pan, ...)
...   program table   (16 bytes × 128 programs)
...   tone  table     (32 bytes × 16 tones per used program)
...   VAG pointer table (2 bytes × 256 — half-word size of each waveform)
...   VAG bodies       (concatenated SPU-ADPCM, 16 bytes per 28 samples)
```

Because header+body are combined, a single VAB is everything a tool needs.

### 4.2 Extract audio out of a VAB — vgmstream (the tool already used here)

The `snd_map*.wav` files in `tools/silentassets/` were produced by **vgmstream**, and
`vgmstream-cli.exe` (plus its support DLLs) is already vendored at
`disc_extract/vgmstream-cli.exe`. vgmstream treats each VAG in the bank as a **subsong**.

```
# how many subsongs (samples) are in the bank
vgmstream-cli -m SND/MAP000.VAB

# one subsong → wav
vgmstream-cli -s 3 -o MAP000_03.wav SND/MAP000.VAB

# every subsong → MAP000_00.wav, MAP000_01.wav, ...
vgmstream-cli -S 0 -o "?f_?s.wav" SND/MAP000.VAB
```

(That last form is how `snd_map000_1.wav … snd_map000_21.wav` — 21 subsongs from
MAP000.VAB — were generated.)

Alternatives if you want the raw `.VAG` files rather than decoded WAV: **VABtool**,
**PSound**, **awave**, or any "VAB ripper" will split the bank into individual `.VAG`
waveforms, which you can then decode/convert with a VAG↔WAV utility.

### 4.3 Repacking a VAB (modifying the audio)

**There is no round-trip VAB repacker in this repo.** `extract.py` copies each VAB out of
the archive byte-for-byte, and `insertovl.py` only re-inserts *overlay* `.BIN` files — it
does not rebuild VABs. So repacking is a manual, external-tool job:

1. **Re-encode your new audio to PSX SPU-ADPCM (`.VAG`).** The sample rate/pitch a tone
   plays at is fixed by the VAB's tone table, so match the original sample's rate to keep
   pitch correct. Encoders: `psxavenc`, the PsyQ/PSn00bSDK `wav2vag`, **MFAudio**, or
   VABtool's encode mode.
2. **Rebuild the VAB.** Swap the VAG body and fix up the VAG pointer table (and the total
   size at 0x0C) so offsets stay valid. VABtool / VAButil-style tools do this; doing it by
   hand is viable only if the replacement sample is the **same length** as the original
   (drop-in body swap, no table edits).
3. **Keep the file size within the original's budget** if you want the loose-file path in
   §5 — a VAB replacement that is *larger* than the original will not load that way.

For BGM specifically, remember the music is VAB (instruments) **+ KDT (sequence)**; editing
which notes play means editing the KDT, which the port/engine parses — see
[`bgm_technical_analysis.md`](bgm_technical_analysis.md).

---

## 5. Getting modified assets into the game

The port **does not read the loose `disc_extract/` tree** at runtime. It reads assets from
a **BIN/CUE disc image** via PsyCross's CDFS (`PsyX_CDFS_Init`, `libcd.c`), resolving each
file through the same embedded file table. There are two ways to override an asset.

### 5.1 Loose-file override (no disc rebuild) — the easy path

Set in the config:

```
allow_loose_files = 1
```

Then drop your replacement at:

```
gamedata/load/<FOLDER>/<NAME>
```

where `<FOLDER>` is the disc folder (`SND`, `BG`, `ITEM`, `1ST`, …) and `<NAME>` is the
exact disc filename. Examples:

```
gamedata/load/SND/MAP000.VAB      ← replace a sound bank
gamedata/load/BG/ITEM_M.TIM       ← replace a texture (or ITEM_M.TIM.png for hi-res)
```

At load time `src/main/fsqueue_3.c` intercepts each file read: if the matching loose file
exists it **byte-replaces** the disc content with your file — for **any file type**,
including VAB. Logging tags: `[LOOSE]` (hit), `[LOOSE/MISS]`, `[LOOSE/HIRES]` (oversized
TIM → hi-res texture override), `[LOOSE/INIT]`, `[LOOSE/SUMMARY]`. Set env
`SH_LOOSE_VERBOSE=1` to log every miss.

**Constraint:** the byte-replace copies into the buffer the engine sized for the *original*
file. A loose replacement that is **larger than the original** is only handled for
oversized **TIM** textures (deferred to the hi-res override path). A larger **VAB / BIN /
mesh** will not fit and won't load — keep non-texture replacements **≤ the original size**.

The launcher's **Mod Manager** automates enabling `allow_loose_files` and staging files
under `gamedata/load/` — see [`Texture_Residency_And_Custom_Textures_Task.md`](Texture_Residency_And_Custom_Textures_Task.md)
and the launcher's `ModManager.cs`.

### 5.2 Rebuild the disc image — no size ceiling

To exceed the size budget (bigger VABs, relocated files) you rebuild the whole image and
point the game at the new `.bin`:

- `tools/silentassets/insertovl.py` re-inserts rebuilt overlay `.BIN`s, patches the exe's
  file table (recomputing LBAs and re-obfuscating names), and emits an updated mkpsxiso XML.
- `tools/psxiso/mkpsxiso` then rebuilds the ISO (`make insert-ovl`).

This is the heavyweight path and is really aimed at code/overlay changes; for pure asset
swaps the loose-file override in §5.1 is almost always what you want.

---

## 6. Quick reference

| Task | Tool | Command / location |
|------|------|--------------------|
| Split ISO → `SILENT.`/`HILL.`/exe | `dumpsxiso` | `dumpsxiso -x out -s layout.xml image.bin` |
| Unpack archives → loose tree | `silentassets/extract.py` | `make extract GAME_VERSION=USA` |
| Identify a release | `extract.py -c` | `python extract.py -exe SLUS_007.07 -c` |
| List/convert VAB samples | `vgmstream-cli` | `disc_extract/vgmstream-cli.exe -m file.VAB` |
| Split VAB → `.VAG` | VABtool / PSound | external |
| Encode WAV → `.VAG` | psxavenc / MFAudio / wav2vag | external |
| Replace an asset (runtime) | loose-file override | `allow_loose_files=1` + `gamedata/load/<FOLDER>/<NAME>` |
| Replace with oversize / rebuild disc | `insertovl.py` + `mkpsxiso` | `make insert-ovl` |
| Replace an FMV | jPSXdec + AVI | see [`fmv_files.md`](fmv_files.md) |
| Replace a texture | loose PNG/TIM or pack | see [`Texture_Residency_And_Custom_Textures_Task.md`](Texture_Residency_And_Custom_Textures_Task.md) |

**File types in the archives:** `TIM` texture · `VAB` sound bank · `BIN` overlay code/data
· `DMS` cutscene · `ANM` animation · `PLM` map geometry · `IPD` map data · `ILM` character
· `TMD` mesh · `DAT` demo · `KDT` audio metadata · `CMP` compressed · (empty) XA track.
