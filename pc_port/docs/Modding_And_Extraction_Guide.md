# Modding & Asset Extraction Guide

How the *Silent Hill* disc is unpacked into the loose `disc_extract/` tree, how the
game's file container works, how to replace its sounds and voices, and how to feed
modified assets back into the PC port.

This is the umbrella document, and it is the reference for **sound** (section 4) and for
the **loose-file override** every mod is built on (section 5). Neighbouring guides own the
other asset classes:

- **FMV / video replacement** → [`fmv_files.md`](fmv_files.md)
- **Textures (loose PNG/TIM + DuckStation packs)** → [`Texture_Residency_And_Custom_Textures_Task.md`](Texture_Residency_And_Custom_Textures_Task.md)
- **Character models (ILM ↔ OBJ, edit/replace in Blender)** → [`Model_Modding_Guide.md`](Model_Modding_Guide.md)
- **Modern item models (glTF)** → [`Modern_Item_GLTF_Modding_Guide.md`](Modern_Item_GLTF_Modding_Guide.md)

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
- **Build reference composites** — a second checkbox in the extract dialog. When set, EVERY
  texture the disc's geometry draws (995 of 996 — world chunks and items included, not just
  characters) also gets a `NAME_reference.png` beside it in one pass: the correct in-game look
  to paint over and *Rebuild* (see §5.1). Same result as *Reference ▾ → Every texture…*, which
  also builds the whole-tree palette-row index the composites are derived from.
- **TIM → PNG…** / **Bulk → PNG…** — convert individual `.TIM` files, or recursively
  convert every `.TIM` under a folder to `<name>.png` in place (with an option to delete the
  originals). This is the quick way to turn an extracted texture tree into PNGs for the
  loose-file / hi-res override workflow in §5.

> The `XA/` tree comes out too, read from `HILL.` at its 2336-byte sectors, on every release
> that has one (the NTSC Preview build does not). You do not need those files to replace a
> voice, though: the port takes a plain WAV per line instead (§4.7).

---

## 3. Audio in the disc: two very different systems

There are two unrelated audio containers, and the extraction handles them differently:

| | Where | What | Work with it using |
|---|---|---|---|
| **VAB sound banks** | `SND/*.VAB`, `1ST/*.VAB` | SPU-ADPCM samples: SFX, footsteps, weapon sounds, monster cries, sequenced BGM instruments | the launcher's **Audio** tool (§4.2), or vgmstream (§4.6) |
| **XA streams** | `XA/` (from `HILL.`) | CD-XA ADPCM: cutscene and event voices, movie audio, some ambience | the launcher's **Voices** tool (§4.7), or vgmstream |

`.KDT` files (`KDT1` magic) sit next to the VABs. They are the **sequence / metadata**
companions (which samples play, note/timing data for the music engine). A `.VAB` is the
instrument bank; the `.KDT` is the "score". To *listen* to the raw samples you only need
the VAB; to reproduce actual in-game music you need both plus the engine.

**XA** is stored in `HILL.` and extracted into `disc_extract/XA/` as pre-stripped 2336-byte
sectors. The port streams these **directly from the disc image at runtime** by seeking to
`(fileLoc + K) * 2352 + 16`, so it never reads the loose `XA/` folder as a runtime path (see
`pc_port/src/xa_player.c`). Replacing a voice line does not need the disc bytes at all: drop
a WAV in and the port plays it instead (§4.7).

---

## 4. Sound banks (VAB): listening, replacing, repacking

Nothing here needs Python, vgmstream or a disc rebuild. The launcher's **Audio** tool reads
and writes the banks itself, and the game can take a plain WAV per sound at runtime.

### 4.1 What a VAB is

A `.VAB` here is a **standard, self-contained PlayStation VAB**, header and body in one
file, and there are 90 of them in `SND/`. The first four bytes read `pBAV` in file order
(the header stores `"VABp"` as a little-endian word), version 7. Layout:

