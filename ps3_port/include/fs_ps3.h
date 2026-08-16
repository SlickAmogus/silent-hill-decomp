/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fs_ps3.h - where the game's data lives on a PS3, resolved once at boot.
 *
 * Plain C types only: this is included by TUs on both sides of the PSL1GHT /
 * decomp-types boundary described in ps3_hal.h.
 */
#ifndef FS_PS3_H
#define FS_PS3_H

/* lv2 paths are absolute and long ("/dev_hdd0/game/SHPS30001/USRDIR/"), so this
 * is generous compared with the 360's, whose devoptab roots are four chars. */
#define SH_PS3_PATH_MAX 256

#ifdef __cplusplus
extern "C" {
#endif

/* Returns non-zero if a .bin disc image was found. Safe to call more than once;
 * the search runs only on the first call. */
int Sh3Fs_Init(void);

/* Directory holding the disc image, with a trailing slash. Empty until a
 * successful Sh3Fs_Init. */
const char* Sh3Fs_DataRoot(void);

/* Filename of the disc image within the data root, or "" if none was found. */
const char* Sh3Fs_BinName(void);

/* Directory for save data, created if absent. Trailing slash. */
const char* Sh3Fs_SaveDir(void);

#ifdef __cplusplus
}
#endif

#endif /* FS_PS3_H */
