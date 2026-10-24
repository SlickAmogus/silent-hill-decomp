/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MENU_PREVIEW_H
#define PC_MENU_PREVIEW_H

/* A separate, disposable game process owns the showcase world. No pointers or
 * save data are shared with the player's game. */
int Pc_MenuPreview_IsChild(void);
/* Linux worker support, opt-out and startup failure; not a load completion flag. */
int Pc_MenuPreview_Available(void);
void Pc_MenuPreview_Configure(void);
void Pc_MenuPreview_Tick(void);
void Pc_MenuPreview_Capture(void);
void Pc_MenuPreview_Stop(void);
void Pc_MenuPreview_Camera(int mode);
/* Select a prepared scene by option key. Unsupported options have no image. */
void Pc_MenuPreview_Select(const char* key);
/* Start the default scene asynchronously when PC options/F10 opens. */
void Pc_MenuPreview_Preload(void);
/* Keep cached workers alive but asleep while a nonvisual row is selected. */
void Pc_MenuPreview_Idle(void);
/* Called only by the disposable renderer, never the player's world. */
void Pc_MenuPreview_WeatherDraw(void);
/* Returns RGBA pixels (bottom-up), or NULL while loading/unavailable. */
const unsigned char* Pc_MenuPreview_Frame(unsigned* serial, int* width, int* height);

#endif
