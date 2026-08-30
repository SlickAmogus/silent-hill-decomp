/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_ui.h - the online client's screen-space UI.
 *
 * All of it is drawn from the PsyX post-capture hook (DbgOverlay_Render), with
 * its own GL program, VAO and atlas; see the file header in sh_net_ui.c for
 * the constraints that arrangement exists to satisfy.
 */
#ifndef SH_NET_UI_H
#define SH_NET_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while the player list is up. Read by the input layer, which
 * swallows the pad while a panel owns it. */
extern int g_ShNetPlayerListOpen;

/* Claim the GL objects on the first frame a context exists, long before a map
 * load starts churning framebuffer targets. A texture allocated later can be
 * handed a recycled name a framebuffer still refers to, and that pass then
 * renders the scene into the panel's glyphs. */
void ShNetUi_PreloadGL(void);

void ShNetUi_TogglePlayerList(void);

/* Per-frame draw from the post-capture hook. Cheap no-op when there is
 * nothing to show. */
void ShNetUi_Draw(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_UI_H */