```
0x00  "VABp"  magic
0x04  version (7)
0x08  VAB id
0x0C  total file size  (MAP000.VAB -> 0x00024600 = 148992, the exact file length)
0x12  program count (u16)
0x16  VAG count (u16)
0x20  program table     16 bytes x 128 entries, always 128 (2048 bytes)
      tone table        32 bytes x 16 tones x program count
      VAG size table    2 bytes x 256
      VAG bodies        concatenated SPU-ADPCM, 16 bytes per 28 samples
```

Two details matter if you write your own tool. The **size table is one-based**: entry 0 is a
dummy and sample *n*'s length is `table[n] * 8` bytes, so reading it as 0-based shifts every
body and produces noise rather than a clean failure. And because bodies are packed back to
back and addressed by a running sum, changing one sample's length **moves every sample after
it**, so a bank has to be rebuilt rather than patched in place. That length field is also the
hard ceiling on a single sample: `65535 * 8` = **524,280 bytes**.

Routing inside a bank runs `sfxId -> program -> tone -> sample`. Everything above the sample
is routing; the sample is what a mod replaces. Several tones can share one sample, which is
why the same sound turns up at more than one pitch.

### 4.2 The Audio tool: browse, preview, export

Open it from the launcher's **Mod Manager** with **Audio ▾ → Sound banks (VAB)…**, or drag a
`.VAB` onto the window. It lists every sample in the bank with its size, duration, whether
its ADPCM blocks carry **loop flags**, the rate it plays at in game, the **sound ids** that
trigger it, which **programs** reference it, and an **Also in** column (see §4.4).

- **Play** / **Stop** audition the selected sample. **Preview rate** is *Auto (in-game)* by
  default, which uses the rate the game's own sound table plays that sample at rather than
  guessing one for the whole bank. The fixed rates are there for samples no sound id claims.
- **Bank slot** (*base*, *weapon*, *ambient*, *music*) narrows the sound-id matching. A bank
  file does not record which slot it gets loaded into, so this is your call.
- **Export WAV…**, **Export raw VAG…** and **Export all…** write
  `<BANK>.<NNN>.wav` / `.vag`, numbered one-based to match the list. That name is
  deliberate: it is exactly what the runtime looks for in `gamedata/load/SND/`, so
  export, edit, drop back in, and it plays (§4.5).
- Raw VAG export is the exact compressed bytes, for re-injecting a sound untouched.

### 4.3 Replacing a sound and saving the bank

**Replace…** takes a `.wav` or a `.vag` for the selected sample.

- A **WAV** is resampled to the rate that tone plays at and re-encoded to PSX ADPCM, because
  a sample carries no rate of its own. If the original sample loops, the loop flags are
  written back into the encoded blocks.
- A **VAG** goes in as-is and must be a whole number of 16-byte ADPCM blocks. A `.vag` with
  the usual 48-byte header needs that header stripped first; the tool says so rather than
  writing noise.
- Either way the result cannot exceed 524,280 bytes (§4.1).

Replacements are **staged**, not written: the row goes bold, **Play** auditions the staged
sound, and **Revert** drops it. Nothing on disk changes until you save.

**Save bank…** opens one dialog that does the whole job. There is no file picker, because
every bank is named by the game: you choose a **destination folder** (default
`gamedata/load/SND`, where the game reads them) and tick which banks to write.

The rows are the bank you edited, first and always ticked, followed by **every other bank
that carries a byte-identical copy of a sample you replaced**. Each row has a **Source**, the
file the rewrite starts from, which defaults to a copy already in the destination folder so a
second round of edits **merges into the first** instead of overwriting it. A bank holding
your earlier edit of that sound starts ticked; one holding a *different* edit of yours stays
unticked and says so.

### 4.4 One sound is copied into many banks

The disc shares sounds **by copy, not by reference**: 236 of the 506 distinct samples in
`SND/` appear in more than one bank, and the game loads one ambient bank per map. The
Groaner block, for example, sits byte-identical in eight banks. Replace it in `MAP200` alone
and it plays only in the areas that load `MAP200`.

That is what the **Also in** column and the save dialog above are for. Two things make them
trustworthy:

