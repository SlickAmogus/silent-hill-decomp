# PAL Support: Fonts/Text, Languages, Launcher — Status & Reference

Status: **IMPLEMENTED 2026-07-08 — awaiting in-game PAL testing.**
Read alongside memory `[[project_pal_eur_support]]`.
Implemented across commits: `52582ca4b` (launcher/config), `33b74e812`
(decrypt tool), `270574235` (FMV), `f5dff3a48` (fonts), `f97055547`
(languages), `ff9b575c2` (adversarial-review fixes: tree-billboard UV
reslice, ~J tab terminator, serial-probe for known filenames, NTSC-J
launcher handling, auto-load font reload, retail +31 hi-res extent, 0xFD). Everything below reflects probe-VERIFIED facts (two multi-agent
disc/exe probe passes over the PAL bin + decrypted SLES-01514 BODYPROG); the
original spec's errors are corrected here.

---

## 0. How to test PAL

- Put ONLY the PAL bin in `gamedata/` (any filename — the game and launcher
  both identify discs by ISO boot serial now; if a USA bin is also present it
  wins).
- `language = en|de|fr|es|it` in config.cfg (launcher has a dropdown).
- Expected: readable menus (PAL font at VRAM 768,128), FMVs play, item
  names/descriptions + in-map messages + death-hint TIPS images in the chosen
  language. Menus/save/load/title stay ENGLISH — retail PAL does the same
  (verified from the PAL OPTION.BIN/SAVELOAD.BIN dumps).

## 1. Fonts — DONE (region layout, US byte-identical)

- US FONT16: 84 glyphs, 1024x16 strip at (0,496), tpages 16-19, v=240,
  CLUT (304,511) packed 0x7FD3.
- PAL FONT16: **21 columns x 6 rows** (NOT 20x6 as the old spec said), 120
  cells — 0..83 byte-identical to US, 84..119 accents. Retail SLES home:
  **(768,128) in tpage 12, CLUT (816,255) packed 0x3FF3**, v=128+row*16,
  u=(cell%21)*12. DR_TPAGE 0x20C. All recovered from the decrypted EUR
  BODYPROG (`pc_port/tools/decrypt_eur_overlay.py` — the disc "encryption" is
  the game's own Fs_DecryptOverlay LCG; tool output sha1 matches
  configs/EUR/bodyprog.yaml).
- Implementation: `pc_port/src/font_region.c` (+`font_region.h`) holds
  per-region layouts + the 120-entry EUR kerning table (EUR file 0x120C =
  0x8002689C) + `Font_MapChar()`; the three text_draw.c draw sites compute
  UV/tpage/clut through `g_FontLayout` (USA output bit-identical, originals
  under `#else` for the PSX build). `Font_ApplyRegionPatches()` (called from
  main_pc.c after region detect) installs the EUR layout + rewrites
  `g_Font16AtlasImg` and STRING_COLORS[LightGrey] (EUR ships 64,64,64).
- **Accent scheme (retail-exact, from EUR disasm):** pre-remaps 0x96→'-',
  0x9C→cell118 (œ), 0xA1→116 (¡), 0xBF→115 (¿), 0xC7→117 (Ç); bytes ≥0xDF →
  cell byte-0x8B; bytes 0xC0..0xDE → TWO emissions: zero-advance combining
  mark (cell 119 acute for 0xC1/0xC9, else cell 114 diaeresis) at posY-3,
  then base letter (Á/Ä→A, É→E, Ö→O, Ü→U, else '*'); bytes <0xC0 → byte-0x27.
  The old spec's "char→glyph mapping is UNCHANGED" was wrong.
- **VRAM co-tenants** (why the other desc patches exist): retail EUR keeps
  every 2D image at its US dest and only moves CLUTs; the required set is
  exactly: FONT16 desc; BG_ETC *reslice* (PAL TIM is US 128x256 cut into two
  halves side-by-side: PAL(u,v)=US(u,v) for v<128, US v≥128 → PAL
  (u+128,v-128)) → IMAGE_ETC material desc becomes u=32,v=64 (world_draw.c);
  FLAME → tpage 13 (832,0) clut (832,64) incl. its draw site
  (map_effects.c); particle.c dust/ember frames (v240..255) remap via
  `Pc_BgEtcSpriteBandUvFix`, rain streaks clamp v=128→127 off the font row;
  exterior tree billboards (Gfx_BillboardDraw via the D_800AE4DC UV table,
  texels 0..63/128..191) remapped in Font_ApplyRegionPatches.
  All other page-12 samplers verified SAFE (u<128, v<128 — census in session
  notes). KONAMI logo TIM is byte-identical US/PAL at the same dest — no
  desc change; instead FONT16 is REQUEUED at `GameFs_TitleGfxLoad` (all boot
  paths) and per map load in game_boot.c (covers auto-load-save), matching
  retail SLES which reloads the font from its B_KONAMI overlay.

