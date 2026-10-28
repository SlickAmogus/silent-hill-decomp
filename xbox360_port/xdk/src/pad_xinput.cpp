/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pad_xinput.cpp - PSX controller (libpad) on Xbox 360, through XInput.
 *
 * Same contract as pad_xbox360.c (libXenon) and xbox_port/src/pad_xbox.c: the
 * game calls PadInitDirect once and then reads that raw PSX pad buffer every
 * frame. PSX buttons are ACTIVE-LOW (0 = pressed).
 *
 * PSX s_AnalogController layout (include/bodyprog/sys/joy.h):
 *   [0] status        : 0x00 connected, 0xFF disconnected
 *   [1] recv:4|term:4 : 0x41 = 16-button digital pad
 *   [2..3] digitalButtons (u16, active-low)
 *   [4..7] rightX, rightY, leftX, leftY (0x80 = neutral)
 */
#include <xtl.h>
#include <string.h>

extern "C" {
#include "sh_log.h"
}

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

#define PadStateStable 6

static unsigned char* s_padBuf = 0;
static int            s_lStickArmed = 0;
static unsigned short s_lastButtons = 0xFFFF;

extern "C" void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;
    b[1] = 0x41;
    b[2] = 0xFF; b[3] = 0xFF;
    b[4] = 0x80; b[5] = 0x80;
    b[6] = 0x80; b[7] = 0x80;
}

extern "C" void Pad_XboxInit(void)
{
    SH_DBG("[PAD] XInput, user 0");
}

extern "C" void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
    SH_DBG("[PAD] PadInitDirect buf=%p", pad1);
}

/* ~40% deadzone, for the same reason the other console ports raised it:
 * drift near a smaller threshold toggled the d-pad bit every frame and the
 * menu's edge detection read that as repeated presses. */
#define STICK_DZ 13000

extern "C" void Pad_Poll(void)
{
    XINPUT_STATE   st;
    unsigned short psx = 0xFFFF;

    if (!s_padBuf)
        return;

    if (XInputGetState(0, &st) != ERROR_SUCCESS) {
        Pad_FillIdle(s_padBuf);
        return;
    }
    {
        const XINPUT_GAMEPAD& g = st.Gamepad;
        #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP)        PRESS(PSX_UP);
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      PRESS(PSX_DOWN);
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      PRESS(PSX_LEFT);
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     PRESS(PSX_RIGHT);
        if (g.wButtons & XINPUT_GAMEPAD_START)          PRESS(PSX_START);
        if (g.wButtons & XINPUT_GAMEPAD_BACK)           PRESS(PSX_SELECT);
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     PRESS(PSX_L3);
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    PRESS(PSX_R3);
        if (g.wButtons & XINPUT_GAMEPAD_A)              PRESS(PSX_CROSS);
        if (g.wButtons & XINPUT_GAMEPAD_B)              PRESS(PSX_CIRCLE);
        if (g.wButtons & XINPUT_GAMEPAD_X)              PRESS(PSX_SQUARE);
        if (g.wButtons & XINPUT_GAMEPAD_Y)              PRESS(PSX_TRIANGLE);
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  PRESS(PSX_L1);
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) PRESS(PSX_R1);
        if (g.bLeftTrigger  > 0x20) PRESS(PSX_L2);
        if (g.bRightTrigger > 0x20) PRESS(PSX_R2);

        /* A stick is not trusted until seen AT REST once, so a pad that comes
         * up deflected cannot hold a direction from boot. */
        if (!s_lStickArmed &&
            g.sThumbLX > -STICK_DZ && g.sThumbLX < STICK_DZ &&
            g.sThumbLY > -STICK_DZ && g.sThumbLY < STICK_DZ) {
            s_lStickArmed = 1;
        }
        if (s_lStickArmed) {
            if (g.sThumbLY >  STICK_DZ) PRESS(PSX_UP);
            if (g.sThumbLY < -STICK_DZ) PRESS(PSX_DOWN);
            if (g.sThumbLX < -STICK_DZ) PRESS(PSX_LEFT);
            if (g.sThumbLX >  STICK_DZ) PRESS(PSX_RIGHT);
        }
        #undef PRESS

        s_padBuf[0] = 0x00;
        s_padBuf[1] = 0x41;
        /* digitalButtons is a u16 the game reads natively, and this machine is
         * big-endian: the HIGH byte goes first. The little-endian order the
         * Original Xbox used swapped bits 0-7 with 8-15 -- d-pad acted as the
         * face buttons and vice versa (first hardware test, 2026-10-28). */
        s_padBuf[2] = (unsigned char)((psx >> 8) & 0xFF);
        s_padBuf[3] = (unsigned char)(psx & 0xFF);

        /* Right stick -> PSX rightX/rightY for the shared TPS/free-look camera.
         * Deadzone applied here too, not only in the camera. */
        {
            int rx = g.sThumbRX, ry = g.sThumbRY;
            if (rx > -STICK_DZ && rx < STICK_DZ) rx = 0;
            if (ry > -STICK_DZ && ry < STICK_DZ) ry = 0;
            s_padBuf[4] = (unsigned char)(128 + (rx >> 8));
            s_padBuf[5] = (unsigned char)(128 - (ry >> 8));
            s_padBuf[6] = 0x80;
            s_padBuf[7] = 0x80;
        }
    }

    if (psx != s_lastButtons) {
        SH_DBG("[PAD] buttons=%04X", (unsigned)psx);
        s_lastButtons = psx;
    }
}

extern "C" int Pad_XboxBlackWhite(int* black, int* white)
{
    if (black) *black = 0;
    if (white) *white = 0;
    return 0;
}

extern "C" unsigned short Pad_XboxButtons(void)
{
    return s_lastButtons;
}

/* libpad surface the game links against. PadGetState reports Stable
 * unconditionally: the game polls it before accepting input. */
extern "C" int  PadChkVsync(void)                              { return 0; }
extern "C" void PadStartCom(void)                              { }
extern "C" int  PadGetState(int socket)                        { (void)socket; return PadStateStable; }
extern "C" int  PadInfoMode(int socket, int term, int offs)    { (void)socket; (void)term; (void)offs; return 0; }
extern "C" int  PadSetActAlign(int socket, unsigned char* a)   { (void)socket; (void)a; return 1; }
extern "C" int  PadSetMainMode(int socket, int offs, int lock) { (void)socket; (void)offs; (void)lock; return 1; }

/* PSX actuator bytes -> pad rumble. table[0]/[1] are the small/large motors. */
extern "C" void PadSetAct(int socket, unsigned char* table, int len)
{
    XINPUT_VIBRATION v;
    (void)socket;
    if (!table || len < 2)
        return;
    v.wLeftMotorSpeed  = (WORD)(table[1] << 8);
    v.wRightMotorSpeed = (WORD)(table[0] << 8);
    XInputSetState(0, &v);
}
