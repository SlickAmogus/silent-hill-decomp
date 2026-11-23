/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_BINDS_H
#define PC_BINDS_H

#ifdef SH_PC_PORT

/* Console command binds: "bind k kill;spawn groaner" runs those console
 * commands when K is pressed in game, "bind pad:dpup nextgun" does the same
 * for a controller input.
 *
 * Deliberately its own system, with no connection to the control-scheme binds
 * in pc_config.h. Those are per camera mode, conflict-checked and reset by the
 * controls screen; these are none of that. A key already driving Harry can
 * carry one too, and editing or clearing the control-scheme binds never
 * touches these.
 *
 * The controls screen lists them under its own rows, one per command, so they
 * can be seen and rebound there: the game's own (Next Gun, Next Melee Weapon,
 * unbound to start with), every command bound from the console, and any the
 * player added with addcontrol.
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

/** `addcontrol <commands>` / `removecontrol <commands>`: a row on the controls
 * screen for a command, bound or not. */
void PcBinds_CmdAddControl(const char* arg);
void PcBinds_CmdRemoveControl(const char* arg);

/** The controls screen's view: one row per command. Value is the key label or
 * controller input name, "" when unbound. RowSet takes the same, or NULL /
 * "NONE" to clear, and returns the row's index afterwards. */
int         PcBinds_RowCount(void);
const char* PcBinds_RowLabel(int row);
const char* PcBinds_RowValue(int row, int isPad);
int         PcBinds_RowSet(int row, int isPad, const char* value);

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
