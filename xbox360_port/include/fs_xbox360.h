/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FS_XBOX360_H
#define FS_XBOX360_H

#include <stdio.h>

/* libfat volume names are short ("uda:/"), so paths here stay well under this. */
#define SH360_PATH_MAX 128

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts FAT volumes and picks the directory holding the .bin disc image.
 * Returns 1 when a .bin was found, 0 when the root fell back to a writable
 * default so logging still works. Idempotent. */
int Sh360Fs_Init(void);

const char* Sh360Fs_DataRoot(void);   /* e.g. "uda:/silenthill/", trailing slash */
const char* Sh360Fs_BinName(void);    /* bare filename, "" if none found */

/* Required after writing anything worth keeping -- see the comment on the
 * definition. Without it a hard power-off loses the file entirely. */
void Sh360Fs_Sync(FILE* f);

/* Kept under the Xbox name because the reused mcard_xbox.c calls it. */
int XboxFs_ResolveSaveDir(char* out, int outSize);

#ifdef __cplusplus
}
#endif

#endif /* FS_XBOX360_H */
