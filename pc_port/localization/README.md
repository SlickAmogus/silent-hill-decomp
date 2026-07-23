# Silent Hill — Localization / Translation

This folder holds the game's English script extracted for translation, plus the
tooling to regenerate it and (later) import a finished translation back.

## Files

| File | Purpose |
|------|---------|
| `SilentHill_EN_for_translation.txt` | **Send this to the translator.** Human-readable English, one entry per `[KEY] / EN: / PT:` block, with a legend explaining the in-line control codes. The translator fills in the `PT:` lines. |
| `SilentHill_EN_raw.json` | Machine map of `KEY -> exact source string` (with the original `_`=space and `~`/`\t` codes). Used to re-import a finished translation into the game's exact format. Don't send this to the translator. |
| `extract_text.py` | Regenerates both files from the source tree. Run with any Python 3: `python extract_text.py`. |

## What's covered (1646 entries)

- **Story / cutscene dialogue** — every `MAP_MESSAGES[]` across all 40 map scenes (`MAP<n>_S<nn>.<index>`).
- **Common messages** — Yes/No, item-pickup prompts, door messages (`COMMON.<index>`), from `include/maps/shared/map_msg_common.h`.
- **Menus & UI** — the full `s_MenuTr[]` table in `pc_port/src/lang_menu.c`: title screen, options, pause, save/load, memory-card messages, inventory action buttons (Equip/Look/…), and the save-location names (Cafe, Church, Nowhere, …). Keys `MENU.<text>`.
- **Item names** and **item descriptions** — `INVENTORY_ITEM_NAMES[]` / `g_ItemDescriptions[]` in `src/bodyprog/items/item_screens_3.c`. Keys `ITEM_NAME.<index>` / `ITEM_DESC.<index>` (index = item id).
- **Other prompts** — a couple of item-use prompts not in `s_MenuTr` (`MISC.<n>`).

Text baked into images (e.g. the title logo, some signs) is **not** here — that's
handled separately as texture/art work.

## Source of truth

The strings are the US/NTSC English script (`VERSION_REGION_IS(NTSC)` branches),
i.e. the game's original text. `_` in the source means a space; the extractor
decodes those for readability and preserves the `~…` control codes (see the
legend at the top of the .txt).

## Adding a language (later)

The PAL build already supports DE/FR/ES/IT. A brand-new language that isn't on
any disc (e.g. Portuguese) gets added PC-side:

1. Translator returns `SilentHill_EN_for_translation.txt` with the `PT:` lines filled.
2. A re-import step aligns each translated `PT:` line to its `KEY` and rebuilds
   the exact source format using `SilentHill_EN_raw.json` (spaces → `_`, control
   codes and their positions preserved), producing per-language tables:
   - map messages → a Portuguese `MAP_MESSAGES` equivalent loaded at runtime,
   - menu/UI → a new column in `s_MenuTr[]` (`lang_menu.c`),
   - item name/description → the `lang_text.c` per-language path.
3. Wire the new language into the launcher/`language` config selector.

Keep `raw.json` and the `[KEY]`s stable so a partially-translated file can be
re-imported incrementally.

## Polish (`language = pl`) — the worked example

Polish is the first fully wired PC-side pack language (EUR discs only). The
pipeline above is realised by:

| File | Purpose |
|------|---------|
| `import_translation.py` | Re-import: `SilentHill_PL_translation.txt` (+`_raw.json`) → `../assets/gamedata/lang/pl.lang` (the runtime pack). Run: `python import_translation.py --in <translated.txt> --code pl --name Polish --menu POLISH`. |
| `add_polish_glyphs.py` | Adds the Polish letterforms to an HD-font pack's `FONT16.png` so the HD font and Polish work together (see below). |

The pack is loaded by `pc_port/src/lang_pack.c`; the extra letters (ą ę ł ż ó
ć ś ń ź + capitals) are drawn by `font_region.c` — most compose from the
existing accent marks, seven are painted into free FONT16 atlas cells.

### Polish + an HD font pack

A loose HD-font override (`gamedata/load/1ST/FONT16.png`) replaces the whole
FONT16 texture, so the seven *built* Polish glyphs (which the port paints into
the low-res disc atlas) would come out blank. `add_polish_glyphs.py` fixes this
by painting the same seven glyphs into the HD PNG at full resolution — copying
each HD base letter and adding a matching diacritic (ink/shadow sampled from
the font). The composed accents (ć ś ń ź ó + caps) already work with any HD
pack, since they stack the pack's own HD acute mark at draw time.

    python add_polish_glyphs.py "gamedata/load/1ST/FONT16.png"   # in place, keeps a .orig backup

Re-run it whenever the HD font pack is updated. Only needed for the EU-layout
HD font (Polish is EUR-only); a US-layout pack won't apply on a EUR disc.
