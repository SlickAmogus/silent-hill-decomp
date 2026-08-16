/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pad_ps3.c - PSX controller (libpad) on PS3, backed by the real pad.
 *
 * This is the easiest of the three console pads by a wide margin, because the
 * PS3 controller IS a PlayStation controller: every button has a direct PSX
 * counterpart with no remapping to invent, and the analog nubs are already
 * 0x00-0xFF with 0x80 at centre, which is exactly the PSX encoding. The Xbox
 * ports had to translate an XID s16 axis and pick which Xbox face button stood
 * in for Circle; here the mapping is the identity.
 *
 * Same contract as xbox_port/src/pad_xbox.c: the game calls PadInitDirect once
 * and then reads that raw PSX pad buffer every frame (Joy_ReadP1 ->
 * Joy_ControllerDataUpdate, which does heldBtnFlags = ~digitalButtons). PSX
 * buttons are ACTIVE-LOW (0 = pressed); PSL1GHT's padData bitfields are 1 when
 * pressed, so the sense is inverted on the way in.
 *
 * PSX s_AnalogController layout (include/bodyprog/sys/joy.h):
 *   [0] status        : 0x00 connected, 0xFF disconnected
 *   [1] recv:4|term:4 : 0x41 = 16-button digital pad
 *   [2..3] digitalButtons (u16, active-low)
 *   [4..7] rightX, rightY, leftX, leftY (0x80 = neutral)
 *
 * If no controller is present we fill a valid idle state rather than failing, so
 * a missing pad can never hang or crash the boot.
 */
#include <string.h>

#include <io/pad.h>

#include "sh_log.h"

/* PSX button bit positions, from the Xbox port's mapping. */
#define PSX_SELECT   0
#define PSX_L3       1
#define PSX_R3       2
#define PSX_START    3
#define PSX_UP       4
#define PSX_RIGHT    5
#define PSX_DOWN     6
#define PSX_LEFT     7
#define PSX_L2       8
#define PSX_R2       9
#define PSX_L1      10
#define PSX_R1      11
#define PSX_TRIANGLE 12
#define PSX_CIRCLE  13
#define PSX_CROSS   14
#define PSX_SQUARE  15

/* From psyq/libpad.h. Defined locally rather than including that header, which
 * drags in decomp types that cannot coexist with PSL1GHT's (see ps3_hal.h). */
#define PadStateStable 6

static unsigned char* s_padBuf = 0;
static int            s_padUp  = 0;
static int            s_lStickArmed = 0;
static int            s_rStickArmed = 0;
static unsigned short s_lastButtons = 0xFFFF;

static void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;                          /* connected */
    b[1] = 0x41;                          /* digital pad */
    b[2] = 0xFF; b[3] = 0xFF;             /* all buttons released (active-low) */
    b[4] = 0x80; b[5] = 0x80;             /* right stick neutral */
    b[6] = 0x80; b[7] = 0x80;             /* left stick neutral */
}

void Pad_XboxInit(void)
{
    s_padUp = (ioPadInit(7) == 0);
    SH_DBG("[PAD] ioPadInit -> %s", s_padUp ? "ok" : "FAILED");
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;                           /* single player: port 0 only */
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
    SH_DBG("[PAD] PadInitDirect buf=%p", pad1);
}

/* Deadzones in PS3 nub units (centre 128, so full deflection is 127).
 *
 * The Xbox port's values were ~40% and ~21% of an s16 axis, and both exist to
 * fix an observed bug rather than to taste: at a smaller left-stick threshold,
 * controller DRIFT sat near the edge and toggled the d-pad bit every frame,
 * which the menu's edge detection read as repeated presses -- the cursor flew
 * through the options the moment the menu appeared. Scaled to this range. */
#define LSTICK_DZ 51
#define RSTICK_DZ 27

