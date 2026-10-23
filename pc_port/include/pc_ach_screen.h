/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ACH_SCREEN_H
#define PC_ACH_SCREEN_H

#include <stddef.h>

#include "pc_native_menu.h"

/* Native achievements screen (list view and single view), drawn with PSX
 * primitives inside the title's 448-line screen. Game thread only. */

void Pc_AchScreen_Open(void);
int  Pc_AchScreen_IsOpen(void);
/* 1 while open; 0 once the player backs out of the list. The screen stays
 * open (and drawable) until Pc_AchScreen_Close. */
int  Pc_AchScreen_Update(const NmInput* in);
void Pc_AchScreen_Close(void);
void Pc_AchScreen_Draw(void);
/* PNG bytes of a downloaded badge; called for every badge the RA queue delivers. */
void Pc_AchScreen_ProvideBadge(const char* badge, const unsigned char* png, size_t len);

#endif
