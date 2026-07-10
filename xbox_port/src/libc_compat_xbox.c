/*
 * libc_compat_xbox.c - libc functions the decomp uses that pdclib (nxdk) lacks.
 * bzero is real. The PSX open/close/read/write/lseek file API (the game only
 * uses it for memcard "buXX:" paths + the dead "sim:" PCdrv path) moved to
 * mcard_xbox.c, which keeps the old behavior for non-memcard paths: fail (-1)
 * so callers fall back to the CD/queue path.
 */
#include <string.h>   /* memset, size_t */

void bzero(void* p, size_t n) { memset(p, 0, n); }
