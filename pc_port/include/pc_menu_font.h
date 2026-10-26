/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MENU_FONT_H
#define PC_MENU_FONT_H

/* CPU-only native FONT16 access. No GL or menu state dependency. */
int Pc_MenuFontReady(void);
float Pc_MenuFontWidth(const char* text, float px);
/* Caller owns the malloc-allocated RGBA result. */
unsigned char* Pc_MenuFontRaster(const char* text, float px, int* width, int* height);

int Pc_MenuLanguageRussian(void);
int Pc_MenuLanguageSupported(void);

#endif
