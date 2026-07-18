#ifndef LANG_TEXT_H
#define LANG_TEXT_H

/* PAL language text (config `language`, EUR discs): item names/descriptions
 * come from the disc's VIN/ITEM_<lang>.BIN, in-map messages from the
 * VIN/VIN2..VIN5 localized overlays (file table already redirected by
 * Fs_InitFileTableForRegion). This includes ENGLISH: PAL-EN is a distinct
 * retranslation of the US script, so a PAL disc always shows its own text.
 * PAL text uses a different markup dialect than the compiled US strings
 * ({X} brace codes, real spaces, literal newlines, Latin-1 accents) —
 * everything is translated to the US dialect at load so the stock renderer
 * draws it; accent bytes pass through raw and resolve in text_draw via the
 * region font layout (font_region.c). */

/* Non-zero when an EUR disc is active (localized text pipeline in use). */
int Pc_LangActive(void);

/* Non-zero when a fan-translated (modified) USA disc was detected: its
 * BODYPROG kerning table or item text differed from the compiled originals.
 * Story text self-detects per map in Pc_LangPatchMapMessages regardless.
 * Also unlocks the port's menu translations (lang_menu.c) on USA discs via
 * the `language` config key. */
int Pc_FanTextActive(void);

/* Non-zero when the options menu should show the Language row (EUR disc +
 * menu entered from the title screen). */
int Pc_LangMenuRowActive(void);

/* Live language switch (0=en 1=de 2=fr 3=es 4=it): persists the config key,
 * rebinds the file table, reloads item text. Title-screen options only. */
void Pc_LangSetLanguage(int lang);

/* Load + parse ITEM_<lang>.BIN once (call after Fs_InitFileTableForRegion). */
void Pc_LangInit(void);

/* Localized item text for inventory index 0..194, or NULL to use the US
 * string (PAL leaves some entries untranslated/NULL — English fallback). */
const char* Pc_LangItemName(int itemIdx);
const char* Pc_LangItemDesc(int itemIdx);

/* After the map overlay BIN finished loading into `ovl` (g_OvlDynamic):
 * extract the localized message table and repoint the active map header at
 * translated strings. No-op unless Pc_LangActive(). */
void Pc_LangPatchMapMessages(int mapIdx, void* ovl, unsigned int ovlSize);

/* Port-written menu translations (lang_menu.c — retail PAL kept every menu
 * English, the disc has no menu strings to reuse). Called from the
 * Gfx_StringDraw chokepoint: returns the translated string when one exists
 * for the active language, else `str` unchanged (US discs: always
 * unchanged). Pc_LangMenuTextWidth measures a (first line of a) menu string
 * in pixels for the centered title/difficulty entries. */
const char* Pc_LangMenuText(const char* str);
int         Pc_LangMenuTextWidth(const char* str);

#endif
