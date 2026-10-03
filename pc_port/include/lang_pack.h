/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LANG_PACK_H
#define LANG_PACK_H

#include <stddef.h>

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
 *   MENU.<us literal>      menu/UI, keyed by the compiled string minus \x01,
 *                          '=' written as '-'
 *   QUICK.<english>        TrueType overlays (quick menu, controls panel,
 *                          confirm boxes); ' ' written as '_', '=' as '-'
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

/* The glyph set a pack needs, from its `!font` header line. A language whose
 * letters the retail PAL atlas already draws needs nothing; the others bring
 * their letterforms with them (font_region.c). */
#define LANG_PACK_FONT_LATIN    0   /* `!font=latin`, or absent */
#define LANG_PACK_FONT_POLISH   1   /* `!font=polish`   -- seven cells built in */
#define LANG_PACK_FONT_CYRILLIC 2   /* `!font=cyrillic` -- atlas replaced */
/* `!font=sjis`: the values are already the engine's own bytes (Shift-JIS, which
 * the embedded kanji font is indexed by), so the loader passes them through
 * untouched instead of transcoding from UTF-8. */
#define LANG_PACK_FONT_SJIS     3
/* `!font=chinese`: the same encoding as sjis -- the Chinese translation writes
 * ordinary JIS kuten codes -- but drawn through the port's embedded Chinese
 * glyph table, which redefines those codes. So values pass through exactly as
 * for sjis, and only the font switched on differs. */
#define LANG_PACK_FONT_CHINESE  4

/* The two that index the embedded kanji font rather than the glyph atlas. */
#define LangPackFontIsKanji(f) ((f) == LANG_PACK_FONT_SJIS || (f) == LANG_PACK_FONT_CHINESE)

int Pc_LangPackFont(void);

/* Installed packs, in a stable order: every .lang file in gamedata/lang. This
 * is how a language MOD works -- the Mod Manager drops the file there and it
 * becomes selectable. Scanned once, on first use; Rescan forces a re-read.
 * Slot N of the Language row is LANG_PACK_FIRST + N. */
int         Pc_LangPackListCount(void);
const char* Pc_LangPackListCode(int idx);
const char* Pc_LangPackListName(int idx);
int         Pc_LangPackListFind(const char* code);   /* index, or -1 */
int         Pc_LangPackListFont(int idx);            /* LANG_PACK_FONT_* */
void        Pc_LangPackListRescan(void);

/* Label for the options-menu Language row (the pack's `!menu` field). */
const char* Pc_LangPackName(void);

/* NULL when the key is absent -- callers fall back to the compiled English. */
const char* Pc_LangPackGet(const char* key);
const char* Pc_LangPackMapMsg(int mapIdx, int msgIdx);
/* The pack key of a map message ("MAP1_S00.23", "COMMON.5"); 0 when the map
 * index carries no text. Independent of whether a pack is loaded. */
int Pc_LangPackMsgKey(int mapIdx, int msgIdx, char* out, size_t outSize);
const char* Pc_LangPackItemName(int itemIdx);
const char* Pc_LangPackItemDesc(int itemIdx);

/* Menu lookup keyed by the compiled US literal, matching the Gfx_StringDraw
 * chokepoint in lang_menu.c. Handles the \x01 kerning bytes itself. */
const char* Pc_LangPackMenu(const char* us);

/* The same entries as UTF-8 text, untouched by the font transcode, for the
 * PC overlays that draw with a TrueType font (quick menu, controls panel).
 * Kept for MENU.* and QUICK.* keys only; NULL when absent. */
const char* Pc_LangPackUtf8(const char* key);
const char* Pc_LangPackMenuUtf8(const char* us);

#endif
