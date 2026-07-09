# PAL Support: Fonts/Text, Languages, Launcher Detection — Task Spec

Status: **OPEN — start a dedicated session on this.** Prepared 2026-07-08.
Read alongside memory `[[project_pal_eur_support]]` (detailed history + disc facts).
NOTE: a parallel session is working on textures (hires_override.c, tex_pack.c,
fsqueue_3.c PostLoadTim, terrain.h pool) — avoid refactoring those files here.

---

## 0. Where PAL support stands (all verified in-game 2026-06-15)

Single exe, no `#ifdef PAL` in game code — **versions are data, not code**:

- `pc_port/tools/gen_pal_filetable.py` decodes the file table from the EXE inside
  a BIN (reproduces the USA table byte-exact as validation). Generated
  `src/main/filetable.c.EUR.inc` (2310 entries) + reference `fileenum.h.EUR.inc`.
- Runtime region support (`851489288`): `g_FileTable` stays US-canonical (2074
  entries, `FILE_*` enums untouched); `Fs_InitFileTableForRegion(region)` rewrites
  each entry's sector/blockCount from the same-named EUR entry. 2059/2074 match;
  15 misses are US-only TIPS_J* (harmless). `g_FileXaLoc` is runtime-selected.
- `PcPort_GetGameDiscPath()` (main_pc.c:~327): priority "Silent Hill (USA).bin" >
  "(PAL).bin" > "(Europe) (En,Fr,De,Es,It).bin" > autodetect ANY .bin by ISO boot
  serial (SLUS→USA / SLES→EUR). xa_player reads the same resolved disc.
- Audio fixed (`96bdce477`): g_AudioData absolute-sector remap at region init.
- Mumbler censorship handled (`525391e9d`): EUR points GreyChild model/texture at
  CLD4 via `CharaData_ApplyRegionPatches()` (chara_data_info.c).
- VRAM-bounds clamp in GR_CopyVRAM (`d13b4cf`) — PAL's reshaped menu TIMs no
  longer crash boot.
- **PAL boots into English gameplay: school save, walking, combat, BGM/SFX/XA all
  work.** Divergence from real PAL starts at the menus.

User's PAL disc: `C:\Claude\silenthill\Silent Hill (Europe) (En,Fr,De,Es,It).bin`
(SLES-01514). To test PAL, have ONLY the PAL bin in gamedata (USA wins if both).

---

## 1. Fonts / text rendering (the "garbled menus") — analysis done, fix paused

FONT16.TIM differs structurally:
- US: 1024x16 texels — 84 glyphs in ONE horizontal strip (renderer wraps strips
  across tpages).
- PAL: 256x96 — a 20-col x 6-row grid. Rows 0-3 = the SAME English glyphs in the
  SAME order; rows 4-5 = accented characters for FR/DE/ES/IT.
- PAL's 96-tall image cannot sit at the US VRAM dest (0,496) — that overflow was
  the boot crash (now clamped, but placement is still US-shaped).
- Reference renders saved: `pc_port/build/reports/font16_US.png` / `font16_PAL.png`.

FIX: make font VRAM placement + `text_draw.c` glyph→UV math region-aware:
PAL `u=(idx%20)*12, v=(idx/20)*16` in a single 256x96 atlas; US keeps the strip
layout. char→glyphIdx mapping is UNCHANGED (same glyph order). Find a VRAM spot
where 256x96 fits without stomping anything (audit neighbors first — see the
VRAM layout facts in `[[project_texture_residency_and_packs]]` and the pool map
in Texture_Residency_And_Custom_Textures_Task.md).

Acceptance: PAL menus/status/inventory text readable; US rendering byte-identical.

---

## 2. Alternate languages (EN/FR/DE/ES/IT)

