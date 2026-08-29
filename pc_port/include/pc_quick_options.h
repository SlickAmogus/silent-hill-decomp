/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_quick_options.h - in-game quick options overlay (key_quick_options, default F10).
 *
 * A translucent GL panel modelled on the randomizer settings panel
 * (pc_rando_settings.c): own GL program + stb_truetype text, drawn from the
 * post-capture hook, updated on the game thread. Rows are the live-applying
 * subset of the PC Options screen (driven through the PcOpt_Quick* API in
 * options.c, so labels / cycling / config saving are the same code) plus a few
 * settings that live elsewhere (shadow resolution, speaker layout, volumes,
 * and the View & Aspect page's display-aspect / CRT trim / hfov / vfov / par).
 * While open the game is frozen the way the console freezes it -- every site
 * that reads g_PcConsoleInputActive also reads g_PcQuickOptionsActive.
 */
#ifndef PC_QUICK_OPTIONS_H
#define PC_QUICK_OPTIONS_H

/* 1 while the panel owns input (open or animating shut). */
int  Pc_QuickOptions_IsOpen(void);
/* Same as IsOpen, as a plain global for the freeze sites (game_main.c,
 * control_style.c, pc_combat.c) that already read g_PcConsoleInputActive. */
extern int g_PcQuickOptionsActive;

void Pc_QuickOptions_Toggle(void);
void Pc_QuickOptions_Close(void);

/* Per-frame input while open (game thread). up/down/left/right are HELD
 * state (the overlay repeats them on its own clock); confirm/close/page are
 * press edges from the player's own bindings. Keyboard (arrows via the pad emulation, Esc / the bound key /
 * PgUp / PgDn / Q / E) and the mouse are read internally. */
void Pc_QuickOptions_Update(int up, int down, int left, int right,
                            int confirm, int close, int pageNext, int pagePrev);

/* Per-frame draw from the post-capture GL hook (cheap no-op while closed). */
void Pc_QuickOptions_Draw(void);

#endif /* PC_QUICK_OPTIONS_H */
