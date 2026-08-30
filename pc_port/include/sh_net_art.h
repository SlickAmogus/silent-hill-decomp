/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_art.h - the online client's generated textures.
 *
 * Kept in its own translation unit, free of every game header, for one
 * reason: art you cannot look at is art you cannot fix. online_server/
 * ghost_art_dump.c links this same code and writes the results to PNG, so
 * "does the silhouette read as a person" is answered by looking at a picture
 * rather than by launching the game.
 *
 * Both images are WHITE with the shape carried entirely in alpha, so the
 * drawing prim's own RGB is what colours them and one texture serves every
 * ghost colour and every marker kind.
 *
 * Caller owns the returned buffer (free()). NULL on allocation failure.
 */
#ifndef SH_NET_ART_H
#define SH_NET_ART_H

#ifdef __cplusplus
extern "C" {
#endif

#define SHNET_ART_GHOST_W 64
#define SHNET_ART_GHOST_H 128
#define SHNET_ART_MEMO_W  64
#define SHNET_ART_MEMO_H  64

/* A hollow humanoid contour: the alpha band straddling the zero crossing of a
 * union-of-capsules distance field, plus a faint interior so the figure still
 * reads at the distance where the contour is two pixels wide. */
unsigned char* ShNetArt_MakeGhost(void);

/* A ring sigil with an inner ring and four cardinal ticks. */
unsigned char* ShNetArt_MakeMarker(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_ART_H */
