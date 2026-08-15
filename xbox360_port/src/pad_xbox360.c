/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pad_xbox360.c - PSX controller (libpad) on Xbox 360, backed by the real pad.
 *
 * Same contract as xbox_port/src/pad_xbox.c: the game calls PadInitDirect once
 * and then reads that raw PSX pad buffer every frame (Joy_ReadP1 ->
 * Joy_ControllerDataUpdate, which does heldBtnFlags = ~digitalButtons). PSX
 * buttons are ACTIVE-LOW (0 = pressed). We capture the buffer pointer and
 * refresh it each frame from libXenon's controller state.
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

#include <input/input.h>
#include <usb/usbmain.h>

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
 * drags in decomp types this TU has no other reason to see. */
#define PadStateStable 6

static unsigned char* s_padBuf = 0;
static int            s_haveCtrl = 0;
/* Set once a stick has been observed inside its deadzone -- see Pad_Poll. */
static int            s_lStickArmed = 0;
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
    usb_init();
    usb_do_poll();
    SH_DBG("[PAD] libxenon usb up");
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;                           /* single player: port 0 only */
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
    SH_DBG("[PAD] PadInitDirect buf=%p", pad1);
}

/* Analog deadzone. libXenon reports sticks as s16. The Xbox port had to raise
 * this to ~40% because controller DRIFT sitting near a smaller threshold toggled
 * the d-pad bit every frame, and the menu's edge detection read that as repeated
 * presses -- the cursor flew through the options the moment the menu appeared.
 * The same failure would appear here, so the same threshold is used. */
#define STICK_DZ 13000

void Pad_Poll(void)
{
    struct controller_data_s c;
    unsigned short psx = 0xFFFF;

    if (!s_padBuf)
        return;

    usb_do_poll();
    s_haveCtrl = (get_controller_data(&c, 0) != 0);
    if (!s_haveCtrl) {
        Pad_FillIdle(s_padBuf);
        return;
    }

    #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))
    if (c.up)    PRESS(PSX_UP);
    if (c.down)  PRESS(PSX_DOWN);
    if (c.left)  PRESS(PSX_LEFT);
    if (c.right) PRESS(PSX_RIGHT);
    if (c.start) PRESS(PSX_START);
    if (c.back)  PRESS(PSX_SELECT);
    if (c.s1_z)  PRESS(PSX_L3);
    if (c.s2_z)  PRESS(PSX_R3);
    if (c.a)     PRESS(PSX_CROSS);
    if (c.b)     PRESS(PSX_CIRCLE);
    if (c.x)     PRESS(PSX_SQUARE);
    if (c.y)     PRESS(PSX_TRIANGLE);
    if (c.lb)    PRESS(PSX_L1);
    if (c.rb)    PRESS(PSX_R1);
    if (c.lt > 0x20) PRESS(PSX_L2);
    if (c.rt > 0x20) PRESS(PSX_R2);

    /* A stick is not trusted until seen AT REST at least once. A pad that comes
     * up reporting a deflected stick would otherwise hold a direction from boot,
     * which the menu's hold-repeat turns into an endless scroll. Arming costs
     * nothing normally: a stick at rest arms on its first report. */
    if (!s_lStickArmed &&
        c.s1_x > -STICK_DZ && c.s1_x < STICK_DZ &&
        c.s1_y > -STICK_DZ && c.s1_y < STICK_DZ) {
        s_lStickArmed = 1;
    }
    if (s_lStickArmed) {
        if (c.s1_y >  STICK_DZ) PRESS(PSX_UP);      /* forward */
        if (c.s1_y < -STICK_DZ) PRESS(PSX_DOWN);    /* back    */
        if (c.s1_x < -STICK_DZ) PRESS(PSX_LEFT);    /* turn L  */
        if (c.s1_x >  STICK_DZ) PRESS(PSX_RIGHT);   /* turn R  */
    }
    #undef PRESS

    s_padBuf[0] = 0x00;
    s_padBuf[1] = 0x41;
    s_padBuf[2] = (unsigned char)(psx & 0xFF);
    s_padBuf[3] = (unsigned char)((psx >> 8) & 0xFF);

    /* Right stick -> PSX rightX/rightY (0..255, 128 = centre) for the shared
     * TPS/free-look camera. Deadzone applied HERE, not only in the camera:
     * ControllerData_AnalogToDigital turns ANY off-centre analog byte into
     * digital stick flags with a smaller threshold than the camera's, so raw
     * drift the camera ignores would still pulse the menu cursor. */
    {
        int rx = c.s2_x, ry = c.s2_y;
        if (rx > -STICK_DZ && rx < STICK_DZ) rx = 0;
        if (ry > -STICK_DZ && ry < STICK_DZ) ry = 0;
        s_padBuf[4] = (unsigned char)(128 + (rx >> 8));
        s_padBuf[5] = (unsigned char)(128 - (ry >> 8));   /* stick-up = look-up */
        s_padBuf[6] = 0x80;
        s_padBuf[7] = 0x80;
    }

    if (psx != s_lastButtons) {
        SH_DBG("[PAD] buttons=%04X", (unsigned)psx);
        s_lastButtons = psx;
    }
}

/* The 360 pad has no Black/White buttons, so the Xbox port's bw_quick_save bind
 * has nothing to sit on here. Reported as "not pressed" rather than faked onto
 * another button, which would silently steal an input the game already uses;
 * quick save/load needs its own 360 bind. */
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

/* PSX actuator bytes -> pad rumble. table[0]/[1] are the small/large motors. */
void PadSetAct(int socket, unsigned char* table, int len)
{
    (void)socket;
    if (!table || len < 2)
        return;
    set_controller_rumble(0, table[0], table[1]);
}
