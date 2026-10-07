# Save Format and the Save Converter

How Silent Hill saves are stored on the PC port, in DuckStation and on a real PlayStation memory card, and how to move them between the three.

## Tools

All in `pc_port/tools/savecard/`:

| File | What it is |
|------|------------|
| `SH Save Converter.bat` | Drag and drop entry point. Drop cards or the save folder onto it. |
| `sh_savecard.ps1` | The command line behind the .bat (Windows PowerShell 5.1, no installs). |
| `ShSaveCard.cs` | Converter core in C# 5. The .ps1 compiles it at run time; the launcher can include it as-is. |
| `sh_savecard.html` + `sh_savecard_core.js` | Browser version. Works offline from disk or hosted on a website. |

The C# and JS cores produce byte-identical output. Change them together.

### Drag and drop

| You drop | You get |
|----------|---------|
| One or more DuckStation / PS1 cards | `SH saves for PC\0.MCD` (then `8.MCD`, `1.MCD`...) next to the first file |
| PC `N.MCD` files, or the whole `gamedata\save` folder | `SH saves for PSX\Silent Hill (USA)_1.mcd` (from `0.MCD`), `_2.mcd` (from `8.MCD`)... |
| PC cards plus one PS1 card | A prompt: add the PC saves into a copy of the PS1 card, or the reverse |

Inputs are never modified. Output goes to a new folder (`SH saves for PC (2)` and so on if one exists).

### Command line

```
sh_savecard.ps1 [-Info] [-Mode auto|topc|topsx|pc-into-psx|psx-into-pc] [-Region usa|eur|jpn] [-OutDir dir] [-NoPause] paths...
```

`-Info` lists every file and save slot on the given cards without converting anything.

## The three formats are one format

A DuckStation `.mcd`, a real-card dump (`.mcr`, as used by SD-card memory cards) and a PC port `N.MCD` are all a raw 128 KB PlayStation memory card image:

- 16 blocks of 8 KB. Block 0 is the header and directory, blocks 1 to 15 hold file data.
- Block 0 is 64 frames of 128 bytes. Frame 0 is the `MC` header, frames 1 to 15 are directory entries (one per data block), frames 16 to 35 are the bad-sector relocation list, and frame 63 is a copy of frame 0.
- A directory entry is `u32 state` (`0x51` first block of a file, `0x52` middle, `0x53` last, `0xA0` free), `u32 size`, `u16 next block` (`0xFFFF` ends the chain), a 20-character file name, and an XOR checksum of the first 127 bytes in byte 127.

DuckStation and real-card images are byte-for-byte compatible. DexDrive `.gme` (0xF40-byte header) and PSP `.vmp` (0x80-byte header) wrap the same image; the converter reads both.

### How the PC port differs

The PC port's memory card backend (`PsyCross/src/psx/libapi.c`) reads and writes the same layout, with a simplified directory:

- Builds before PsyCross `f290785` did not write directory checksums, and wrote `0` instead of `0xFFFF` as the next-block link. Current builds write standard directories, so a card the port formats or saves to is readable by DuckStation and real hardware as-is. Older cards still load; the port ignores checksums and links when reading.
- It reads a file's data from the block matching its directory slot, so a file must sit in consecutive blocks starting at its own slot. That always holds for Silent Hill, whose files are one block each.
- PC-written files have a blank icon (`MemCard_SaveBlockInit` skips the icon copy on PC), and the title was written as UTF-8 with the file number patched in byte by byte, which leaves invalid text. The PS1 memory card screen shows these as garbage. The save data itself is unaffected.

The port reads fully standard cards without trouble, so the converter always writes standard cards. The same output file works in the PC port, DuckStation and on hardware.

### Card numbering on the PC

Card files are named by the PSX device number (`buXY` becomes `X*8+Y`). The game supports the multitap and checks all eight slots, but the PC has no multitap: only `0.MCD` and `8.MCD` are connected, and the other slots report "no card" like an empty console slot. Older builds reported all eight as present and created blank `1-3.MCD` / `9-11.MCD`. Empty ones can be deleted; one that holds saves stays connected so nothing is lost.

| PC file | In game |
|---------|---------|
| `0.MCD` | Memory Card 1 |
| `1.MCD` to `3.MCD` | Memory Card 1-B to 1-D (multitap) |
| `8.MCD` | Memory Card 2 |
| `9.MCD` to `11.MCD` | Memory Card 2-B to 2-D (multitap) |

## Inside a Silent Hill file

Each file is named `BASLUS-00707SILENTnn` (USA), `BESLES-01514SILENTnn` (Europe) or `BISLPM-86192SILENTnn` (Japan), where `nn` is `00` to `14`. The game shows it as FILE01 to FILE15. The PC port always uses the USA name whatever disc it runs, so the converter renames Europe and Japan saves on the way in.

A file is exactly one block. All values are little-endian:

