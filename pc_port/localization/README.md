# Silent Hill - Localization / Translation

This folder holds the game's English script extracted for translation, plus the
tooling to regenerate it and (later) import a finished translation back.

## Files

| File | Purpose |
|------|---------|
| `SilentHill_EN_for_translation.txt` | **Send this to the translator.** One file with everything: human-readable English, one entry per `[KEY] / NOTE: / EN: / TR:` block, with a legend explaining the in-line control codes and length limits. The translator fills in the `TR:` lines. |
| `SilentHill_EN_raw.json` | Machine map of `KEY -> exact source string` (with the original `_`=space and `~`/`	` codes). Used to re-import a finished translation into the game's exact format. Don't send this to the translator. |
| `extract_text.py` | Regenerates both files from the source tree. Run with any Python 3: `python extract_text.py`. Re-run it whenever a menu row is added. |
| `import_translation.py` | Turns a filled-in file back into a runtime pack (`../assets/gamedata/lang/<code>.lang`). |
| `make_ru_template.py` | Writes `SilentHill_RU_for_review.txt`: the same file with an `RU:` line pre-filled from the Team Raccoon (ViT Co / Metallist) Russian disc, voiced-line subtitles from the same team's text-only release, and the port's own Russian menus where no disc has them. PC-port menus stay blank for the translator. |
| `SilentHill_RU_for_review.txt` | That output, for the Russian reviewer. |
| `make_update_template.py` | `python make_update_template.py pl` writes `SilentHill_PL_for_update.txt`: the template pre-filled from a shipping pack, so a translator only fills the empty lines (text added since their last pass). |
| `SilentHill_PL_translation.txt` | The Polish master the shipped `pl.lang` is built from. |

## What's covered (1869 entries)

- **Story / cutscene dialogue**: every `MAP_MESSAGES[]` across all 40 map scenes (`MAP<n>_S<nn>.<index>`).
- **Common messages**: Yes/No, item-pickup prompts, door messages (`COMMON.<index>`), from `include/maps/shared/map_msg_common.h`.
- **Menus & UI**: the `s_MenuTr[]` table in `pc_port/src/lang_menu.c`: title screen, options, pause, save/load, memory-card messages, inventory action buttons (Equip/Look/...), and the save-location names (Cafe, Church, Nowhere, ...). Keys `MENU.<text>`.
- **PC Options menu**: every row name and value label of the five PC Options pages in `src/screens/options/options.c`. The game looks these up by their English text exactly like the menus above, so they share the `MENU.<text>` keys. `NOTE:` lines give each one's character budget (label and value share a line).
- **Quick Options menu (F10)**, its **Cheats/Debug pages**, the **Controls panel** (key binding screen) and the **confirm boxes**: `pc_quick_options.c`, `pc_cheats.c`, `pc_bind_panel.c`, `control_style.c`, `pc_confirm_dialog.c`. Keys `QUICK.<text>` (spaces as `_`, `=` as `-`). These overlays draw with a TrueType font, so they take real spaces and any Unicode letter, and `{n}`/`{key}`/`{name}`/`{action}`/`{camera}` placeholders are filled in by the game.
- **Item names** and **item descriptions**: `INVENTORY_ITEM_NAMES[]` / `g_ItemDescriptions[]` in `src/bodyprog/items/item_screens_3.c`. Keys `ITEM_NAME.<index>` / `ITEM_DESC.<index>` (index = item id).
- **Results screen** after the credits (`ranking.c`): play statistics and rank. `MENU.<text>` keys, with the room before each value as the budget.
- **Inventory prompts and labels** drawn by `Gfx_Inventory_ItemDescriptionDraw` ("Can't use it here.", Stock:, Fuel:, ==On==...). They go through the same menu lookup, so they are `MENU.<text>` keys (`=` written as `-`). The old `MISC.0`/`MISC.1` keys were hand-typed stand-ins the game never read; the importer maps them onto the real keys.