## 2. Languages — DONE (config `language`, EUR discs)

- **VIN dir ↔ language (string-dump verified):** VIN=EN, VIN2=DE, VIN3=FR,
  VIN4=ES, VIN5=IT — matches the PAL option-menu order EN,DE,FR,ES,IT.
  Config ids: en/de/fr/es/it → g_PcConfig.language 0..4.
- **Map overlays are NOT same-named across dirs** (old spec wrong): language
  digit is baked into name char 6 (MAP0_S00 → S10/S20/S30/S40). The redirect
  in `Fs_InitFileTableForRegion` transforms name char 6 + looks up EUR
  pathIdx 9+lang; the ACTIVE table keeps US names/pathIdx (g_FilePaths has
  11 entries — EUR path indices must never land there).
- **TIPS death hints:** TIPS_E→G(DE)/R(FR)/S(ES)/T(IT), letter at name char
  5, rebind of the 15 TIPS_E entries — image-render verified letters.
- **PC maps compile US English**, so redirected overlay bytes alone change
  nothing: `pc_port/src/lang_text.c` extracts the message table from
  g_OvlDynamic after each map load (table ptr at FILE offset 0x34, EUR link
  base 0x800CB370, **fileOffset = ptr - base, NO +4**), translates PAL
  dialect → US dialect, and repoints a writable map-header copy.
- **Markup dialect (census-verified):** PAL `{X}` ↔ US `~X` 1:1 for
  E,D,C2,C3,C5,C7,S3,S4,L4,H,M,J0/J1/J2(x); PAL has NO {N} — line breaks are
  literal 0x0A (US parser needs `~N`); PAL uses real spaces (US '_'); tabs
  are skipped by both parsers; single-letter codes get a pad byte (the US
  parser always consumes one arg char). No PAL-only codes; US ~C6 has no PAL
  equivalent (unused).
- **Index shift:** EUR count = US+1 on ALL 41 maps — one insert at index 3
  (the split second half of the intro message; an "{E}" stub in EN/FR).
  Handled by joining EUR[2]+EUR[3] into US index 2. Seven additional
  per-language page splits (probe-pinpointed) are joined via the
  `s_MsgSplits` table: MAP1_S01 US[23] DE/FR ×2, IT US[23]×3 + US[24]×2 +
  US[25]×2; MAP1_S03 US[22] ES ×2; MAP5_S02 US[43] IT ×2. Joins over the
  9-line renderer cap collapse blank lines, then clip — **FR/IT piano-poem
  pages still lose 1-3 lines** (known limitation; candidate for
  hand-condensed loose-file overrides later).
