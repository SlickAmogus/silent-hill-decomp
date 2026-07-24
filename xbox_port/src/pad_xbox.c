/*
 * pad_xbox.c - PSX controller (libpad) on Xbox, backed by the real USB pad.
 *
 * The game calls PadInitDirect(&g_GameWork.rawController, ...) once, then reads
 * that raw PSX pad buffer every frame (Joy_ReadP1 -> Joy_ControllerDataUpdate,
 * which does heldBtnFlags = ~digitalButtons). PSX buttons are ACTIVE-LOW
 * (0 = pressed). We capture that buffer pointer and refresh it each frame
 * (Pad_Poll, called from VSync) from the first USB XID gamepad via nxdk's USB
 * host stack; if no pad is connected we fill a valid idle state.
 *
 * Robust by construction: if USB init / enumeration / the report read fails or
 * no controller is present, Pad_Poll falls back to idle — no hang, no regression.
 *
 * PSX s_AnalogController layout (include/bodyprog/sys/joy.h):
 *   [0] status        : 0x00 connected, 0xFF disconnected
 *   [1] recv:4|term:4 : 0x41 = 16-button digital pad
 *   [2..3] digitalButtons (u16, active-low)
 *   [4..7] rightX, rightY, leftX, leftY (0x80 = neutral)
 */
#include <string.h>
#include <usbh_lib.h>
#include <xid_driver.h>
#include <hal/xbox.h>   /* XLaunchXBE / XReboot for the soft reset */
#include "sh_log.h"

static unsigned char* s_padBuf  = 0;
static xid_dev_t*     s_xid      = 0;
static xid_gamepad_in s_report;
static int            s_haveReport = 0;

/* Rumble target set by PadSetAct (game loop), sent by Pad_Poll (USB-safe). */
static unsigned s_wantRumbleL = 0, s_wantRumbleH = 0;
static int      s_rumbleValid = 0;

static void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;                          /* connected */
    b[1] = 0x41;                          /* digital pad */
    b[2] = 0xFF; b[3] = 0xFF;             /* all buttons released (active-low) */
    b[4] = 0x80; b[5] = 0x80;             /* right stick neutral */
    b[6] = 0x80; b[7] = 0x80;             /* left stick neutral */
}

/* USB interrupt-read completion: copy the gamepad report and re-arm. nxdk's USB
 * is polling-based, so this fires synchronously from usbh_pooling_hubs(). */
static void Pad_ReadCb(UTR_T* utr)
{
    if (utr->status < 0)
        return;
    {
        unsigned len = utr->xfer_len;
        if (len > sizeof(s_report))
            len = sizeof(s_report);
        memcpy(&s_report, utr->buff, len);
        s_haveReport = 1;
    }
    utr->xfer_len        = 0;
    utr->bIsTransferDone = 0;
    usbh_int_xfer(utr);
}

static void Pad_ConnCb(xid_dev_t* dev, int param)
{
    (void)param;
    if (s_xid == 0 && dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER) {
        s_xid = dev;
        usbh_xid_read(dev, 0, Pad_ReadCb);
    }
}

static void Pad_DisconnCb(xid_dev_t* dev, int param)
{
    (void)param;
    if (dev == s_xid) {
        s_xid = 0;
        s_haveReport = 0;
    }
}

/* Called once at startup from main_xbox.c (after the HAL is up). */
void Pad_XboxInit(void)
{
    int i;
    usbh_core_init();
    usbh_xid_init();
    usbh_install_xid_conn_callback(Pad_ConnCb, Pad_DisconnCb);
    /* Give a connected controller a head start to enumerate; per-frame pumping in
     * Pad_Poll completes it within the first second regardless. */
    for (i = 0; i < 200; i++)
        usbh_pooling_hubs();
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
}

void PadStartCom(void) { }

/* Xbox XID gamepad -> PSX digital pad. PSX digitalButtons is active-low, so start
 * "all released" (0xFFFF) and clear a bit per pressed button. */
/* Soft reset: L trigger + R trigger + Start + White button, held ~0.4s (the
 * four-way combo is awkward enough that no debounce is needed beyond the hold).
 * Relaunches our own XBE for an instant reload after a crash/hang — much faster
 * than a dashboard round-trip. XReboot is the fallback if relaunch fails. */
static void Pad_CheckSoftReset(void)
{
    static int held = 0;
    if (s_xid && s_haveReport &&
        s_report.l     > 0x40 &&
        s_report.r     > 0x40 &&
        (s_report.dButtons & (1u << 4)) &&      /* Start */
        s_report.white > 0x40) {
        if (++held >= 20) {
            extern void SH_DebugLogFlush(void);
            SH_DBG("[SH-XBOX] soft reset (L+R+Start+White) -> relaunch");
            SH_DebugLogFlush();
            XLaunchXBE("D:\\default.xbe");       /* reload the game directly */
            XReboot();                            /* fallback */
        }
    } else {
        held = 0;
    }
}

