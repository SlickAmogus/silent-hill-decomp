/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_BINDS_H
#define PC_BINDS_H

#ifdef SH_PC_PORT

/* Console key binds: "bind k kill;spawn groaner" runs those console commands
 * when K is pressed in game.
 *
 * Deliberately its own system, with no connection to the control-scheme binds
 * in pc_config.h. Those are per camera mode, conflict-checked and rewritten by
 * the bind panel; these are none of that. A key already driving Harry can carry
 * one too, editing or clearing the control-scheme binds never touches these,
 * and these never appear in the bind panel.
 *
 * They persist to config.cfg as the lines the user typed, so a bind set can be
 * pasted into a message and back into a config. */

#define PC_BIND_MAX      48
#define PC_BIND_KEY_CAP  32
#define PC_BIND_CMDS_CAP 192

/** `bind` console command. `arg` is everything after the word BIND:
 *  ""                 - usage
 *  "LIST"             - list the current binds
 *  "<key> <commands>" - set one, replacing any bind already on that key */
void PcBinds_CmdBind(const char* arg);

/** `unbind <key>` / `unbindall`. */
void PcBinds_CmdUnbind(const char* arg);
void PcBinds_CmdUnbindAll(void);

/** One config.cfg line, without the leading "bind ". Load-time only: it does
 * not save, so loading the file cannot rewrite it. */
void PcBinds_ParseConfigLine(const char* rest);

/** Fires bound keys. Call once per frame with the SDL keyboard state, only
 * when the game should be listening (console closed, no menu up). */
void PcBinds_Update(const unsigned char* keyState);

/** Number of binds, and line `i` rendered exactly as it is written to the
 * config ("bind K KILL;SPAWN GROANER"). For the config writer. */
int         PcBinds_Count(void);
const char* PcBinds_Line(int i);

#endif /* SH_PC_PORT */
#endif /* PC_BINDS_H */