Not included on purpose: bare numbers (`30`, `2x`, `5.1`), map ids and monster/character names on the debug pages, and keyboard key names.

Text baked into images (e.g. the title logo, some signs) is **not** here; that's
handled separately as texture/art work.

## Where the PC overlays get their text

The quick menu, the controls panel and the confirm boxes draw with a TrueType
font, so they take UTF-8 rather than the game font's bytes.
`pc_port/src/lang_quick.c` serves them, picking the language the same way the
menus do: a Russian-patched disc first, then a pack (`QUICK.` keys, and the
UTF-8 copy the loader keeps of every `MENU.` value), then the German / French /
Spanish / Italian columns (`lang_quick_pal.inc` for `QUICK.` text, `s_MenuTr`
for PC Options rows). Japanese discs keep English.

## Source of truth

The strings are the US/NTSC English script (`VERSION_REGION_IS(NTSC)` branches),
i.e. the game's original text. `_` in the source means a space; the extractor
decodes those for readability and preserves the `~…` control codes (see the
legend at the top of the .txt).

## Adding a language (later)

The PAL build already supports DE/FR/ES/IT. A brand-new language that isn't on
any disc (e.g. Portuguese) gets added PC-side:

1. Translator returns `SilentHill_EN_for_translation.txt` with the `TR:` lines filled. (Older files with `PT:` lines, a language code such as `RU:`, or the retired `PCOPT.`/`PCOPT_VAL.` keys import too.)
2. A re-import step aligns each translated `TR:` line to its `KEY` and rebuilds
   the exact source format using `SilentHill_EN_raw.json` (spaces → `_`, control
   codes and their positions preserved), producing per-language tables:
   - map messages → a Portuguese `MAP_MESSAGES` equivalent loaded at runtime,
   - menu/UI → a new column in `s_MenuTr[]` (`lang_menu.c`),
   - item name/description → the `lang_text.c` per-language path.
3. Wire the new language into the launcher/`language` config selector.

Keep `raw.json` and the `[KEY]`s stable so a partially-translated file can be
re-imported incrementally.

## Polish (`language = pl`) - the worked example

Polish is the first fully wired PC-side pack language (EUR discs only). The
pipeline above is realised by:

| File | Purpose |
|------|---------|
| `import_translation.py` | Re-import: `SilentHill_PL_translation.txt` (+`_raw.json`) → `../assets/gamedata/lang/pl.lang` (the runtime pack). Run: `python import_translation.py --in <translated.txt> --code pl --name Polish --menu Polish`. |
| `add_polish_glyphs.py` | Adds the Polish letterforms to an HD-font pack's `FONT16.png` so the HD font and Polish work together (see below). |

`SilentHill_PL_translation.txt` is the Polish master: the translator's
September pass (Quick Options, Controls panel, PC Options and a revision of
the script) plus the results screen. Regenerate the pack from it:

    python import_translation.py --in SilentHill_PL_translation.txt --code pl --name Polish --menu Polish

The pack is loaded by `pc_port/src/lang_pack.c`; the extra letters (ą ę ł ż ó
ć ś ń ź + capitals) are drawn by `font_region.c` - most compose from the
existing accent marks, seven are painted into free FONT16 atlas cells.

### Polish + an HD font pack

A loose HD-font override (`gamedata/load/1ST/FONT16.png`) replaces the whole
FONT16 texture, so the seven *built* Polish glyphs (which the port paints into
the low-res disc atlas) would come out blank. `add_polish_glyphs.py` fixes this
by painting the same seven glyphs into the HD PNG at full resolution - copying
each HD base letter and adding a matching diacritic (ink/shadow sampled from
the font). The composed accents (ć ś ń ź ó + caps) already work with any HD
pack, since they stack the pack's own HD acute mark at draw time.

    python add_polish_glyphs.py "gamedata/load/1ST/FONT16.png"   # in place, keeps a .orig backup

Re-run it whenever the HD font pack is updated. Only needed for the EU-layout
HD font (Polish is EUR-only); a US-layout pack won't apply on a EUR disc.
