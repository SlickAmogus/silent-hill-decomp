/*
 * libapi_stub.c - PSX kernel API stubs
 *
 * On Windows, EnterCriticalSection/ExitCriticalSection are Windows API functions.
 * The PSX versions have different signatures (no args). We define renamed versions
 * and use macros in the header to redirect game code to these.
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

/* ==========================================================================
 * Memory card filesystem stubs.
 *
 * PsyCross has implementations of these in libapi.c, but the static-lib
 * archive resolution wasn't pulling that .obj in (other symbols in the
 * same .obj weren't referenced first). Defining them here in a .obj
 * that's always linked into the exe sidesteps that. These remain no-op
 * placeholders — the real save/load implementation will replace them
 * with actual MemCardOpen/Read/Write calls.
 *
 * Signatures pulled from include/psyq/libapi.h:54-60. PSX `long` is
 * 32-bit and matches `int` on Win64 ABI. Don't rename or reorder; the
 * PSX header expects these exact names.
 * ==========================================================================*/

struct DIRENTRY* firstfile(char* name, struct DIRENTRY* dir)
{
    (void)name; (void)dir;
    return 0;
}

struct DIRENTRY* nextfile(struct DIRENTRY* dir)
{
    (void)dir;
    return 0;
}

long erase(char* name)
{
    (void)name;
    return 0;
}

long format(char* name)
{
    (void)name;
    return 0;
}