void Pad_Poll(void)
{
    padInfo        info;
    padData        data;
    unsigned short psx = 0xFFFF;

    if (!s_padBuf)
        return;

    if (!s_padUp || ioPadGetInfo(&info) != 0 || info.connected < 1 ||
        ioPadGetData(0, &data) != 0 || data.len == 0) {
        Pad_FillIdle(s_padBuf);
        return;
    }

    #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))
    if (data.BTN_UP)       PRESS(PSX_UP);
    if (data.BTN_DOWN)     PRESS(PSX_DOWN);
    if (data.BTN_LEFT)     PRESS(PSX_LEFT);
    if (data.BTN_RIGHT)    PRESS(PSX_RIGHT);
    if (data.BTN_START)    PRESS(PSX_START);
    if (data.BTN_SELECT)   PRESS(PSX_SELECT);
    if (data.BTN_L3)       PRESS(PSX_L3);
    if (data.BTN_R3)       PRESS(PSX_R3);
    if (data.BTN_L1)       PRESS(PSX_L1);
    if (data.BTN_R1)       PRESS(PSX_R1);
    if (data.BTN_L2)       PRESS(PSX_L2);
    if (data.BTN_R2)       PRESS(PSX_R2);
    if (data.BTN_TRIANGLE) PRESS(PSX_TRIANGLE);
    if (data.BTN_CIRCLE)   PRESS(PSX_CIRCLE);
    if (data.BTN_CROSS)    PRESS(PSX_CROSS);
    if (data.BTN_SQUARE)   PRESS(PSX_SQUARE);

    /* Left stick -> d-pad, ORed with the real d-pad, so the stick walks and
     * turns in the classic tank scheme. This is the ONLY stick->UI path: the pad
     * reports as DIGITAL (0x41), so the game's analog->digital stage ignores the
     * stick bytes and the right-stick camera reads them directly. */
    {
        int lx = (int)data.ANA_L_H - 128;
        int ly = (int)data.ANA_L_V - 128;   /* ANA_L_V: 0 = up, 255 = down */

        /* A stick is not trusted until seen AT REST at least once. A pad that
         * comes up reporting a deflected stick would otherwise hold a direction
         * from boot, which the menu's hold-repeat turns into an endless scroll.
         * Arming costs nothing normally: a stick at rest arms on the first
         * report. */
        if (!s_lStickArmed &&
            lx > -LSTICK_DZ && lx < LSTICK_DZ && ly > -LSTICK_DZ && ly < LSTICK_DZ)
            s_lStickArmed = 1;

        if (s_lStickArmed) {
            if (ly < -LSTICK_DZ) PRESS(PSX_UP);      /* stick up    -> forward */
            if (ly >  LSTICK_DZ) PRESS(PSX_DOWN);    /* stick down  -> back    */
            if (lx < -LSTICK_DZ) PRESS(PSX_LEFT);    /* stick left  -> turn L  */
            if (lx >  LSTICK_DZ) PRESS(PSX_RIGHT);   /* stick right -> turn R  */
        }
    }
    #undef PRESS

    s_padBuf[0] = 0x00;
    s_padBuf[1] = 0x41;
    s_padBuf[2] = (unsigned char)(psx & 0xFF);
    s_padBuf[3] = (unsigned char)((psx >> 8) & 0xFF);

    /* Right stick -> PSX rightX/rightY for the shared TPS/free-look camera.
     * Deadzone applied HERE, not only in the camera: ControllerData_AnalogToDigital
     * turns ANY off-centre analog byte into digital stick flags with a smaller
     * threshold than the camera's, so raw drift the camera ignores would still
     * pulse the menu cursor.
     *
     * No axis inversion, unlike both Xbox ports: XID reports +Y as up and had to
     * be flipped, whereas the PS3 nub already encodes 0 = up exactly as the PSX
     * DualShock does. Values pass through untouched apart from the deadzone. */
    {
        int rx = (int)data.ANA_R_H - 128;
        int ry = (int)data.ANA_R_V - 128;

        if (!s_rStickArmed &&
            rx > -RSTICK_DZ && rx < RSTICK_DZ && ry > -RSTICK_DZ && ry < RSTICK_DZ)
            s_rStickArmed = 1;
        if (!s_rStickArmed) { rx = 0; ry = 0; }

        /* Subtract rather than zero outside the zone so motion past the
         * threshold ramps from nothing instead of jumping. */
        if      (rx >  RSTICK_DZ) rx -= RSTICK_DZ;
        else if (rx < -RSTICK_DZ) rx += RSTICK_DZ;
        else                      rx  = 0;
        if      (ry >  RSTICK_DZ) ry -= RSTICK_DZ;
        else if (ry < -RSTICK_DZ) ry += RSTICK_DZ;
        else                      ry  = 0;

        s_padBuf[4] = (unsigned char)(128 + rx);
        s_padBuf[5] = (unsigned char)(128 + ry);
        s_padBuf[6] = 0x80;
        s_padBuf[7] = 0x80;
    }

    if (psx != s_lastButtons) {
        SH_DBG("[PAD] buttons=%04X", (unsigned)psx);
        s_lastButtons = psx;
    }
}

/* The PS3 pad has no Black/White buttons, so the Xbox port's bw_quick_save bind
 * has nothing to sit on. Reported as "not pressed" rather than faked onto
 * another button, which would silently steal an input the game already uses;
 * quick save/load needs its own PS3 bind. */
int Pad_XboxBlackWhite(int* black, int* white)
{
    if (black) *black = 0;
    if (white) *white = 0;
    return 0;
}

unsigned short Pad_XboxButtons(void)
{
    return s_lastButtons;
}

/* ---- libpad surface the game links against -------------------------------
 * The port drives the pad through PadInitDirect + Pad_Poll, so these exist to
 * satisfy the PSX API rather than to do work. PadGetState reports Stable
 * unconditionally: the game polls it before accepting input, and reporting a
 * transient state would make it discard perfectly good pad reports. */
int  PadChkVsync(void)                                 { return 0; }
void PadStartCom(void)                                 { }
int  PadGetState(int socket)                           { (void)socket; return PadStateStable; }
int  PadInfoMode(int socket, int term, int offs)       { (void)socket; (void)term; (void)offs; return 0; }
int  PadSetActAlign(int socket, unsigned char* a)      { (void)socket; (void)a; return 1; }
int  PadSetMainMode(int socket, int offs, int lock)    { (void)socket; (void)offs; (void)lock; return 1; }

/* PSX actuator bytes -> pad rumble. table[0]/[1] are the small/large motors,
 * which is exactly what padActParam wants: the small motor is on/off and the
 * large one is a 0-255 magnitude, same as the PSX convention. */
void PadSetAct(int socket, unsigned char* table, int len)
{
    padActParam act;

    (void)socket;
    if (!table || len < 2)
        return;
    memset(&act, 0, sizeof(act));
    act.small_motor = table[0] ? 1 : 0;
    act.large_motor = table[1];
    ioPadSetActDirect(0, &act);
}
