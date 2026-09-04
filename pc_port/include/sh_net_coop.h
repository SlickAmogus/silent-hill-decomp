/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_coop.h - "somebody else is physically in this world".
 *
 * THE DISTINCTION THIS HEADER EXISTS TO DRAW. The port has two online systems
 * and only one of them changes how the game may behave:
 *
 *   THE LIVING WORLD (master server, sh_net.h). Ghosts and messages from
 *   strangers. It is a PICTURE of somebody else's game painted into yours.
 *   Nothing you do affects them and nothing they do affects you, so pausing,
 *   opening the inventory, or alt-tabbing for ten minutes is entirely your
 *   business. It is deliberately seamless: there is no connect step, no lobby,
 *   no waiting for anyone.
 *
 *   A CO-OP SESSION (Steam, sh_net_session.h). Another player is IN your world,
 *   or you are in theirs. Now your local clock is shared. Pausing would stop a
 *   world somebody else is standing in.
 *
 * g_ShNetCoopActive is 1 only in the second case. The ghost world must never
 * set it, however many ghosts are on screen.
 *
 * WHAT IT DOES TODAY: blocks the pause button. That is the whole of it, and it
 * is deliberately the whole of it -- the flag is the contract, and the list of
 * things that consult it grows as co-op does.
 *
 * WHAT SETS IT TODAY: nothing in gameplay, because joining another player's
 * world is not implemented yet. The session layer sets it once both sides have
 * agreed they are in one world (ShSession's world handshake), and the console
 * command `net coop 1` forces it so the suppression can be tested now. That is
 * an honest split: the mechanism is real and exercised, the trigger is waiting
 * on the feature it belongs to.
 */
#ifndef SH_NET_COOP_H
#define SH_NET_COOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Read it through ShNet_CoopActive() from anywhere. The global is exposed
 * because the decomp's gameplay files reach for externs directly and a call
 * through a header they do not include would be the odd one out. */
extern int g_ShNetCoopActive;

int  ShNet_CoopActive(void);

/* The seam. `why` is a short phrase for the log, e.g. "guest joined". */
void ShNet_SetCoopActive(int active, const char* why);

/* 1 when the pause button must be ignored. Its own function rather than a
 * bare read of the flag, so that when co-op grows a "host may pause, guests
 * may not" rule there is one place to put it. */
int  ShNet_PauseBlocked(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_COOP_H */
