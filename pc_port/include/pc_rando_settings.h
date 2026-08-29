/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_rando_settings.h - in-game randomizer settings panel.
 *
 * An overlay modelled on the achievement browser (pc_ra_browser.c): its own GL
 * program + stb_truetype text, drawn from the post-capture hook, updated on the
 * game thread. Opened in a live run by TAPPING the Map button; HOLDING the Map
 * button opens the real map instead. Navigable by keyboard, controller and mouse.
 */
#ifndef PC_RANDO_SETTINGS_H
#define PC_RANDO_SETTINGS_H

/* 1 while the panel owns input (open or animating shut). Gameplay must freeze
 * and yield input to it in that case. */
int  Pc_RandoSettings_IsOpen(void);

void Pc_RandoSettings_Open(void);
void Pc_RandoSettings_Close(void);

/* Per-frame input while open (game thread). The caller resolves the signals from
 * the player's own bindings: up/down move the selection, left/right adjust the
 * selected setting, confirm activates an action row, close dismisses (and saves
 * if anything changed). Mouse is read internally. */
void Pc_RandoSettings_Update(int up, int down, int left, int right, int confirm, int close);

/* Per-frame draw from the post-capture GL hook (cheap no-op while closed). */
void Pc_RandoSettings_Draw(void);

/* Map-button tap/hold arbiter — call every gameplay frame while a run is live and
 * the panel is closed, passing the Map button's press-edge and held state. A
 * quick tap opens the panel (returns RANDO_MAP_OPENED_SETTINGS); a hold past the
 * threshold returns RANDO_MAP_WANT_MAP so the caller opens the real map screen. */
enum
{
    RANDO_MAP_NONE = 0,
    RANDO_MAP_OPENED_SETTINGS,
    RANDO_MAP_WANT_MAP
};
int  Pc_RandoSettings_MapButtonArbiter(int mapClicked, int mapHeld);

#endif /* PC_RANDO_SETTINGS_H */