| Offset | Size | Contents |
|--------|------|----------|
| `0x000` | 512 | PSX title block: `SC`, icon flags `0x11`, block count 1, Shift-JIS title (`SILENT HILL  FILE01`), 16-colour icon palette at `0x60`, 16x16 4bpp icon at `0x80` |
| `0x200` | 256 | Save header (`s_MemCard_SaveHeader`): 12 bytes of metadata per slot for the save screen |
| `0x300` | 128 | Options (`s_Savegame_OptionsConfig`) |
| `0x380` | 11 x 640 | Save slots (`s_Savegame_Container`: 636-byte `s_Savegame` + footer) |
| `0x1F00` | 256 | Unused |

Each of the header, options and slot records ends in a 4-byte footer: the 8-bit XOR of the whole record (taken with the two checksum bytes at zero) written twice, then the magic `0xDCDC`. A slot is in use when its metadata has a non-zero `totalSavegameCount`.

Capacity: 11 saves per file, 15 files per card, so 165 saves per card and up to 1320 across all eight PC cards.

The `s_Savegame` and options layouts contain no pointers, and every record's checksum and footer lands at the same offset in PC-written and PS1-written files. The save data is portable in both directions without translation. Only the title block and the directory wrapper change.

## Regions

| Disc | Save file name | Title on the PS1 card screen |
|------|----------------|------------------------------|
| USA (SLUS-00707) | `BASLUS-00707SILENTnn` | SILENT HILL  FILEnn |
| Europe (SLES-01514, all five languages) | `BESLES-01514SILENTnn` | SILENT HILL  FILEnn |
| Japan (SLPM-86192, and the SLPM-86498 / SLPM-87029 reissues) | `BISLPM-86192SILENTnn` | サイレントヒル　ファイルnn |

The Japanese reissues keep the original serial in their save names, so all three Japanese discs share saves.

The save contents have the same layout in every version: the decomp builds all five releases from one set of save structures with no per-version differences. Only the name and title differ, and each game only looks for its own name. A European PS1 does not see USA-named saves, and so on.

The PC port finds a save under any of the three names, whichever disc it is running, and keeps writing to a file under the name it was found under. Files the port creates take the name (and title) of the disc being played. So:

- A DuckStation or real card from any region works in the port as-is: rename it to `0.MCD`. Saves the player makes afterwards stay readable on their own console.
- If one card holds the same FILE number under two names, the port shows the one matching the running disc (then USA, Europe, Japan). The other file is left untouched.
- Builds before this change only read and wrote USA names. The converter still renames incoming saves to USA names so its output works with those builds too.
- Going back, the converter needs to know which disc will play the saves. The .bat asks; the web page has a Disc selector. Merging into a card that already has Silent Hill saves reuses their region.

The language picked on a PAL or Japanese disc in the port is a PC setting, not part of the save. The PAL options block has a language field, which the port ignores; set the language again after moving saves.

## Conversion rules

- PS1 to PC: only Silent Hill files are copied. Data is copied unchanged. Other games' files are left out.
- PC to PS1: data is copied unchanged. The title block is rewritten in Shift-JIS for the chosen region and a blank icon is replaced by the disc's icon.
- Merging: every file already on the target card is kept, from any game. An incoming Silent Hill file whose FILE number is taken moves to the next free number, and its title is renumbered to match.

## Save editor

The web page (`sh_savecard.html`) also edits saves. It needs `sh_savecard_core.js`, `sh_savecard_data.js` and `sh_savecard_editor.js` beside it.

- **Overview:** save-list label, map, room, position, facing, health, difficulty, play time, counters, Next Fear, endings.
- **Inventory:** all 40 item slots, slot count, equipped weapon, radio and flashlight toggles, key item states.
- **Event flags:** all 1663 flags, with the decomp's names, notes and the maps whose code uses each flag in the hover text. A per-map table counts set flags and item pickups, and any save can be compared against another to list differing flags.
- **Maps & enemies:** paper maps owned and the per-map enemy-alive masks.
- **Stats** and **File options** (the options record shared by a file's 11 slots).
- **Slot order:** the arrows swap a save with the neighbouring slot, and Copy to / Move to places it in an empty slot or a new FILE. The save screen lists FILE01 to FILE15, then slots 1 to 11, skipping empty ones, and opens on the save with the highest newest-save counter.

The save-list label is a choice from the game's fixed list of 25 location names; the game cannot show custom text there.

Every edit re-seals the slot the way the game does when saving: the slot's header entry (location, save count, play time, the Next Fear / 290-hour / special-item bits) is copied from the save data and both checksums are recomputed. Running this on unedited saves reproduces the game's bytes exactly.

`sh_savecard_data.js` is generated from the decomp by `gen_savecard_data.py` (flag names from `include/event_flags.h`, usage from `src/`, map descriptions from `pc_port/src/map_registry.c`). Re-run it when those change.
