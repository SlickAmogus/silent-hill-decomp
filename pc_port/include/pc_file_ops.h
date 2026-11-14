/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_file_ops.h - file operations done the way the launcher does them.
 *
 * The Mod Manager (C#) tells a file it deployed from one the player edited by
 * size and last-write time, and it copies with File.Copy, which keeps the
 * source's time. Anything the game writes into the folders the Mod Manager owns
 * has to agree with that to the second, so these ask the OS directly instead of
 * going through the C runtime's stat()/utime().
 *
 * Implemented in pc_file_ops.c, which includes <windows.h> and therefore
 * nothing from the game.
 */
#ifndef PC_FILE_OPS_H
#define PC_FILE_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Size and last-write time of a file. The time is in .NET ticks, UTC (100 ns
 * steps from 0001-01-01): the unit mods/deployed.txt records. 0 if there is no
 * such file. Either out pointer may be NULL. */
int PcFile_Stat(const char* path, long long* size, long long* ticks);

/* Copy a file over `to`, last-write time included. 1 on success. */
int PcFile_CopyKeepTime(const char* from, const char* to);

/* Move a file or a folder onto `to`, replacing a file already there. 1 on
 * success, 0 when the OS cannot do it in one step (the caller then copies). */
int PcFile_MoveReplace(const char* from, const char* to);

#ifdef __cplusplus
}
#endif

#endif /* PC_FILE_OPS_H */
