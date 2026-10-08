/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MSG_VOICE_H
#define PC_MSG_VOICE_H

/* Voice files for text boxes the game never voiced:
 *   gamedata/load/XA/msg_<KEY>.wav
 * KEY is the language-pack message key with '.' as '_' (MAP1_S00_23, COMMON_5).
 * A page without its own file keeps the previous file playing, so one take can
 * cover a whole multi-page message; the box closing always stops it. */

/* 1 while Gfx_MapMsg_Draw runs for a message the game voices itself (the
 * *WithAudio wrappers); those never get a loose file. */
extern int g_PcMapMsgGameVoiced;

void Pc_MsgVoice_Touch(void);        /* every Gfx_MapMsg_Draw call */
void Pc_MsgVoice_OnPage(int msgIdx); /* message start and each genuine page turn */
void Pc_MsgVoice_OnEnd(void);
void Pc_MsgVoice_Update(void);       /* once per game frame */

/* 1 while the page that STARTED a loose voice file is still owed screen time:
 * the clip is still producing audio, or it finished less than msg_voice_tail_ms
 * ago. The message display holds its auto-advance on this, so an added voice is
 * never cut off by the authored ~J timer, which was written for a silent line.
 * A later page riding the same take does NOT hold -- one recording is allowed
 * to span several pages, and those keep their authored pacing. */
int Pc_MsgVoice_Holding(void);

#endif
