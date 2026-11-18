/* SPDX-License-Identifier: GPL-3.0-or-later */
/* <unistd.h> for the UWP build; MinGW supplies it on desktop Windows. */
#ifndef SH_UWP_UNISTD_H
#define SH_UWP_UNISTD_H

#include <direct.h>
#include <io.h>
#include <process.h>

/* The app-family CRT declares only the underscored spellings of these. */
#define getcwd _getcwd
#define chdir  _chdir

#ifndef F_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#endif

#endif
