/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Chinese text for NTSC-J, supplied by gamedata/lang/zh.pack.
 *
 * Lets a RETAIL Japanese disc show the Chinese translation and switch to it in
 * game: the port already carries the Chinese glyphs, and this carries the words
 * that the translation otherwise only ships as a disc PPF. See lang_zh.c for
 * the format and pc_port/tools/gen_zh_pack.py for how the file is built.
 *
 * Every getter returns NULL when the pack is absent, so callers fall back to
 * whatever the disc gave them by testing for NULL and nothing else. */
#ifndef PC_LANG_ZH_H
#define PC_LANG_ZH_H

/** Loads the pack on first call. 0 = no usable pack; the game keeps Japanese. */
int Pc_LangZhAvailable(void);

/** Inventory text, indexed exactly like the compiled JP tables. NULL if absent. */
const char* Pc_LangZhItemName(int idx);
const char* Pc_LangZhItemDesc(int idx);

/** Map story text, in the DISC's (JP) message order — the caller still applies
 * the port's US->JP index map (lang_jpn_msgmap.inc) on top. */
int         Pc_LangZhMapMessageCount(int mapIdx);
const char* Pc_LangZhMapMessage(int mapIdx, int msgIdx);

#endif /* PC_LANG_ZH_H */
