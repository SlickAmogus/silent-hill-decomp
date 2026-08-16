/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * psx_io_console.h - keep the PSX BIOS file API from colliding with POSIX.
 *
 * The PSX BIOS exposes open/close/read/write/lseek/rename operating on
 * "buXX:NAME" memory-card paths, and the decomp calls them by those names.
 * mcard_xbox.c implements them. On the Original Xbox that was harmless: nxdk's
 * libc is Win32-backed and provides no POSIX syscalls of those names.
 *
 * libXenon (360) and PSL1GHT (PS3) are both newlib, whose stdio is built ON
 * those exact symbols. With both definitions present the link fails -- and if
 * the PSX ones had won instead, every fopen in the port would have been routed
 * into the memory-card handler, silently breaking the log and the disc image.
 * Both sets are genuinely needed, so the PSX ones are renamed.
 *
 * Started life as the 360's psx_io_xbox360.h. The name is neutral because the
 * hazard is newlib's, not any one console's, and a second port hitting the same
 * wall is what proved it: this file lives in pc_port/include so every console
 * port picks it up off an include path it already has.
 *
 * FUNCTION-LIKE macros on purpose. An object-like `#define read PsxIo_read`
 * would also rewrite `g_FsQueue.read.idx` in fsqueue_3.c, which is a struct
 * member. A function-like macro only expands when the name is followed by `(`,
 * so calls and definitions are rewritten and field accesses are left alone.
 *
 * Included by the definition site (mcard_xbox.c) as well as the call sites, so
 * one header keeps both ends in agreement.
 */
#ifndef PSX_IO_CONSOLE_H
#define PSX_IO_CONSOLE_H

#if defined(SH_XBOX360_PORT) || defined(SH_PS3_PORT)

/* Arguments are deliberately NOT parenthesised. These macros must rewrite the
 * DEFINITIONS in mcard_xbox.c and the PROTOTYPES in psyq/libapi.h as well as the
 * calls, and `PsxIo_open((char* path), (unsigned int flags))` is not a valid
 * parameter list. Leaving them bare is safe here because every argument lands as
 * a complete entry in a comma-separated argument list, where no operator can
 * bind across the comma that already delimits it. */
#define open(path, flags)        PsxIo_open(path, flags)
#define close(handle)            PsxIo_close(handle)
#define read(handle, buf, n)     PsxIo_read(handle, buf, n)
#define write(handle, buf, n)    PsxIo_write(handle, buf, n)
#define lseek(handle, off, w)    PsxIo_lseek(handle, off, w)
#define rename(from, to)         PsxIo_rename(from, to)

#endif /* SH_XBOX360_PORT || SH_PS3_PORT */

#endif /* PSX_IO_CONSOLE_H */
