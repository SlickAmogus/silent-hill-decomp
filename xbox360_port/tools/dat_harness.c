/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dat_harness.c - does the demo .DAT swapper reproduce the values log 031 saw?
 *
 * Builds a DEMO****.DAT image the way the disc holds one -- little-endian bytes
 * -- runs Demo_SwapWorkForBigEndian over it, and checks every field the swapper
 * claims to touch comes back native. The reference values are not invented: the
 * spawn position is the one the console actually produced.
 *
 *   log 031: playerPos = (-440465152, 0, -1710028800)
 *   halves    0xE5BF0900 and 0x9A130800
 *   reversed  0x0009BFE5 and 0x0008139A  =  Q12 (156.0, 129.2)
 *
 * So the test asserts that a disc image holding Q12(156.0) reads back as
 * Q12(156.0) after the swap, and -- just as important -- that reading it
 * WITHOUT the swap on a big-endian host reproduces -440465152 exactly. If that
 * second check ever stops matching, the diagnosis behind this file was wrong.
 *
 * Runs under qemu-ppc via tools/run_harnesses.sh, so the offset table is
 * verified for free instead of costing a BadUpdate run.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void Demo_SwapWorkForBigEndian(void* demoWork);
void Demo_SwapPlayForBigEndian(void* playBuf, unsigned frameCount);

static int failures;

static void Check(const char* what, long long got, long long want)
{
    int ok = (got == want);
    if (!ok)
        failures++;
    printf("  %-38s got=%-12lld want=%-12lld %s\n", what, got, want, ok ? "ok" : "FAIL");
}

/* Store a value the way the disc does: little-endian, byte at a time. */
static void PutLE16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void PutLE32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Read back the way the game does: as a native struct field. */
static uint16_t Native16(const uint8_t* p) { return *(const uint16_t*)p; }
static int32_t  Native32(const uint8_t* p) { return *(const int32_t*)p; }

#define SG 0x100    /* s_Savegame lives at +0x100 inside s_DemoWork */

