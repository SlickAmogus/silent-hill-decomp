/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_qol_mods.h - Essential QoL Mods installer for the builds that have no
 * launcher (Android, iOS, web).
 *
 * The catalog and the mod zips live in the qol-mods GitHub release, the same
 * one the desktop launcher's Mod Manager reads (essential-mods.json). This
 * module offers them once on first boot, lists them from the platform options
 * menu, downloads the ones ticked and installs them into gamedata/.
 *
 * Overlay in the pc_rando_settings.c mould: own GL program + stb_truetype,
 * drawn from the post-capture hook, input fed by the game thread.
 */
#ifndef PC_QOL_MODS_H
#define PC_QOL_MODS_H

/* 1 while the window (or the first-boot prompt) is up and owns input. */
int  Pc_QolMods_IsOpen(void);

/* The platform options row: open the mod list. */
void Pc_QolMods_OpenList(void);

/* Once per frame on the game thread, after the pad is read. atTitle = the
 * title menu is up, which is where the first-boot prompt waits to appear.
 * Feeds input while open; returns 1 when it consumed this frame's input (the
 * caller then clears the pad flags). */
int  Pc_QolMods_Frame(int atTitle, int up, int down, int left, int right, int confirm, int cancel);

/* Post-capture GL hook. Cheap no-op while closed. */
void Pc_QolMods_Draw(void);

/* Native builds: blocking download of url to path, on a worker thread.
 * Returns the HTTP status (200 = the file at path is complete), 0 on no
 * response. Progress may be published through got (bytes so far); a backend
 * that cannot leaves it alone and the size of path is polled instead. */
int  Pc_QolHttpDownload(const char* url, const char* path, volatile long long* got);

#endif /* PC_QOL_MODS_H */
