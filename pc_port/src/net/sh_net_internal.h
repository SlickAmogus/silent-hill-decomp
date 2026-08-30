/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_internal.h - the seam between the protocol client and the game-side
 * glue.
 *
 * sh_net_client.c is deliberately free of every game header: it is pure
 * protocol, threading and state, and can be unit-tested without an engine.
 * sh_net_game.c is the half that knows what g_SysWork is. These two calls are
 * the entire interface between them.
 */
#ifndef SH_NET_INTERNAL_H
#define SH_NET_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game thread -> worker: this frame's local player state, published under the
 * lock. `valid` 0 means "not in the world right now" (menus, loading, the
 * title screen), which stops the server placing a ghost at a stale position. */
void ShNet_PublishLocal(int valid, int mapIdx, int charaId, int flags,
                        int x, int y, int z, short rotY, short health,
                        unsigned short animIdx, unsigned short animFrame);

/* Worker -> game thread: refresh the lock-free private copy that every
 * accessor in sh_net.h reads. */
void ShNet_PumpToGameThread(void);

/* Resolved once at Init from config; the UI reads it back for display. */
const char* ShNet_LocalName(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_INTERNAL_H */