PAL stores text per-language; the disc has everything needed:
- `ITEM_ENG/FRN/GER/ITL/SPN` (+ COLORS1/2) item text files.
- `VIN2..VIN5` path dirs: per-language localized map overlays (MAP*_S1x..S4x).
  EUR `g_FilePaths` has 15 entries vs US 11 (VIN2-5 added, XA moves 10→14).

Work:
1. A `language` config key (config.cfg + launcher dropdown; default English).
   Decide whether an in-game selector (PAL had one in its option flow?) is worth
   it beyond the config key — config-only is fine for v1.
2. File resolution: the US-canonical `g_FileTable` doesn't contain EUR-only
   files (ITEM_FRN etc. / VIN2-5 overlays). Extend `Fs_InitFileTableForRegion`:
   when language != EN, redirect the ITEM_* file's sector to the chosen
   language's entry, and map overlay loads to the matching VIN2-5 entry (same
   8-char name, different pathIdx). The full EUR table is already generated —
   redirect by name lookup exactly like the existing region matching.
3. Accented glyph support falls out of §1 (rows 4-5 of the PAL atlas) — text
   bytes in localized files index those glyphs; verify the char→glyph mapping
   for >0x53 indices.
4. How does the retail PAL exe pick language? (It boots a language select or
   reads a setting.) Check SLES exe behavior in DuckStation for the canonical
   flow; we only need the DATA wiring, not their menu.

Acceptance: with the PAL bin + `language = fr` (etc.), menus, item text, and
in-map text show that language; EN default unchanged; US disc ignores the key.

---

## 3. Launcher: detect non-USA bins

`pc_port/launcher/SilentHillPC_Launcher/Form1.cs:69` (`CheckDiscImage`) warns
unless literally `gamedata/Silent Hill (USA).bin` exists — a PAL-only setup
nags every launch even though the game runs fine.

Fix: accept any `gamedata/*.bin`; ideally identify region the same way the game
does (read the ISO boot serial: SLUS-00707 / SLES-01514 — port the small probe
from `PcPort_GetGameDiscPath`) and show which disc/region was found (nice for the
language dropdown default too). Keep warning when no .bin at all.

RULES: bump `AssemblyFileVersion` when touching the launcher, and DO NOT build
the launcher — the user builds it themselves (memory
`[[feedback_launcher_version_bump]]`).

---

## 4. Known-remaining PAL caveats (park unless they bite)

- PAL title background: black + fog near bottom; we currently render the US
  white-fog look. Cosmetic; fix once fonts land (same menu-TIM family).
- Map `extracted_data` (zero-stub tables) is US-baked; EUR gameplay data is
  believed identical (EUR extras are localization). If a PAL-only zero-stub bug
  appears, regenerate the affected map's data from the PAL overlays SURGICALLY
  (never bulk-regen — memory `[[feedback_no_bulk_extract_regen]]`).
- A few `VERSION_IS(JAP0)`-style spots in game code may need EUR cases when the
  fonts/menus work exposes them.
- Optional cosmetic toggle: keep Grey Children on PAL (skip the Mumbler patch) —
  trivial config if requested.

---

## 5. Key files

- `src/main/fileinfo.c` — region tables + `Fs_InitFileTableForRegion` (language
  redirects go here).
- `src/main/filetable.c.EUR.inc` / `include/main/fileenum.h.EUR.inc` — generated
  EUR data (regen via `pc_port/tools/gen_pal_filetable.py <PAL.bin>`).
- `src/bodyprog/text_draw.c` (+ the font TIM load site — grep FONT16) — §1.
- `pc_port/src/main_pc.c` — `PcPort_GetGameDiscPath`, region init call order.
- `pc_port/src/chara_data_info.c` — `CharaData_ApplyRegionPatches`.
- `pc_port/src/pc_config.c/.h` — `language` key.
- `pc_port/launcher/SilentHillPC_Launcher/Form1.cs` — disc detection (§3).

Suggested order: §3 launcher (small, independent) → §1 fonts → §2 languages.