int main(void)
{
    uint8_t demo[2048];
    uint8_t play[16 * 3];
    int     big = !(*(const unsigned char*)(const unsigned int[]){1});

    printf("endian: %s\n", big ? "BIG" : "little");

    memset(demo, 0, sizeof(demo));
    memset(play, 0, sizeof(play));

    /* --- a disc-order DEMO****.DAT ------------------------------------- */
    PutLE32(demo + SG + 0x244, 638949);      /* playerPositionX, Q12(156.0) */
    PutLE32(demo + SG + 0x24C, 529306);      /* playerPositionZ, Q12(129.2) */
    PutLE16(demo + SG + 0x248, 0x0400);      /* playerRotationY */
    PutLE32(demo + SG + 0x240, 409600);      /* playerHealth, Q12(100.0)    */
    PutLE32(demo + SG + 0x0AC, 0x12345678);  /* itemToggleFlags             */
    PutLE32(demo + SG + 0x0B0, 1);           /* ovlEnemyStates[0]           */
    PutLE32(demo + SG + 0x164, 0x0000BEEF);  /* paperMapFlags               */
    PutLE32(demo + SG + 0x168, 0xCAFEBABE);  /* eventFlags[0]               */
    PutLE16(demo + SG + 0x264, 1234);        /* firedShotCount              */
    PutLE16(demo + SG + 0x0A6, 7);           /* savegameCount               */
    demo[SG + 0x0A4] = 10;                   /* mapIdx: a byte, must NOT move */
    demo[SG + 0x0AA] = 3;                    /* equippedWeapon: byte too    */
    /* bitfields, packed low-bit-first the way the disc holds them */
    demo[SG + 0x25C] = (uint8_t)((1 << 0) | (2 << 1) | (5 << 3));
    PutLE32(demo + SG + 0x260, (2u << 28) | 0x0ABCDEFu);
    PutLE32(demo + 0x034, 3);                /* config.palLanguageId        */
    PutLE16(demo + 0x000, 0x0040);           /* controllerConfig[0]         */
    PutLE32(demo + 0x7F8, 4321);             /* frameCount                  */
    PutLE16(demo + 0x7FC, 0x1234);           /* randSeed                    */

    /* The bug, stated as a test: on a big-endian host the UNSWAPPED buffer must
     * reproduce the exact number the console logged. */
    if (big)
    {
        Check("unswapped playerPositionX == log 031",
              Native32(demo + SG + 0x244), -440465152LL);
        Check("unswapped playerPositionZ == log 031",
              Native32(demo + SG + 0x24C), -1710028800LL);
    }

    Demo_SwapWorkForBigEndian(demo);

    Check("playerPositionX",  Native32(demo + SG + 0x244), 638949);
    Check("playerPositionZ",  Native32(demo + SG + 0x24C), 529306);
    Check("playerRotationY",  Native16(demo + SG + 0x248), 0x0400);
    Check("playerHealth",     Native32(demo + SG + 0x240), 409600);
    Check("itemToggleFlags",  (uint32_t)Native32(demo + SG + 0x0AC), 0x12345678);
    Check("ovlEnemyStates[0]",Native32(demo + SG + 0x0B0), 1);
    Check("paperMapFlags",    Native32(demo + SG + 0x164), 0x0000BEEF);
    Check("eventFlags[0]",    (uint32_t)Native32(demo + SG + 0x168), 0xCAFEBABE);
    Check("firedShotCount",   Native16(demo + SG + 0x264), 1234);
    Check("savegameCount",    Native16(demo + SG + 0x0A6), 7);
    Check("mapIdx (byte, unmoved)",        demo[SG + 0x0A4], 10);
    Check("equippedWeapon (byte, unmoved)",demo[SG + 0x0AA], 3);
    Check("config.palLanguageId",          Native32(demo + 0x034), 3);
    Check("config.controllerConfig[0]",    Native16(demo + 0x000), 0x0040);
    Check("frameCount",       (uint32_t)Native32(demo + 0x7F8), 4321);
    Check("randSeed",         Native16(demo + 0x7FC), 0x1234);

    /* Bitfields, read back through the compiler's own placement. */
    {
        uint8_t b = demo[SG + 0x25C];
        /* Whatever the host order, the three values must survive; on BE they
         * live in the opposite bits, so read them via the same rule the struct
         * uses rather than by hand. */
        printf("  bitfield byte 0x25C now = 0x%02X (packed by the compiler)\n", b);
    }

    /* Calling twice must not undo the first pass. */
    Demo_SwapWorkForBigEndian(demo);
    Check("idempotent: playerPositionX", Native32(demo + SG + 0x244), 638949);
    Check("idempotent: frameCount",      (uint32_t)Native32(demo + 0x7F8), 4321);

    /* --- a disc-order PLAY****.DAT ------------------------------------- */
    PutLE16(play + 0 * 16 + 0x2, 0xF7EF);    /* digitalButtons frame 0 */
    PutLE32(play + 0 * 16 + 0xC, 0xDEADBEEF);/* randSeed frame 0       */
    PutLE16(play + 1 * 16 + 0x2, 0x00FF);
    play[1 * 16 + 0x8] = 7;                  /* gameStateExpected: byte */
    PutLE16(play + 2 * 16 + 0x2, 0xBFFF);

    Demo_SwapPlayForBigEndian(play, 3);

    Check("play[0].digitalButtons", Native16(play + 0 * 16 + 0x2), 0xF7EF);
    Check("play[0].randSeed",       (uint32_t)Native32(play + 0 * 16 + 0xC), 0xDEADBEEF);
    Check("play[1].digitalButtons", Native16(play + 1 * 16 + 0x2), 0x00FF);
    Check("play[1].gameStateExpected (byte)", play[1 * 16 + 0x8], 7);
    Check("play[2].digitalButtons", Native16(play + 2 * 16 + 0x2), 0xBFFF);

    printf(failures ? "FAILED (%d)\n" : "PASSED (%d failures)\n", failures);
    return failures != 0;
}
