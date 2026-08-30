/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_DISCORD_H
#define PC_DISCORD_H

/* Discord Rich Presence. Init once after config load, Update once per frame,
 * Shutdown at exit. All three are safe no-ops when the feature is disabled
 * (config discord_rich_presence = 0) or no Discord application id is set. */
void Pc_Discord_Init(void);
void Pc_Discord_Update(void);
void Pc_Discord_Shutdown(void);
/* Player-facing area name for an e_MapIdx, e.g. "Midwich Elementary School".
 * Lives in pc_discord.c because the presence line needed it first; the online
 * player list shows the same names. */
const char* Pc_MapAreaName(int idx);


#endif /* PC_DISCORD_H */
