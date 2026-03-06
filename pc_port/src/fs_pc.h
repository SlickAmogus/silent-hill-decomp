/*
 * fs_pc.h - PC File System abstraction
 *
 * Replaces PSX CD-ROM file access with standard file I/O.
 * Game assets can be read from:
 * 1. An extracted directory structure matching the CD layout
 * 2. A BIN/CUE CD image (via PsyCross CDFS)
 */
#ifndef _FS_PC_H
#define _FS_PC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the PC file system with the game data path */
int FsPC_Init(const char* dataPath);

/* Read a file by its CD path (e.g., "1ST\\BODYPROG.BIN") */
int FsPC_ReadFile(const char* path, void* dest, size_t maxSize);

/* Get file size */
size_t FsPC_GetFileSize(const char* path);

/* Check if a file exists */
int FsPC_FileExists(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* _FS_PC_H */
