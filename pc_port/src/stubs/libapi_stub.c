/*
 * libapi_stub.c - PSX kernel API stubs (PC-specific shims only)
 *
 * On Windows, EnterCriticalSection/ExitCriticalSection are Windows API functions.
 * The PSX versions have different signatures (no args). We define renamed versions
 * and use macros in the header to redirect game code to these.
 *
 * The memcard filesystem (firstfile/nextfile/erase/format/open/read/write/etc)
 * is now implemented for real in PsyCross/src/psx/libapi.c (operating on
 * 0.MCD / 1.MCD files). Those shims used to live here — no longer needed.
 */

#include <psyq/libapi.h>

/* PSX EnterCriticalSection - disables interrupts (no-op on PC) */
int PsxEnterCriticalSection(void)
{
    return 0;
}

/* PSX ExitCriticalSection - enables interrupts (no-op on PC) */
void PsxExitCriticalSection(void)
{
}
