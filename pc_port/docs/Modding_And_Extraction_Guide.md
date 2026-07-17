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

### One-click extraction in the launcher (no tools, no Python)

The two-stage `make extract` above is the dev path. **End users don't need it** — the
launcher's **Mod Manager** window can unpack a disc image directly, with no `dumpsxiso`,
no Python, and no `make`. The whole pipeline (ISO split + archive unpack + the disc's XOR /
LZSS / `.CMP` oddities) is reimplemented in the launcher itself (`BinExtractor.cs`, a C#
port of `extract.py` that reads `SILENT.`/`HILL.`/exe straight out of the raw-sector `.bin`
via the same ISO9660 reader the game uses).

- **Extract BIN…** — browse to a Silent Hill `.bin` (defaults to `gamedata/`), choose an
  output folder, and it writes the same `<FOLDER>/<NAME>.<TYPE>` tree as `make extract`.
  You can also **drag a `.bin` onto the Mod Manager window** — it asks "Extract x.bin?"
  then prompts for the destination. The release is auto-detected from the executable's
  CRC32, so USA/PAL/NTSC-J retail discs (and the demos/prototypes) all work.
- **Convert textures to PNG** — a checkbox in the extract dialog. When set, every extracted
  `.TIM` also gets a same-named `.png` beside it, decoded exactly the way the game draws it
  (`TimConverter.cs` mirrors `hires_override.c`: BGR555, `cx==0` transparent, STP ignored).
- **TIM → PNG…** / **Bulk → PNG…** — convert individual `.TIM` files, or recursively
  convert every `.TIM` under a folder to `<name>.png` in place (with an option to delete the
  originals). This is the quick way to turn an extracted texture tree into PNGs for the
  loose-file / hi-res override workflow in §5.

> XA audio tracks aren't the focus of the launcher extractor (they stream from the disc and
> aren't loose-overridable anyway); if you specifically need the `XA/` tree, use the dev
> `make extract`.

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

#### Multi-palette textures (monsters, characters)

Most gameplay textures — every monster and character, and many backgrounds — are a
**single 4-bit index sheet with several CLUT rows** (palette variants). A model draws its
head, body and limbs through *different* palette rows over the *same* pixels. A single PNG
carries one palette, so replacing such a texture with one image only ever recolours one
region (the classic "I replaced the monster but only its head changed").

To handle this, the extractor emits **one PNG per palette row** for a multi-CLUT TIM, named
so the game can match each row:

```
CHARA/DOB.TIM   (7 palettes)  →  DOB.TIM.p00.png  DOB.TIM.p01.png … DOB.TIM.p06.png
CHARA/CLD1.TIM  (1 palette)   →  CLD1.png
```

Edit the palette-row PNG for the region you want to change and drop the files in
`gamedata/load/CHARA/` (etc.). At load, `fsqueue_3.c` registers the disc texture as the
base and **overlays each supplied `pNN.png` onto its palette row** — untouched rows keep the
original art (tag `[LOOSE/HIRES] … loose CLUT-row override(s)` / `[POOLTEX] … loose CLUT-row
override(s)`). You may ship only the rows you edited, **but keep `p00.png`** — its presence
is what tells the game a per-palette set exists (the extractor always writes it). A single
`NAME.png` still fully replaces a one-palette texture as before.

Extraction tools that produce these PNGs:

- **Launcher Mod Manager** → *Extract BIN…* (tick "Convert textures to PNG"), *TIM → PNG…*,
  or *Bulk → PNG…* — all emit the per-palette set automatically. Windows only.
- **`pc_port/tools/tim2png.py`** — a dependency-free Python 3 converter (no Pillow) with the
  same output, for **Linux/macOS** or scripting:
  `python3 tim2png.py CHARA/DOB.TIM` or `python3 tim2png.py --bulk disc_extract/CHARA`.

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

---

## 7. Texture packs (DuckStation-style) and archives

Beyond per-file loose overrides, the game reads **content-hashed texture packs** from
`gamedata/texturemods/` when `texture_packs = 1` (the default). A pack is a folder (or an
archive) of `texupload-<srcHash>-<palHash>-…​.png` sub-images matched to a texture by the
**XXH3 hash of its pixels + palette** — so a pack is **independent of where a file sits on
the disc**. See [`Texture_Residency_And_Custom_Textures_Task.md`](Texture_Residency_And_Custom_Textures_Task.md).

**Archive packs (`.zip` / `.rar` / `.7z`).** The Mod Manager **extracts** every archive
dropped into `texturemods/` to a sibling `<name>.extracted/` folder that the game reads as a
loose folder (`.rar` via the embedded UnRAR, `.zip`/`.7z` via the embedded 7-Zip — 7-Zip
decodes *every* compression method, whereas the built-in in-place zip reader only handled
Store/Deflate and would silently load nothing from an LZMA zip). Enable/disable toggles that
folder; load order is set in the Mod Manager. A `.zip` **hand-dropped without the launcher**
is still read in place as a fallback.

## 8. Linux / macOS

The game ships **native Linux and macOS builds** (nightly), and the whole texture-mod
runtime is platform-neutral. Everything above works natively — **do not run the game under
WINE**. Minimum steps without the (Windows-only) launcher:

- **Texture packs:** drop the pack folder (or `.zip`) into `gamedata/texturemods/`.
  `texture_packs` is on by default, so no config edit is needed. `loadorder.txt` is optional
  (absent = deterministic order; it only breaks ties between overlapping packs).
- **Loose overrides:** set `allow_loose_files = 1` and place files under
  `gamedata/load/<FOLDER>/`. Enable/disable a pack by adding/removing a `.disabled` suffix
  (`mv`). Extract a `.rar`/`.7z` with the native `unrar`/`7z`/`unar` CLI first.
- **TIM → PNG authoring:** use `pc_port/tools/tim2png.py` (pure Python 3, per-palette output).
- **Disc extraction:** `make extract` (dumpsxiso + `extract.py`) or the launcher under Mono/WINE.

## 9. Fan-translation disc images (Spanish, Brazilian PT-BR, …)

Texture packs and loose overrides **work on fan-translation `.bin` images**, because pack
matching is by content hash and loose matching is by file name — neither depends on disc
sector layout, and the Brazilian rebuild's sector remap (`Fs_RemapFromDiscTable`) runs before
any texture loads while preserving file names. Point the launcher's **Disc** dropdown at the
fan `.bin` (writes `disc_image`) and your mods apply as on a retail disc.

The **only** exception is art the patch itself re-drew (title/menu/story-text overlays on the
BR rebuild): a pack keyed to *retail* pixels won't match those, because their bytes changed —
capture such replacements from the fan disc, or override them by file name via the loose path.
Confirm a pack is applying by checking the log for `[TEXPACK] composed …` on the target
texture (and, for the BR disc, that `remapped N file sectors` appears at boot).
