/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_qol_update.h - offers updates for the Essential QoL mods the launcher's
 * Mod Manager installed (desktop builds). Android, iOS and the web build have
 * their own installer, pc_qol_mods.c, which asks the same question there; on
 * those this module is empty.
 *
 * The first time a new build reaches the title screen it reads the catalog,
 * and when a mod the player has is out of date it asks: Yes, No (ask again
 * next time) or Don't ask again.
 */
#ifndef PC_QOL_UPDATE_H
#define PC_QOL_UPDATE_H

/* Once per frame while the title's main menu is up. Returns 1 while its
 * question, its progress notice or its result owns the input; the caller then
 * drops the frame's pad and mouse input. */
int Pc_QolUpdate_TitleFrame(int left, int right, int confirm, int cancel);

/* 1 while one of those is on screen. */
int Pc_QolUpdate_IsOpen(void);

#endif /* PC_QOL_UPDATE_H */