- **A clean reference.** The tool compares the open bank against the same bank in a pristine
  extract (**File → Clean SND folder…**, remembered as `launcher_audio_clean_snd` in
  `config.cfg`). Samples you have already changed are marked with `*` in the `#` column, and
  the duplicate search still uses the **disc** bytes, so you can open either the pristine
  bank or your edited copy and find the same family.
- **MEP/MAP twins.** `SND/` carries seven banks the game never loads (`MAP000`, `MAP100`,
  `MAP101`, `MAP102`, `MAP103`, `MAP502`, `MAP604`); it loads the near-identical `MEP*` twin
  instead. The tool says so when you open one, and the runtime accepts the twin's file names
  anyway (§4.5), so an edit made in `MAP000` still plays.

### 4.5 Loose sounds at runtime: one WAV per sample, no repacking

This is the easy path, and the one with no size ceiling. Set `allow_loose_files = 1` and
drop a WAV in:

```
gamedata/load/SND/PISTOL.002.wav      sample 2 of the PISTOL bank
gamedata/load/SND/MAP000.005.wav      the long ambient at the start of the game
```

`<BANK>.<NNN>.wav` is the bank's disc name, a dot, and the one-based sample number the Audio
tool shows. `<BANK>_<NNN>.wav` works too, since that is what older exports were named. For a
`MEP*` bank the `MAP*` twin name is accepted as well.

When a bank is uploaded to SPU RAM, `pc_port/src/pc_sfx_override.c` checks every sample for
such a file and registers the ones it finds against the address the voice will play from.
The mixer substitutes PC-owned audio at playback, which means:

- **Any length.** The replacement is not written into the bank, so nothing has to fit.
- **Any rate.** The file plays at the rate its own header declares, so what you hear in your
  editor is what plays in game. Pitch the game applies after the trigger still scales from
  there, so modulated sounds keep their modulation; what you give up is trigger-time pitch
  variation.
- **Format:** uncompressed PCM WAV, 8 or 16 bit, mono or stereo (stereo is mixed down).
- Up to 256 replacements can be live at once, and untouched samples decode exactly as before.

A **whole repacked bank** works the same way: drop `gamedata/load/SND/<BANK>.VAB` (what
*Save bank…* writes) and its samples are lifted out one by one through this same path. So a
loose VAB is **not** bound by the "no larger than the original" rule in §5.1, because it is
never byte-replaced into the disc buffer. Keep the sample count and order the same as the
disc bank, which is what *Save bank…* does.

The log tells you which half went wrong. Every bank load prints
`[SFXMOD] bank 'MAP201' loaded: 32 samples - replace as gamedata/load/SND/MAP201.001.wav ...`
whether or not anything matched, so a wrong **bank** and a wrong **file name** look
different. Each hit prints `[SFXMOD] MAP201 sample 9 <- ... (N samples @ R Hz ...)`, and a
rate of 0 there means the WAV header could not be read.

### 4.6 Command-line alternatives

`vgmstream-cli.exe` (plus its DLLs) is vendored at `disc_extract/vgmstream-cli.exe`. It
treats each VAG in a bank as a **subsong**, which is handy for bulk conversion or on
Linux/macOS:

```
vgmstream-cli -m SND/MAP000.VAB                    # how many subsongs, and their details
vgmstream-cli -s 3 -o MAP000_03.wav SND/MAP000.VAB # one subsong -> wav
vgmstream-cli -S 0 -o "?f_?s.wav" SND/MAP000.VAB   # every subsong
```

Note that vgmstream numbers and names its output its own way, so rename to
`<BANK>.<NNN>.wav` before using §4.5. For raw `.VAG` files rather than decoded WAV, or to
repack outside the launcher, **VABtool**, **PSound** and **awave** all split a bank, and
`psxavenc` / PsyQ's `wav2vag` / **MFAudio** encode WAV to SPU-ADPCM.

For BGM, remember the music is VAB (instruments) **plus** KDT (sequence): changing which
notes play means editing the KDT, which the port parses. See
[`bgm_technical_analysis.md`](bgm_technical_analysis.md).

### 4.7 Replacing voices (XA) and voicing silent text boxes

