#ifndef LANG_PACK_H
#define LANG_PACK_H

/* PC-side language packs (gamedata/lang/<code>.lang).
 *
 * The five retail languages come off the PAL disc (lang_text.c reads
 * ITEM_<lang>.BIN and the VIN2..VIN5 map overlays). A language that ships on
 * no disc has nowhere to come from, so it is supplied as a data file built by
 * pc_port/localization/import_translation.py from a translator's filled-in
 * script.
 *
 * The file is UTF-8 `KEY=value`, keyed exactly as the extractor names things:
 *   COMMON.<0-14>          shared prompts (every map's message indices 0-14)
 *   MAP<n>_S<nn>.<idx>     per-map story text, indices 15+
 *   MENU.<us literal>      menu/UI, keyed by the compiled string minus \x01
 *   ITEM_NAME.<idx>        inventory names   (idx = item id - 32)
 *   ITEM_DESC.<idx>        inventory descriptions
 * Values are already in engine format ('_' = rendered space, ~X codes intact);
 * only the UTF-8 -> font-byte transcode happens at load. A missing key means
 * "keep English", so a partial pack is valid. */

/* Load gamedata/lang/<code>.lang. Returns 1 on success, 0 if absent or
 * unparseable (caller stays on English). Reloadable: a second call frees the
 * previous pack. */
int Pc_LangPackLoad(const char* code);

/* Drop the active pack (called by Load before installing a new one). */
void Pc_LangPackFree(void);

/* Non-zero once a pack is loaded and serving text. */
int Pc_LangPackActive(void);

/* Label for the options-menu Language row (the pack's `!menu` field). */
const char* Pc_LangPackName(void);

/* NULL when the key is absent -- callers fall back to the compiled English. */
const char* Pc_LangPackGet(const char* key);
const char* Pc_LangPackMapMsg(int mapIdx, int msgIdx);
const char* Pc_LangPackItemName(int itemIdx);
const char* Pc_LangPackItemDesc(int itemIdx);

/* Menu lookup keyed by the compiled US literal, matching the Gfx_StringDraw
 * chokepoint in lang_menu.c. Handles the \x01 kerning bytes itself. */
const char* Pc_LangPackMenu(const char* us);

#endif