- **Item text:** VIN/ITEM_{GER,FRN,SPN,ITL}.BIN parsed off the disc
  (4-byte zero prefix + 195 {namePtr,descPtr} pairs, base 0x800C8B68,
  ITEM_ENG is verbatim US text). Served through the s_ItemName/s_ItemDesc
  chokepoints; NULL/empty entries fall back to English (matches the disc —
  e.g. Lobby_key is EN-only). Only translation needed: spaces→'_' (tabs
  don't exist in item text; Gfx_StringDraw ignores tabs anyway).
- **EUR overlay pointer rebase:** EUR overlays are linked for 0x800CB370 but
  load at the US base 0x800C9578 — player_control.c's map-anim header patch
  now rebases by 0x1DF8 on EUR (was a LIVE bug for the 7 per-map anim-table
  maps on any PAL disc, language aside).
- **English on PAL** = compiled US strings (PAL-EN is a slightly different
  retranslation; policy: keep US text for en).

## 3. Launcher — DONE

Any `gamedata/*.bin` accepted; discs identified by ISO boot serial
(`DiscProbe.cs`, table-driven prefixes incl. SLPS/SLPM/SIPS for future
NTSC-J); detected serial/region shown in the UI; Language dropdown
round-trips the `language` key. AssemblyFileVersion 2026.7.08.1. Launcher is
built by the user, never by Claude.

## 4. FMV — DONE

All 30 movies are byte-identical US↔PAL (only placed +0x1E88 sectors later);
fmv_player now opens the resolved disc and reads base sectors from the
region-remapped g_FileTable. 15fps pacing + null-sector demux verified
region-identical — no timing change.

## 5. Remaining / parked

- **In-game PAL test pass** (user): boot each language, school save, item
  screen, map messages, death TIPS, FMV, match flame, rain/dust particles,
  water splashes (page-12 effects), warm reboot, skip_intros.
- FR/IT piano-poem + IT finale clipped lines (see §2) — polish via text
  overrides if it bothers anyone.
- PAL title background differs cosmetically (black + fog near bottom; we
  render the US look). Parked.
- COLORS1/COLORS2: the plates-puzzle images carry NO text (render-verified)
  — shared COLORS.TIM path is correct for all languages. Closed.
- Attract demos: PAL DAT inputs may have been re-recorded for 50Hz —
  possible desync at forced 60Hz (cosmetic, attract only). Unverified.
- In-game Options "Language" row (retail PAL had one): config-only for now;
  `s_OptionsConfig.palLanguageId` (offset 0x34) exists as the natural
  persistence slot if ever wanted.
- `func_8004B76C`-family (dead GsSPRITE glyph funcs, cx=304/v=240): no
  callers on US or EUR; left untouched.

## 6. Future: NTSC-J (JAP0/JAP1) roadmap notes

- File tables already in-tree (`filetable.c.JAP0/JAP1.inc`, same shape/names
  as USA — wholesale copy, identity path map); XA locs in fileinfo.c #else
  branch ready to lift; serial prefixes SLPS/SLPM/SIPS already in the
  launcher probe table (game-side Pc_DetectRegionFromBin needs them added).
- JAP0 vs JAP1 cannot be disambiguated by boot serial (same SLPM) — needs an
  EXE-size/table probe.
- JAP FONT16 is US-shaped (33 blocks) but text rendering is the structurally
  different `text_draw_jp.c` (SJIS/kanji rasterize-to-VRAM, currently
  excluded from the PC build in pc_port/CMakeLists.txt:114) + different
  overlay bases (g_OvlDynamic 0x800CBAA8/0x800CBBD0) + compile-time NTSCJ
  forks in text_draw.h/item screens. "Regions are data" covers files/descs;
  the JP renderer needs runtime dispatch — plan a dedicated session.
- The region descriptor pattern to extend: e_GameRegion + per-region tables
  (file table, XA locs, font layout, overlay link base, desc patches,
  language list). Grep `Region_EUR` for the two-valued sites to generalize.

## 7. Key files

- `pc_port/src/font_region.c` / `pc_port/include/font_region.h` — layouts,
  widths, accent mapping, region desc patch.
- `src/bodyprog/text/text_draw.c` — the three draw sites (SH_PC_PORT).
- `src/main/fileinfo.c` — region tables + language redirect +
  Fs_EurFileLookup.
- `pc_port/src/lang_text.c` / `lang_text.h` — item text + map-message
  extraction/translation.
- `src/bodyprog/game_boot/game_boot.c` — per-map lang patch + font requeue.
- `src/bodyprog/sys/fs_screens.c` — pre-title font requeue.
- `src/bodyprog/gfx/world_draw.c`, `map_effects.c`, `src/maps/particle.c` —
  BG_ETC/FLAME/particle co-tenant patches.
- `pc_port/tools/decrypt_eur_overlay.py` — EUR overlay decrypter (the source
  of every desc/width/accent constant).
- `pc_port/launcher/SilentHillPC_Launcher/DiscProbe.cs` — serial probe.
- Probe reports (session scratchpad, not committed): decrypt-tool /
  tim-census / lang-census / fmv-verify summaries; reference renders in
  `pc_port/build/reports/` (font16_PAL_grid.png, TIPS language renders,
  BG_ETC reslice comparisons).