void Pad_Poll(void)
{
    usbh_pooling_hubs();   /* pump the USB host stack (fires Pad_ReadCb) */

    /* Send any pending rumble here (the USB-polling context, right after the
     * hub pump), never from PadSetAct's game-loop context. Change-detected —
     * the motor state holds, so an idle {0,0} stops it and we only hit the bus
     * on a change. */
    if (s_rumbleValid && s_xid) {
        static unsigned s_lastL = 0xFFFFFFFFu, s_lastH = 0xFFFFFFFFu;
        if (s_wantRumbleL != s_lastL || s_wantRumbleH != s_lastH) {
            static int s_firstBuzz = 0;
            if ((s_wantRumbleL || s_wantRumbleH) && !s_firstBuzz) {
                s_firstBuzz = 1;
                SH_DBG("[VIB] rumble l=%u h=%u (engine -> motors live)", s_wantRumbleL, s_wantRumbleH);
            }
            s_lastL = s_wantRumbleL;
            s_lastH = s_wantRumbleH;
            usbh_xid_rumble(s_xid, (unsigned short)s_wantRumbleL, (unsigned short)s_wantRumbleH);
        }
    }

    if (!s_padBuf)
        return;

    Pad_CheckSoftReset();

    if (s_xid && s_haveReport) {
        unsigned short xb  = s_report.dButtons;
        unsigned short psx = 0xFFFF;
        #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))
        if (xb & (1u << 0)) PRESS(4);   /* DPad Up    -> Up    */
        if (xb & (1u << 1)) PRESS(6);   /* DPad Down  -> Down  */
        if (xb & (1u << 2)) PRESS(7);   /* DPad Left  -> Left  */
        if (xb & (1u << 3)) PRESS(5);   /* DPad Right -> Right */
        if (xb & (1u << 4)) PRESS(3);   /* Start      -> Start */
        if (xb & (1u << 5)) PRESS(0);   /* Back       -> Select*/
        if (xb & (1u << 6)) PRESS(1);   /* L3         -> L3    */
        if (xb & (1u << 7)) PRESS(2);   /* R3         -> R3    */
        if (s_report.a     > 0x20) PRESS(14);  /* A     -> Cross    */
        if (s_report.b     > 0x20) PRESS(13);  /* B     -> Circle   */
        if (s_report.x     > 0x20) PRESS(15);  /* X     -> Square   */
        if (s_report.y     > 0x20) PRESS(12);  /* Y     -> Triangle */
        if (s_report.white > 0x20) PRESS(10);  /* White -> L1       */
        if (s_report.black > 0x20) PRESS(11);  /* Black -> R1       */
        if (s_report.l     > 0x20) PRESS(8);   /* LTrig -> L2       */
        if (s_report.r     > 0x20) PRESS(9);   /* RTrig -> R2       */
        /* Left analog stick -> d-pad directions: walk/turn with the stick in
         * the classic tank scheme (up = forward, left/right = turn), ORed with
         * the real d-pad. XID sticks are s16, Y+ = up / X+ = right; ~25%
         * deadzone. (Right stick stays unused — camera is fixed on Xbox.) */
        #define STICK_DZ 8000
        if (s_report.leftStickY >  STICK_DZ) PRESS(4);   /* up    -> forward */
        if (s_report.leftStickY < -STICK_DZ) PRESS(6);   /* down  -> back    */
        if (s_report.leftStickX < -STICK_DZ) PRESS(7);   /* left  -> turn L  */
        if (s_report.leftStickX >  STICK_DZ) PRESS(5);   /* right -> turn R  */
        #undef STICK_DZ
        #undef PRESS
        s_padBuf[0] = 0x00;
        s_padBuf[1] = 0x41;
        s_padBuf[2] = (unsigned char)(psx & 0xFF);
        s_padBuf[3] = (unsigned char)((psx >> 8) & 0xFF);
        /* Right analog stick -> PSX rightX/rightY (0..255, 128=center) so the
         * shared TPS/free-look camera (Pc_TpsCamera_Apply, game_main.c) can orbit.
         * XID axes are s16 (+X=right, +Y=up); >>8 maps to +-128, Y inverted so
         * stick-up = look-up. The camera's own 24-unit deadzone handles center.
         * Only steers the camera in a non-Classic control style (R3 / Xbox Options
         * enable it); in Classic these bytes are read but the camera ignores them. */
        {
            /* Deadzone at the PAD level (not just the camera): the game's
             * ControllerData_AnalogToDigital (joy.c) turns ANY off-center analog
             * byte into digital stick flags with a threshold smaller than the
             * camera's own deadzone, so raw controller DRIFT that the camera
             * ignores still pulsed the menu cursor (cursor flew through the main
             * menu on a slightly-drifting stick). Snap drift to dead-center (128)
             * here so no stick flag is generated; subtract the deadzone so motion
             * past it ramps smoothly. RSTICK_DZ matches the camera's ~24-byte
             * deadzone (24<<8) so the camera loses no usable range. */
            #define RSTICK_DZ 7000
            int rawx = (int)s_report.rightStickX;
            int rawy = (int)s_report.rightStickY;
            int rxb, ryb;
            if      (rawx >  RSTICK_DZ) rawx -= RSTICK_DZ;
            else if (rawx < -RSTICK_DZ) rawx += RSTICK_DZ;
            else                        rawx  = 0;
            if      (rawy >  RSTICK_DZ) rawy -= RSTICK_DZ;
            else if (rawy < -RSTICK_DZ) rawy += RSTICK_DZ;
            else                        rawy  = 0;
            rxb = 128 + (rawx >> 8);
            ryb = 128 - (rawy >> 8);
            if (rxb < 0) rxb = 0; if (rxb > 255) rxb = 255;
            if (ryb < 0) ryb = 0; if (ryb > 255) ryb = 255;
            s_padBuf[4] = (unsigned char)rxb;   /* rightX */
            s_padBuf[5] = (unsigned char)ryb;   /* rightY */
            #undef RSTICK_DZ
        }
        s_padBuf[6] = 0x80; s_padBuf[7] = 0x80;
    } else {
        Pad_FillIdle(s_padBuf);
    }
}

