/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dat_endian.c - byte-swap the attract-demo .DAT files after they are read.
 *
 * MISC/DEMO****.DAT is overlaid straight onto s_DemoWork and MISC/PLAY****.DAT
 * onto an array of s_DemoFrameData. Neither has a reformat walker, so on a
 * big-endian host every multi-byte field reads reversed -- the same class as
 * the TIM and ANM bugs (tim_endian.c, anm_endian.c) and the same remedy:
 * convert once at the load chokepoint so everything downstream sees native
 * values.
 *
 * WHAT THIS ACTUALLY BROKE, from Xbox 360 log 031. The demo's savegame carries
 * the player's spawn position, and it arrived as
 *   playerPos = (-440465152, 0, -1710028800)
 * whose halves are 0xE5BF0900 and 0x9A130800. Byte-reversed those are 0x0009BFE5
 * and 0x0008139A -- Q12 world coordinates of about (156.0, 129.1), an entirely
 * ordinary spawn point. So Harry was placed roughly 400 million units away, the
 * chunk streamer computed mapCell=(-2689,-10438), no loaded chunk ever matched
 * the player's cell, [IPD-DRAW] reported VOID every frame and 3140 [COLL-MISS]
 * followed. Nothing was wrong with the renderer: it was being asked to draw a
 * point in empty space.
 *
 * The per-frame file matters just as much and is easier to overlook.
 * s_AnalogController.digitalButtons is a u16, so with the bytes reversed the
 * demo replays a completely different set of button presses -- the game runs,
 * animates and goes nowhere, which is exactly what was on screen.
 *
 * TWO BITFIELDS NEED MORE THAN A SWAP, and they are the first live instances of
 * that hazard in this port. Bit allocation within a storage unit also flips on
 * big-endian: the first-declared member takes the HIGH bits instead of the low
 * ones. Reversing the bytes of the storage unit does not fix that, so those two
 * are rebuilt through the C struct fields, letting the compiler place them:
 *   - s_Savegame +0x25C  u8  : isNextFearMode:1, add290Hours:2,
 *                              pickedUpSpecialItemCount:5
 *   - s_Savegame +0x260  u32 : field_260:28, gameDifficulty:4
 * s_AnalogController's received_bytes:4 / terminal_type:4 share a byte and are
 * handled the same way.
 */
#include <stdint.h>

#include "game.h"
#include "dat_endian.h"

#include "bodyprog/demo.h"
#include "bodyprog/savegame.h"

#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

/* Read the disc's little-endian value out of memory, byte-wise so the read
 * itself carries no host byte order. */
static uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Fix16(uint8_t* p) { *(uint16_t*)p = Rd16(p); }
static void Fix32(uint8_t* p) { *(uint32_t*)p = Rd32(p); }

static void Fix16n(uint8_t* p, int n) { int i; for (i = 0; i < n; i++) Fix16(p + i * 2); }
static void Fix32n(uint8_t* p, int n) { int i; for (i = 0; i < n; i++) Fix32(p + i * 4); }

/* Offsets are the ones documented in savegame.h / demo.h, used directly rather
 * than via offsetof so this file reads against those tables line by line. */
#define SG_SAVEGAMECOUNT   0x0A6
#define SG_ITEMTOGGLEFLAGS 0x0AC
#define SG_OVLENEMYSTATES  0x0B0   /* s32[45] */
#define SG_PAPERMAPFLAGS   0x164
#define SG_EVENTFLAGS      0x168   /* u32[52] */
#define SG_HEALTHSAT       0x238
#define SG_PICKEDUPITEMS   0x23C
#define SG_PLAYERHEALTH    0x240
#define SG_PLAYERPOSX      0x244
#define SG_PLAYERROTY      0x248
#define SG_PLAYERPOSZ      0x24C
#define SG_GAMEPLAYTIMER   0x250
#define SG_RUNDISTANCE     0x254
#define SG_WALKDISTANCE    0x258
#define SG_BITS_25C        0x25C
#define SG_BITS_260        0x260
#define SG_SHOTCOUNTS      0x264   /* u16[11] through field_278 */

