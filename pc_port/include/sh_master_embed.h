/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_master_embed.h - the relay server (online_server/sh_master.c) compiled into
 * the game so a player can host co-op with no separate server process.
 *
 * sh_master.c built with -DSH_MASTER_EMBED drops its main(), signal handlers and
 * on-disk marker store, and exposes just these three entry points. The co-op UDP
 * client then points at 127.0.0.1 and talks to it exactly as it would a hosted
 * server, so there is one relay implementation, not two.
 *
 * ShMaster_Serve blocks until ShMaster_RequestStop clears the run flag, so it is
 * meant to be called on a dedicated thread (see sh_net_coopudp.c).
 */
#ifndef SH_MASTER_EMBED_H
#define SH_MASTER_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

void ShMaster_SetConfig(unsigned short port, int maxPlayers, const char* name);
int  ShMaster_Serve(void);     /* blocks until stopped; returns 0 on clean exit */
void ShMaster_RequestStop(void);
int  ShMaster_Running(void);   /* 1 while the socket is bound and serving */

#ifdef __cplusplus
}
#endif

#endif /* SH_MASTER_EMBED_H */
