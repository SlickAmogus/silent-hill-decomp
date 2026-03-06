/*
 * libapi_stub.c - PSX kernel API stubs
 *
 * On Windows, EnterCriticalSection/ExitCriticalSection are Windows API functions.
 * The PSX versions have different signatures (no args). We define renamed versions
 * and use macros in the header to redirect game code to these.
 */

/* PSX EnterCriticalSection - disables interrupts (no-op on PC) */
int PsxEnterCriticalSection(void)
{
    return 0;
}

/* PSX ExitCriticalSection - enables interrupts (no-op on PC) */
void PsxExitCriticalSection(void)
{
}