static void SwapSavegame(uint8_t* s)
{
    /* items[40] are four u8 each, and every field from 0xA0..0xA5 and
     * 0xA8..0xAB is a single byte, so none of them move. */
    Fix16(s + SG_SAVEGAMECOUNT);
    Fix32(s + SG_ITEMTOGGLEFLAGS);
    Fix32n(s + SG_OVLENEMYSTATES, 45);
    Fix32(s + SG_PAPERMAPFLAGS);
    Fix32n(s + SG_EVENTFLAGS, 52);
    Fix32(s + SG_HEALTHSAT);
    Fix16(s + SG_PICKEDUPITEMS);
    Fix32(s + SG_PLAYERHEALTH);
    Fix32(s + SG_PLAYERPOSX);
    Fix16(s + SG_PLAYERROTY);
    Fix32(s + SG_PLAYERPOSZ);
    Fix32(s + SG_GAMEPLAYTIMER);
    Fix32(s + SG_RUNDISTANCE);
    Fix32(s + SG_WALKDISTANCE);
    Fix16n(s + SG_SHOTCOUNTS, 11);

    /* Bitfields: rebuild through the struct so the compiler decides placement.
     * The disc byte packs them low-bit-first; big-endian gcc allocates the
     * first-declared member from the top of the unit. */
    {
        s_Savegame* sg  = (s_Savegame*)s;
        uint8_t     raw = s[SG_BITS_25C];
        sg->isNextFearMode           = (raw >> 0) & 0x01;
        sg->add290Hours              = (raw >> 1) & 0x03;
        sg->pickedUpSpecialItemCount = (raw >> 3) & 0x1F;
    }
    {
        s_Savegame* sg  = (s_Savegame*)s;
        uint32_t    raw = Rd32(s + SG_BITS_260);
        sg->field_260      = raw & 0x0FFFFFFFu;
        sg->gameDifficulty = (int32_t)(raw >> 28) & 0x0F;
    }
}

static void SwapOptionsConfig(uint8_t* c)
{
    Fix16n(c + 0x00, 14);   /* s_ControllerConfig: 14 u16 button masks */
    /* 0x1C..0x2D are all single bytes. */
    Fix16(c + 0x2E);        /* seenGameOverTips[1] */
    /* 0x30..0x33 is s8[4]. */
    Fix32(c + 0x34);        /* palLanguageId */
}

void Demo_SwapWorkForBigEndian(void* demoWork)
{
    uint8_t* d = (uint8_t*)demoWork;

    if (!d)
        return;

    /* Idempotency by validation rather than by a flag, because the demo state
     * machine re-reads the file on a failed setup and a second swap would undo
     * the first. Retail frameCounts are a few thousand; the same value read the
     * wrong way round is astronomically larger, so its magnitude is a reliable
     * discriminator and cannot be confused for a plausible native value. */
    if (Rd32(d + 0x7F8) > 0x00FFFFFFu)
        return;   /* reads sane as little-endian => not yet swapped: fall through */

    SwapOptionsConfig(d + 0x000);
    SwapSavegame(d + 0x100);
    Fix32(d + 0x7F8);       /* frameCount */
    Fix16(d + 0x7FC);       /* randSeed */
}

void Demo_SwapPlayForBigEndian(void* playBuf, unsigned frameCount)
{
    uint8_t* p = (uint8_t*)playBuf;
    unsigned i;

    if (!p || frameCount == 0 || frameCount > 0x00FFFFFFu)
        return;

    for (i = 0; i < frameCount; i++)
    {
        uint8_t* f = p + i * 16;

        /* s_AnalogController: status is a byte; the nibble pair shares one byte
         * and flips allocation; digitalButtons is the u16 that decides what the
         * demo actually presses; the four stick axes are bytes. */
        {
            s_AnalogController* ac  = (s_AnalogController*)f;
            uint8_t             raw = f[1];
            ac->received_bytes = (raw >> 0) & 0x0F;
            ac->terminal_type  = (raw >> 4) & 0x0F;
        }
        Fix16(f + 0x2);

        /* 0x8..0xB are single bytes (gameStateExpected, videoPresentInterval,
         * unk_A[2]). */
        Fix32(f + 0xC);     /* randSeed */
    }
}

#else

void Demo_SwapWorkForBigEndian(void* demoWork) { (void)demoWork; }
void Demo_SwapPlayForBigEndian(void* playBuf, unsigned frameCount)
{
    (void)playBuf; (void)frameCount;
}

#endif