/* Raw PSX digitalButtons (active-low, bit layout above) for pollers that run
 * outside the game loop (FMV skip check). Reflects the last Pad_Poll. */
unsigned short Pad_XboxButtons(void)
{
    if (!s_padBuf)
        return 0xFFFF;
    return (unsigned short)(s_padBuf[2] | ((unsigned short)s_padBuf[3] << 8));
}

int PadGetState(int port)  { (void)port; return 6; }  /* 6 = stable */

/* Vibration. The game's DualShock detection + effect engine
 * (bodyprog_80089090.c, lib_8009E198.c) drive these; they were dead stubs, so
 * rumble never fired. Mirror PsyCross's proven recipe (PsyCross/src/psx/
 * libpad.c): PadChkVsync=1 so the engine pumps the actuator buffer every
 * frame, PadInfoMode reports an analog pad (7), PadSetActAlign succeeds, and
 * PadSetAct maps the PSX actuator bytes to the OG Xbox pad's rumble motors.
 * Vibration defaults ENABLED (settings_reset.c) and toggles in the Vibration
 * option. */
int PadInfoMode(int socket, int term, int offs)    { (void)socket; (void)term; (void)offs; return 7; }
int PadSetMainMode(int socket, int offs, int lock) { (void)socket; (void)offs; (void)lock; return 0; }
int PadSetActAlign(int socket, unsigned char* tbl) { (void)socket; (void)tbl; return 1; }
int PadChkVsync(void)      { return 1; }

void PadSetAct(int socket, unsigned char* table, int len)
{
    /* PSX actuator bytes: table[0] = small (high-freq) motor 0/1, table[1] =
     * large (low-freq) motor 0-255. usbh_xid_rumble(l,h): l = low/large motor,
     * h = high/small motor, 0..65535. Mirror PsyCross's *255 scale + minimal-
     * shake clamp. The motor state HOLDS until set again, so an idle {0,0}
     * (which the engine sends every frame with no active effect) stops it;
     * only touch the USB bus on a change. */
    unsigned h = (table && len > 0) ? (unsigned)table[0] * 255u : 0u;
    unsigned l = (table && len > 1) ? (unsigned)table[1] * 255u : 0u;
    (void)socket;
    if (l != 0 && l < 4096) l = 4096;
    if (h != 0 && h < 4096) h = 4096;
    /* Do NOT touch USB here. PadSetAct runs from the game loop (vibration engine
     * pump), which is NOT the USB-polling context — usbh_xid_write from here can
     * submit a transfer that only completes when usbh_pooling_hubs runs (in
     * Pad_Poll), a cross-context stall. Just record the target; Pad_Poll sends it
     * from the safe context. Change-detected there (motor state holds). */
    s_wantRumbleL = l;
    s_wantRumbleH = h;
    s_rumbleValid = 1;
}