Voices are not in the banks; they stream from the disc as XA. With `allow_loose_files = 1`:

```
gamedata/load/XA/xa_0123.wav          replace disc voice line 123
gamedata/load/XA/msg_MAP1_S00_23.wav  voice a text box the game never voiced
```

`xa_NNNN` is the line's index (0 to 726). `msg_<KEY>` is the message key with dots replaced
by underscores, the same key the game logs as `[MSGBOX] MAP1_S00.23` for every unvoiced box,
so you can read a line in game, alt-tab, and copy its name out of `SilentHill.log`. A page
with no file of its own keeps the running one, so a single take can cover a multi-page
message. WAVs are 8 or 16 bit PCM, mono or stereo, any rate.

The launcher's **Voices** tool (**Audio ▾ → Voices (XA)…**) lists both sets: all 727 disc
lines with their length and format, and every unvoiced text box with its text in whichever
language the selected disc carries. It plays the original, plays your replacement, imports a
file, or **records one from your microphone** straight into the right file name.

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
gamedata/load/BG/ITEM_M.TIM       ← replace a texture (or ITEM_M.TIM.png for hi-res)
gamedata/load/CHARA/DOB.ILM       ← replace a character model
```

At load time `src/main/fsqueue_3.c` intercepts each file read: if the matching loose file
exists it **byte-replaces** the disc content with your file, for **any file type**. Logging
tags: `[LOOSE]` (hit), `[LOOSE/MISS]`, `[LOOSE/HIRES]` (oversized TIM, deferred to the
hi-res texture override), `[LOOSE/WARN]`, `[LOOSE/INIT]`, `[LOOSE/SUMMARY]`. Set env
`SH_LOOSE_VERBOSE=1` to log every miss.

**Constraint:** the byte-replace copies into the buffer the engine sized for the *original*
file, so a replacement **larger than the original** needs a path that hands the engine its
own buffer instead. Those exist for **TIM** textures (the hi-res override), **CHARA ILM**
models (`pc_big_lm.c`), **ITEM TMD** item models (`pc_big_tmd.c`) and **map IPD** chunks
(`pc_big_ipd.c`). Anything else oversized is refused with `[LOOSE/WARN] … > buf … for
non-TIM read; ignoring loose file` and the disc file loads instead, so keep other
replacements **at or under the original size**.

**Sounds do not go through this path at all.** The sound system seeks banks by absolute disc
sector, so a loose `SND/*.VAB` is invisible to the file reader above. It is picked up when
the bank is uploaded instead, along with per-sample WAVs, which is why sounds have no size
ceiling. See §4.5.

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
is what tells the game a per-palette set exists (the extractor always writes it).

**Whole reskin (one PNG for every palette).** If you don't want to author per-palette files,
drop a single `NAME.png` (e.g. `BOS.png`) with **no** `pNN.png` set — the game applies that
one image to **all** of the texture's CLUT rows. So: a lone `NAME.png` replaces the whole
texture regardless of palette; a `NAME.TIM.pNN.png` set replaces each palette region
individually.

Extraction tools that produce these PNGs:

- **Launcher Mod Manager** → *Extract BIN…* (tick "Convert textures to PNG"), *TIM → PNG…*,
  or *Bulk → PNG…* — all emit the per-palette set automatically. Windows only.
- **`pc_port/tools/tim2png.py`** — a dependency-free Python 3 converter (no Pillow) with the
  same output, for **Linux/macOS** or scripting:
  `python3 tim2png.py CHARA/DOB.TIM` or `python3 tim2png.py --bulk disc_extract/CHARA`.

##### Author from one correct image (recommended)

Editing raw `pNN.png` palette rows directly is fiddly — each one is the whole sheet tinted
by a single palette, so none of them looks like the thing you see in-game (verified: `DOB`
spreads 278 primitives across all 7 rows, `HERO` uses 14, a Old-Town chunk sheet 14).
Instead, work from the **composite**: one image showing every region in the palette the game
actually draws it with. Which region uses which palette is baked into each model primitive's
CLUT word (`row = (field_2 >> 6) − (material.field_10 >> 6)`), so the true look is
reconstructable offline — no running game needed.

This works for **every** texture class, not just characters: the same `s_LmHeader` sits at
offset 0 of an `.ILM` (characters) and a `.PLM` (weapons, BG globals), and at the `u32` at
offset 4 of an `.IPD` (world geometry chunk). Materials are matched to their `.TIM` by the
**material name**, which is why `BIRD.ILM` finds `REBIRD.TIM` and every weapon `.PLM` finds
`CHARA/HERO.TIM`.

- **Index the tree once** — `python3 clut_tool.py index <extracted-tree> -o clut_index.json`
  (about a second). One `.TIM` is shared by up to 118 `.IPD` chunks, and for 54 sheets no
  single chunk uses every palette row the game does, so the row map has to be **UNIONed
  across the whole tree** — that is what the index is for.
- **Build every reference in one pass** —
  `python3 clut_tool.py compose-all --index clut_index.json -o refs/` writes
  `refs/<FOLDER>/<NAME>_reference.png` for every texture on the disc and prints per-class
  coverage. A sheet no model references (2D backgrounds, UI, effect atlases) is emitted as
  its plain decode — there is no row map to recover for it.
- **Build one Reference** — *Reference…* in the Mod Manager, or
  `python3 clut_tool.py compose BG/THR0003F.TIM --index clut_index.json` (or point it at a
  model: `compose CHARA/DOB.ILM`, which without `--index` uses only that one model). Edit
  *that* image in any editor.
- **Rebuild Textures** — *Rebuild…*, or
  `python3 clut_tool.py split THR0003F_reference.png BG/THR0003F.TIM --index clut_index.json
  -o out/` — slices your edited image back into the `NAME.TIM.pNN.png` set above, each row
  carrying only the texels the game samples through it. Drop the set into
  `gamedata/load/<FOLDER>/`. Pass the same `--index` you composed with.

The per-row loose PNGs are uploaded as full RGBA, so this path is **not** limited to 16
colours per region — paint freely — and the edit can be **high-resolution**: paint the
reference at its native size or at an upscale of it (an exact multiple like 2×/4×/8× gives the
sharpest region edges), and *Rebuild* keeps that resolution in the output PNGs.
`compose → split` with no edits round-trips losslessly.

Two limits worth knowing. The runtime keeps **16** CLUT rows per texture
(`HIRES_POOL_MAX_ROWS`), so `pNN.png` files for row 16 and above can never load — the tools
no longer write them (`PRS.TIM` has 48 CLUT rows, `BOS2.TIM` 32; no geometry uses rows past
13, so nothing is lost). And 3.1% of covered texels are drawn through *more than one*
palette row; a composite necessarily shows one of them, so an edit there applies only to the
row it picked. Both tools measure that per sheet and report it as **shared%** — past **20%**
they warn and tell you to edit that sheet's `pNN.png` set instead, which is the only way to
paint those texels correctly. 30 of the 995 composable sheets are over the line (the worst
are `BG/THRB502H` 88%, `BG/RSRG1F` 81%, `ITEM/DRILL` 80%, `BG/HU1F051` 70%); `compose-all`
lists every one of them at the end, and `--exclude-shared 20` skips writing them at all.

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
| Browse / play / export a sound bank | launcher **Audio** tool | Mod Manager → Audio ▾ → Sound banks (VAB)… (§4.2) |
| Replace a sound, no repacking | loose WAV | `gamedata/load/SND/<BANK>.<NNN>.wav` (§4.5) |
| Repack a whole bank | launcher **Audio** tool | *Replace…* then *Save bank…* (§4.3) |
| Replace a voice line / voice a text box | launcher **Voices** tool | `gamedata/load/XA/xa_NNNN.wav`, `msg_<KEY>.wav` (§4.7) |
| List/convert VAB samples on the CLI | `vgmstream-cli` | `disc_extract/vgmstream-cli.exe -m file.VAB` |
| Split VAB → `.VAG`, encode WAV → `.VAG` | VABtool / PSound / psxavenc / MFAudio | external (§4.6) |
| Replace an asset (runtime) | loose-file override | `allow_loose_files=1` + `gamedata/load/<FOLDER>/<NAME>` |
| Replace with oversize / rebuild disc | `insertovl.py` + `mkpsxiso` | `make insert-ovl` |
| Replace an FMV | jPSXdec + AVI | see [`fmv_files.md`](fmv_files.md) |
| Rewrite dialogue / message text | text file | `gamedata/load/text_overrides.txt` (section 10) |
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

### 7.1 Dumping the game's own textures (`dump_textures`)

Set `dump_textures = 1` in `config.cfg` and play. The game's art is written to
**`gamedata/dump/`** as PNGs, *already named* the way the pack loader matches them — so
the dump folder **is** a texture pack:

```
gamedata/dump/texupload-P4-20D7201241D7412C-16F3DC9AA8F748C5-64x256-88-0-104x109-P0-15.png
                                                             ^sheet   ^box in the sheet
```

Each file is **one object**, not a whole sheet: the piece of the sheet a model actually
draws, through the palette row it actually draws it with — the same shape DuckStation
dumps. The `88-0-104x109` above is that box in native texels; the piece is drawn out of
the model file (`.ILM` for characters, `.PLM` for weapons/props, `.IPD` for world
geometry) as the engine loads it, so you get a small, tight, correctly-coloured image you
can edit directly instead of a 256×256 sheet you have to hunt through. Sheets that no
model references at all — fonts, HUD, 2D event screens — still come out as the whole
upload, because for those the whole sheet *is* the object.

Copy `gamedata/dump/` into `gamedata/texturemods/MyPack/`, repaint (or AI-upscale) the
files in place, and they load back over the exact uploads they came from. Upscales must
keep the aspect ratio; any integer scale works. Pieces you do not touch simply keep the
original art — the loader lays every crop over the native texture, so a pack can be as
partial as you like.

**Why this and not the per-palette extractor.** A SH1 texture is one 4-bit index sheet
shared by many CLUT rows, and a single model draws its head, body and limbs through
*different* rows at the same time — so `NAME.TIM.pNN.png` from §5.1 is the whole sheet
tinted by one palette and none of them looks like the thing you see in-game. A dump is
the region as it is actually drawn, through the palette actually in use. `clut_tool.py`
(§5.1) reads the same model data offline and covers the **whole disc** rather than only
the rooms you walked through, so it is still the better starting point for a full-game
pack; dumping is the way to grab exactly the object in front of you, and the only way to
reach the sheets no model references.

Notes: visit the areas you want; art is dumped as it loads. Each file is written once
(already-dumped entries are skipped, in this session and in later ones), so delete
`gamedata/dump/` to start over. A newly-seen texture costs a few ms on the loading frame,
so turn the option back off for normal play. Sheets with no model are held briefly in case
their model is still loading, and are written whole at the latest when you quit — exit the
game normally rather than killing it. Textures with no palette at all (`FONT8NOC.TIM`) and
24-bit uploads are skipped — the pack format cannot address them.

## 8. Linux / macOS

The game ships **native Linux and macOS builds** (nightly), and the whole texture-mod
runtime is platform-neutral. Everything above works natively — **do not run the game under
WINE**. Minimum steps without the (Windows-only) launcher:

- **Texture packs:** drop the pack folder (or `.zip`) into `gamedata/texturemods/`.
  `texture_packs` is on by default, so no config edit is needed. `loadorder.txt` is optional
  (absent = deterministic order; it only breaks ties between overlapping packs).
- **Loose overrides:** set `allow_loose_files = 1` and place files under
  `gamedata/load/<FOLDER>/`. The lookup is **case-insensitive**, so a folder/file authored
  on Windows (e.g. `chara/dob.tim.png`) resolves even though the disc names are uppercase.
  Enable/disable a pack by adding/removing a `.disabled` suffix (`mv`). Extract a `.rar`/`.7z`
  with the native `unrar`/`7z`/`unar` CLI first.
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

---

## 10. Text overrides (rewriting dialogue and messages)

Any map message the game shows in a text box can be replaced from a plain text file,
with no tools and no disc rebuild. Drop it at:

```
gamedata/load/text_overrides.txt
```

Several text mods can be installed at the same time. The game gathers entries from your
own file above and from every `gamedata/load/text_overrides/*.txt`, so mods that change
different lines all apply at once. See "Shipping it as a mod" below for how conflicts
are settled.

One replacement per line, `key = new text`:

```
# Lines starting with # are comments; blank lines are ignored.
map0_s00.21 = Cheryl? ~N Is that Cheryl!?
map1_s00.16 = The hands are stopped at 3:00.
map0_s00.11 = This door is welded shut.
```

The key is the map name, a dot, and the message index: the same
`MAP0_S00.21` the extracted script uses, in any case (`map0_s00.21` works too).
Map names are the port's own, `map0_s00` through `map7_s03`, exactly as listed in
`pc_port/src/map_registry.c`.

**Writing the replacement text**

| You write | Meaning |
|-----------|---------|
| plain spaces | a space (the engine's `_` is applied for you) |
| `~N` | line break |
| `~C2` … `~C7` | colour change, as in the original script |

Two things are handled automatically, so leave them out. The original line's leading
timing cue (`~J0(2.5)`) is preserved, which keeps a voiced line in sync with its audio,
and the terminating `~E` is appended. Everything else you type is passed through, so any
control code the original script uses is available.

**Indices 0 to 14 are the shared block** every map carries: Yes/No, the item pickup
prompts, the jammed door, "It's locked", and so on. They exist once per map, so
`map0_s00.11` rewrites the jammed-door line in Old Silent Hill only. Repeat the line for
each map to change it everywhere.

**Finding the key for a line**

- `pc_port/localization/SilentHill_EN_for_translation.txt` is the whole script in
  readable form, one `[KEY]` block per line of dialogue. This is the file to read from.
- `pc_port/localization/SilentHill_EN_raw.json` is the same thing as `KEY -> exact source
  string`, useful when you want to see the original control codes.
- Both are regenerated by `python pc_port/localization/extract_text.py`.
- In game, every text box logs its own key as `[MSGBOX] MAP1_S00.16` in `SilentHill.log`,
  so you can read a line, alt-tab, and copy the key straight out of the log.
- The launcher's Voices tool has a "Text boxes" list showing every key with its text, in
  any language the selected disc carries.

**Shipping it as a mod**

Put the file at `load/text_overrides.txt` inside a mod folder or zip. The Mod Manager
gives each mod its own file when it deploys, named by that mod's place in the list:

```
gamedata/load/text_overrides/000_better_dialogue.txt
gamedata/load/text_overrides/001_meme_text_pack.txt
```

So any number of text mods can be enabled together. Only a line both mods replace is a
conflict, and there the mod nearer the top of the Mod Manager list wins, exactly as it
does for any other file. Drag a mod up or down to change who wins, then Apply.

A mod can also ship a whole folder of files as `load/text_overrides/*.txt`, useful for
keeping chapters or characters in separate files. Each keeps its own name behind the
priority number.

Your own `gamedata/load/text_overrides.txt` outranks every mod, so it stays the place to
put a personal tweak without editing anyone's mod. The Mod Manager never writes to it.

**Limits and behaviour**

- 512 replacements in total, across at most 64 files, 512 bytes per line.
- Files are read once when the first map loads. Restart the game after editing one.
- Overrides are applied after any language swap, so they win on every disc region, and a
  key that names a map with no such message is skipped.
- Menus, item names and item descriptions are not covered here. Those come from a
  language pack (`gamedata/lang/*.lang`, built by
  `pc_port/localization/import_translation.py`).
- The log names every file it reads and how many replacements each contributed
  (`[MODTEXT] 000_better_dialogue.txt: 12 override(s)`), then
  `[MODTEXT] loaded N text override(s) from M file(s)`, and
  `[MODTEXT] map N: text override(s) applied` per map. A mod that lost a line to a
  higher-priority mod simply reports fewer, which is the quickest way to see a conflict.
