/*
 * pad_xbox.c - PSX controller (libpad) on Xbox, backed by the real USB pads.
 *
 * The game calls PadInitDirect(&g_GameWork.rawController, g_Controller1) once,
 * then reads that raw PSX pad buffer every frame (Joy_ReadP1 ->
 * Joy_ControllerDataUpdate, which does heldBtnFlags = ~digitalButtons). PSX
 * buttons are ACTIVE-LOW (0 = pressed). We capture both buffer pointers and
 * refresh them each frame (Pad_Poll, called from VSync) from the USB XID
 * gamepads via nxdk's USB host stack; a player with no pad gets a valid idle
 * state.
 *
 * Player 1 is the first gamepad to finish enumerating, exactly as before split
 * screen existed; a pad that comes back on the port a player last used gets that
 * player again. Player 2 only exists while a split-screen game has bound one
 * (Pad_XboxBindP2); until then the second PSX buffer is never written, so a
 * single-player game sees the same zeroed controllers[1] it always has.
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
#include <usb/libusbohci/inc/hub.h>
#include <xboxkrnl/xboxkrnl.h>
#include <hal/xbox.h>   /* XLaunchXBE / XReboot for the soft reset */
#include "sh_log.h"
#include "pc_config.h"   /* bw_quick_save: black/white become quick save/load */
#include "xbox_extra_actions.h"

#define PAD_DEV_MAX CONFIG_XID_MAX_DEV

/* One attached game controller. The read completion runs in the OHCI interrupt
 * handler, so everything it writes is volatile and it never logs: SH_DBG takes
 * the C library's lock, which must not be waited on at interrupt level. */
typedef struct {
    xid_dev_t*        dev;        /* NULL = free slot */
    uint32_t          uid;        /* guards against the nxdk slot being reused */
    int               port;       /* front port 1-4, 0 = unknown */
    xid_gamepad_in    report;
    volatile uint32_t seq;        /* bumped after every accepted report */
    volatile int      haveReport;
    volatile int      readArmed;
    volatile int      badReport;
    volatile unsigned badStart, badLen, badXfer;
    int               firstLogged, badLogged;
    int               lArmed, rArmed; /* stick seen at rest (see Pad_BuildPsx) */
    unsigned          rumL, rumH;     /* last rumble the pad ACCEPTED; ~0u resends */
} PadDev;

static PadDev         s_dev[PAD_DEV_MAX];
static int            s_plDev[2]  = { -1, -1 }; /* s_dev index of each player's pad */
static int            s_plPort[2];              /* the port each player last used */
static int            s_p2Joined;
static unsigned char* s_padBuf  = 0;            /* P1: &g_GameWork.rawController */
static unsigned char* s_pad2Buf = 0;            /* P2: g_Controller1's analogController */
static unsigned char  s_p2Raw[8];

/* Rumble targets per PSX socket (0 = port 1, 0x10 = port 2), sampled per frame
 * by Pad_Poll (USB-safe) out of the actuator buffers PadSetAct registered. */
static unsigned char* s_actTable[2];
static int            s_actLen[2];
static int            s_rumbleValid[2];

/* Player 2 has no vibration engine of its own (the game's is one self-referential
 * instance, Player 1's): split screen sends its effects here as timed pulses on
 * the large motor (split_xbox.c). */
static unsigned       s_p2PulseLevel;
static uint32_t       s_p2PulseEnd;

static void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;                          /* connected */
    b[1] = 0x41;                          /* digital pad */
    b[2] = 0xFF; b[3] = 0xFF;             /* all buttons released (active-low) */
    b[4] = 0x80; b[5] = 0x80;             /* right stick neutral */
    b[6] = 0x80; b[7] = 0x80;             /* left stick neutral */
}

/* The front port a pad is plugged into, from nxdk SDL's xid_get_device_port
 * (SDL_xboxjoystick.c), with the parent dereference guarded. */
static int Pad_XidPort(xid_dev_t* x)
{
    UDEV_T* u   = (x && x->iface) ? x->iface->udev : NULL;
    int     hub = (XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB) != 0;

    while (u)
    {
        UDEV_T* up = (u->parent && u->parent->iface) ? u->parent->iface->udev : NULL;

        if ((hub && up && !up->parent) || (!hub && !u->parent))
        {
            switch (u->port_num)
            {
                case 3: return 1;
                case 4: return 2;
                case 1: return 3;
                case 2: return 4;
                default: return 0;
            }
        }
        u = up;
    }
    return 0;
}

/* USB interrupt-read completion: copy the gamepad report and re-arm.
 *
 * The report is VALIDATED before it is accepted. xid_driver.c allocates the
 * transfer buffer with usbh_alloc_mem and never clears it, so until the pad
 * actually fills it the buffer holds whatever junk was last in the USB heap --
 * and a completion that reports a full length hands us that junk as a
 * controller state. Read as a report its stick fields are arbitrary, which is
 * why the main menu sometimes flew upwards from the moment it appeared and
 * stopped the instant the stick was touched (the first real state change finally
 * overwrote the buffer). Every genuine XID gamepad report self-describes with
 * startByte 0 and bLength 20, which junk essentially never satisfies.
 *
 * A failed or aborted transfer is NOT resubmitted. On an unplug nxdk frees the
 * device's transfers before the abort is delivered, so resubmitting handed the
 * host controller a freed transfer; Pad_Poll arms a fresh read instead. */
static void Pad_ReadCb(UTR_T* utr)
{
    xid_dev_t* x = (xid_dev_t*)utr->context;
    PadDev*    d = x ? (PadDev*)x->user_data : NULL;

    if (!d || d->dev != x || d->uid != x->uid)
        return;
    if (utr->status < 0)
    {
        d->readArmed = 0;
        return;
    }
    if (utr->xfer_len >= sizeof(xid_gamepad_in))
    {
        const xid_gamepad_in* in = (const xid_gamepad_in*)utr->buff;

        if (in->startByte == 0 && in->bLength == sizeof(xid_gamepad_in))
        {
            memcpy(&d->report, in, sizeof(d->report));
            d->haveReport = 1;
            __asm__ __volatile__("" ::: "memory");
            d->seq++;
        }
        else if (!d->badReport)
        {
            d->badStart  = in->startByte;
            d->badLen    = in->bLength;
            d->badXfer   = utr->xfer_len;
            d->badReport = 1;
        }
    }
    utr->xfer_len        = 0;
    utr->bIsTransferDone = 0;
    if (usbh_int_xfer(utr) != USBH_OK)
    {
        /* Left done so the next usbh_xid_read reaps it. */
        utr->bIsTransferDone = 1;
        d->readArmed         = 0;
    }
}

static void Pad_Arm(PadDev* d)
{
    int r = usbh_xid_read(d->dev, 0, Pad_ReadCb);
    d->readArmed = (r == USBH_OK || r == HID_RET_XFER_IS_RUNNING);
}

/* A consistent copy of a pad's last report. Single CPU: the interrupt handler
 * always runs to completion, so a copy taken while seq did not move is whole. */
static int Pad_Snap(int i, xid_gamepad_in* out)
{
    PadDev*  d;
    uint32_t s;

    if (i < 0 || i >= PAD_DEV_MAX)
        return 0;
    d = &s_dev[i];
    if (!d->dev || !d->haveReport)
        return 0;
    do
    {
        s = d->seq;
        __asm__ __volatile__("" ::: "memory");
        memcpy(out, (const void*)&d->report, sizeof(*out));
        __asm__ __volatile__("" ::: "memory");
    } while (s != d->seq);
    return 1;
}

/* The player a newly attached pad belongs to: one coming back on its player's
 * port first, then the lowest player without a pad. Before split screen binds
 * Player 2 only Player 1 exists, which is the original single-pad rule. */
static int Pad_PlayerFor(int port)
{
    int n = s_p2Joined ? 2 : 1;
    int p;

    for (p = 0; p < n; p++)
        if (s_plDev[p] < 0 && s_plPort[p] && s_plPort[p] == port)
            return p;
    for (p = 0; p < n; p++)
        if (s_plDev[p] < 0)
            return p;
    return -1;
}

/* Connect / disconnect run on the game thread, inside usbh_pooling_hubs. */
static void Pad_ConnCb(xid_dev_t* x, int param)
{
    int i, p;

    (void)param;
    if (x->xid_desc.bType != XID_TYPE_GAMECONTROLLER)
        return;
    for (i = 0; i < PAD_DEV_MAX && s_dev[i].dev; i++)
    {
    }
    if (i == PAD_DEV_MAX)
        return;

    memset(&s_dev[i], 0, sizeof(s_dev[i]));
    s_dev[i].uid  = x->uid;
    s_dev[i].port = Pad_XidPort(x);
    s_dev[i].rumL = s_dev[i].rumH = ~0u;
    x->user_data  = &s_dev[i];
    s_dev[i].dev  = x;

    p = Pad_PlayerFor(s_dev[i].port);
    if (p >= 0)
    {
        s_plDev[p]  = i;
        s_plPort[p] = s_dev[i].port;
    }
    Pad_Arm(&s_dev[i]);
    SH_DBG("[PAD] attach slot=%d port=%d -> %s", i, s_dev[i].port, p < 0 ? "spare" : p ? "P2" : "P1");
}

static void Pad_DisconnCb(xid_dev_t* x, int param)
{
    PadDev* d = (PadDev*)x->user_data;
    int     i, p;

    (void)param;
    if (!d || d->dev != x)
        return;
    /* dev first, so a completion still in flight stops matching. */
    d->dev       = NULL;
    x->user_data = NULL;
    i            = (int)(d - s_dev);
    for (p = 0; p < 2; p++)
        if (s_plDev[p] == i)
            s_plDev[p] = -1;
    SH_DBG("[PAD] detach slot=%d port=%d", i, d->port);
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
    s_padBuf  = (unsigned char*)pad1;
    s_pad2Buf = (unsigned char*)pad2;
    if (s_padBuf)
        Pad_FillIdle(s_padBuf);
}

void PadStartCom(void) { }

/* Soft reset: L trigger + R trigger + Start + White button, held ~0.4s (the
 * four-way combo is awkward enough that no debounce is needed beyond the hold).
 * Relaunches our own XBE for an instant reload after a crash/hang — much faster
 * than a dashboard round-trip. XReboot is the fallback if relaunch fails. */
static void Pad_CheckSoftReset(const xid_gamepad_in* r)
{
    static int held = 0;
    if (r &&
        r->l     > 0x40 &&
        r->r     > 0x40 &&
        (r->dButtons & (1u << 4)) &&      /* Start */
        r->white > 0x40) {
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

/* Xbox XID gamepad -> PSX digital pad. PSX digitalButtons is active-low, so start
 * "all released" (0xFFFF) and clear a bit per pressed button. Player 1 keeps the
 * extra-action mutes, the black/white quick save and the modern trigger binds;
 * Player 2 gets the plain mapping (split screen runs the Classic camera). */
static void Pad_BuildPsx(const xid_gamepad_in* rep, PadDev* d, int isP1, unsigned char* out)
{
    unsigned short xb  = rep->dButtons;
    unsigned short psx = 0xFFFF;
    /* Buttons bound to an extra action (xbox_extra_actions.c) send only
     * that action during gameplay, not their PSX button as well. */
    unsigned       mute = isP1 ? g_XbExtraMute : 0;
    #define PRESS(bit) (psx &= (unsigned short)~(1u << (bit)))
    #define LIVE(b)    (!(mute & (1u << (b))))
    if ((xb & (1u << 0)) && LIVE(XB_PHYS_UP))    PRESS(4);   /* DPad Up    -> Up    */
    if ((xb & (1u << 1)) && LIVE(XB_PHYS_DOWN))  PRESS(6);   /* DPad Down  -> Down  */
    if ((xb & (1u << 2)) && LIVE(XB_PHYS_LEFT))  PRESS(7);   /* DPad Left  -> Left  */
    if ((xb & (1u << 3)) && LIVE(XB_PHYS_RIGHT)) PRESS(5);   /* DPad Right -> Right */
    if (xb & (1u << 4)) PRESS(3);   /* Start      -> Start */
    if ((xb & (1u << 5)) && LIVE(XB_PHYS_BACK))  PRESS(0);   /* Back       -> Select*/
    if ((xb & (1u << 6)) && LIVE(XB_PHYS_L3))    PRESS(1);   /* L3         -> L3    */
    if ((xb & (1u << 7)) && LIVE(XB_PHYS_R3))    PRESS(2);   /* R3         -> R3    */
    if (rep->a     > 0x20) PRESS(14);  /* A     -> Cross    */
    if (rep->b     > 0x20) PRESS(13);  /* B     -> Circle   */
    if (rep->x     > 0x20) PRESS(15);  /* X     -> Square   */
    if (rep->y     > 0x20) PRESS(12);  /* Y     -> Triangle */
    /* White/Black normally map to L1/R1 = step left / step right. With
     * bw_quick_save on they become quick save / quick load
     * (quicksave_xbox.c) and must STOP emitting L1/R1, or every quick save
     * would sidestep at the same time. */
    if (!isP1 || !g_PcConfig.bwQuickSave) {
        if (rep->white > 0x20 && LIVE(XB_PHYS_WHITE)) PRESS(10);  /* White -> L1 */
        if (rep->black > 0x20 && LIVE(XB_PHYS_BLACK)) PRESS(11);  /* Black -> R1 */
    }
    #undef LIVE
    /* STANDARD (modern shooter) binds while a NON-CLASSIC camera is active:
     * hold LT to aim, press RT to fire. A stays Cross, so RT and A both fire
     * when aiming and both "use" when not — the game only ever tests Cross.
     *
     * Done at the INPUT layer on purpose. The alternative is editing
     * g_GameWork.config.controllerConfig (.aim/.action), which is savegame
     * state and is re-read by every screen; remapping here is a pure
     * translation with nothing downstream to keep in sync, and it reverts the
     * instant the style goes back to Classic. Classic keeps the PSX binds
     * exactly (RT aim, A fire) because tank controls depend on them. */
    {
        extern int g_ControlStyle;
        int modern = isP1 && g_PcConfig.stdAltCamControls && g_ControlStyle != 0;

        if (modern) {
            if (rep->l > 0x20) PRESS(9);   /* LT -> R2    = aim (hold) */
            if (rep->r > 0x20) PRESS(14);  /* RT -> Cross = fire / use */
        } else {
            if (rep->l > 0x20) PRESS(8);   /* LTrig -> L2 */
            if (rep->r > 0x20) PRESS(9);   /* RTrig -> R2 */
        }
    }
    /* Left analog stick -> d-pad directions: walk/turn with the stick in
     * the classic tank scheme (up = forward, left/right = turn), ORed with
     * the real d-pad. XID sticks are s16, Y+ = up / X+ = right.
     * This is the ONLY stick->UI path (the pad reports as DIGITAL, status
     * 0x41, so the game's analog->digital stage ignores the sticks entirely —
     * the right-stick camera reads its bytes directly). The deadzone was 8000
     * (~24%); on a drifting controller the drift sat near that edge and
     * toggled the d-pad bit every frame, and the main menu edge-reads that as
     * repeated presses -> the cursor flew through the options the moment the
     * menu appeared. Raised to ~40% so typical drift stays firmly below the
     * threshold; a real push still walks/navigates. */
    #define STICK_DZ 13000
    /* A stick is not trusted until it has been seen AT REST at least once.
     * A pad that comes up reporting a deflected stick (see Pad_ReadCb, and
     * pads whose ADC reads stale until first moved) would otherwise hold a
     * direction from boot, which the menu's hold-repeat turns into an
     * endless scroll. Arming costs nothing in the normal case: a stick at
     * rest arms on the very first report. */
    if (!d->lArmed &&
        rep->leftStickX > -STICK_DZ && rep->leftStickX < STICK_DZ &&
        rep->leftStickY > -STICK_DZ && rep->leftStickY < STICK_DZ) {
        d->lArmed = 1;
    }
    if (d->lArmed) {
        if (rep->leftStickY >  STICK_DZ) PRESS(4);   /* up    -> forward */
        if (rep->leftStickY < -STICK_DZ) PRESS(6);   /* down  -> back    */
        if (rep->leftStickX < -STICK_DZ) PRESS(7);   /* left  -> turn L  */
        if (rep->leftStickX >  STICK_DZ) PRESS(5);   /* right -> turn R  */
    }
    #undef STICK_DZ
    #undef PRESS
    out[0] = 0x00;
    out[1] = 0x41;
    out[2] = (unsigned char)(psx & 0xFF);
    out[3] = (unsigned char)((psx >> 8) & 0xFF);
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
        int rawx = (int)rep->rightStickX;
        int rawy = (int)rep->rightStickY;
        int rxb, ryb;
        /* Same arming rule as the left stick, for the same reason: an
         * untrusted right stick reports dead centre, so no camera motion and
         * no analog->digital menu pulses. */
        if (!d->rArmed &&
            rawx > -RSTICK_DZ && rawx < RSTICK_DZ &&
            rawy > -RSTICK_DZ && rawy < RSTICK_DZ) {
            d->rArmed = 1;
        }
        if (!d->rArmed) { rawx = 0; rawy = 0; }
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
        out[4] = (unsigned char)rxb;   /* rightX */
        out[5] = (unsigned char)ryb;   /* rightY */
        #undef RSTICK_DZ
    }
    out[6] = 0x80; out[7] = 0x80;
}

/* Sample a socket's PSX actuator buffer LIVE — the engine writes into it every
 * frame without re-calling PadSetAct (see there). table[0] = small
 * (high-freq) motor 0/1, table[1] = large (low-freq) motor 0-255.
 * usbh_xid_rumble(l, h) takes 0..65535, so *255, with PsyCross's
 * minimal-shake clamp so a barely-nonzero value still moves the motor. Sent
 * only on a change, from this USB-polling context, never from PadSetAct's; the
 * motor holds its state, so an idle {0,0} stops it. The last value is only
 * recorded once the pad has accepted it, so a command refused while the
 * transfer queue was full is sent again next frame rather than leaving a motor
 * running. */
static void Pad_Rumble(int p)
{
    PadDev*  d;
    unsigned h = 0, l = 0;

    if (!s_rumbleValid[p] || s_plDev[p] < 0)
        return;
    d = &s_dev[s_plDev[p]];
    if (!d->dev)
        return;
    if (s_actTable[p] && s_actLen[p] > 0) {
        h = (unsigned)s_actTable[p][0] * 255u;
        if (s_actLen[p] > 1) l = (unsigned)s_actTable[p][1] * 255u;
        if (l != 0 && l < 4096) l = 4096;
        if (h != 0 && h < 4096) h = 4096;
    } else if (p == 1 && s_p2PulseLevel) {
        if ((int32_t)((uint32_t)KeTickCount - s_p2PulseEnd) < 0) {
            l = s_p2PulseLevel * 255u;
            if (l < 4096) l = 4096;
        } else {
            s_p2PulseLevel = 0;
        }
    }
    if (l != d->rumL || h != d->rumH) {
        if (usbh_xid_rumble(d->dev, (unsigned short)l, (unsigned short)h) == USBH_OK) {
            static int s_firstBuzz[2];
            if ((l || h) && !s_firstBuzz[p]) {
                s_firstBuzz[p] = 1;
                SH_DBG("[VIB] P%d rumble l=%u h=%u (engine -> motors live)", p + 1, l, h);
            }
            d->rumL = l;
            d->rumH = h;
        }
    }
}

void Pad_Poll(void)
{
    xid_gamepad_in rep;
    int            i;

    usbh_pooling_hubs();   /* pump the USB host stack (connects, disconnects) */

    for (i = 0; i < PAD_DEV_MAX; i++)
    {
        PadDev* d = &s_dev[i];

        if (!d->dev)
            continue;
        if (!d->readArmed)
            Pad_Arm(d);
        if (d->haveReport && !d->firstLogged)
        {
            d->firstLogged = 1;
            SH_DBG("[PAD] first report slot=%d port=%d: lx=%d ly=%d rx=%d ry=%d", i, d->port,
                   (int)d->report.leftStickX,  (int)d->report.leftStickY,
                   (int)d->report.rightStickX, (int)d->report.rightStickY);
        }
        if (d->badReport && !d->badLogged)
        {
            d->badLogged = 1;
            SH_DBG("[PAD] rejected malformed report slot=%d: start=%u len=%u xfer=%u",
                   i, d->badStart, d->badLen, d->badXfer);
        }
    }

    Pad_Rumble(0);
    if (s_p2Joined)
        Pad_Rumble(1);

    if (!s_padBuf)
        return;

    if (Pad_Snap(s_plDev[0], &rep))
    {
        Pad_CheckSoftReset(&rep);
        Pad_BuildPsx(&rep, &s_dev[s_plDev[0]], 1, s_padBuf);
    }
    else
    {
        Pad_CheckSoftReset(NULL);
        Pad_FillIdle(s_padBuf);
    }

    if (s_p2Joined)
    {
        if (Pad_Snap(s_plDev[1], &rep))
            Pad_BuildPsx(&rep, &s_dev[s_plDev[1]], 0, s_p2Raw);
        else
            Pad_FillIdle(s_p2Raw);
    }
}

/* ---- Player 2 (split screen, split_xbox.c) ---- */

/* The attached controllers Player 1 is not using, ordered by port: Player 2's
 * choices, its own current pad included (the menu reopens with it still bound).
 * Returns how many were written. */
int Pad_XboxSparePorts(int* ports, int max)
{
    int n = 0, port;

    for (port = 1; port <= 4 && n < max; port++)
    {
        int i;
        for (i = 0; i < PAD_DEV_MAX; i++)
        {
            if (s_dev[i].dev && s_dev[i].port == port && i != s_plDev[0])
            {
                ports[n++] = port;
                break;
            }
        }
    }
    return n;
}

int Pad_XboxPlayerPort(int p)
{
    if (p < 0 || p > 1 || s_plDev[p] < 0)
        return 0;
    return s_dev[s_plDev[p]].port;
}

int Pad_XboxP2Connected(void)
{
    return s_p2Joined && s_plDev[1] >= 0 && s_dev[s_plDev[1]].dev != NULL;
}

/* Give Player 2 the spare pad on `port` (0 = the first spare). A pad that
 * disconnects keeps Player 2's port, so plugging it back in rebinds it. */
int Pad_XboxBindP2(int port)
{
    int i, pick = -1;

    for (i = 0; i < PAD_DEV_MAX; i++)
    {
        if (!s_dev[i].dev || i == s_plDev[0])
            continue;
        if (port == 0 || s_dev[i].port == port)
        {
            if (pick < 0 || s_dev[i].port < s_dev[pick].port)
                pick = i;
            if (port != 0)
                break;
        }
    }
    s_p2Joined = 1;
    s_plDev[1] = pick;
    s_plPort[1] = (pick >= 0) ? s_dev[pick].port : port;
    if (pick >= 0)
        s_dev[pick].rumL = s_dev[pick].rumH = ~0u;
    Pad_FillIdle(s_p2Raw);
    SH_DBG("[PAD] Player 2 bound: slot=%d port=%d", pick, s_plPort[1]);
    return pick >= 0;
}

/* Back to one player: stop Player 2's motors and leave controllers[1] exactly
 * as a single-player game has it (zeroed; nothing else ever writes it here). */
void Pad_XboxUnbindP2(void)
{
    if (!s_p2Joined)
        return;
    if (s_plDev[1] >= 0 && s_dev[s_plDev[1]].dev)
        usbh_xid_rumble(s_dev[s_plDev[1]].dev, 0, 0);
    s_p2Joined        = 0;
    s_plDev[1]        = -1;
    s_plPort[1]       = 0;
    s_actTable[1]     = 0;
    s_actLen[1]       = 0;
    s_rumbleValid[1]  = 0;
    s_p2PulseLevel    = 0;
    if (s_pad2Buf)
        memset(s_pad2Buf, 0, 8);
    SH_DBG("[PAD] Player 2 unbound");
}

void Pad_XboxP2Pulse(unsigned level, unsigned ms)
{
    uint32_t end = (uint32_t)KeTickCount + ms;

    if (!s_p2Joined || level == 0)
        return;
    if (s_p2PulseLevel && (int32_t)((uint32_t)KeTickCount - s_p2PulseEnd) < 0 && level < s_p2PulseLevel)
        level = s_p2PulseLevel;
    s_p2PulseLevel   = level > 255 ? 255 : level;
    s_p2PulseEnd     = end;
    s_rumbleValid[1] = 1;
}

/* Right after Joy_ReadP1 copies Player 1's raw bytes, put Player 2's where the
 * PSX pad 2 port would have written them, so Joy_ControllerDataUpdate derives
 * Player 2's held / clicked / pulsed flags in controllers[1] the PSX way. */
void Pad_XboxLatchP2(void)
{
    if (s_p2Joined && s_pad2Buf)
        memcpy(s_pad2Buf, s_p2Raw, 8);
}

/* The physical buttons an extra action can be bound to, as a mask of
 * 1 << XB_PHYS_*, read straight from Player 1's report (before the PSX mapping,
 * so a muted button still reads). */
unsigned Pad_XboxPhysButtons(void)
{
    xid_gamepad_in r;
    unsigned short xb;
    unsigned       m = 0;

    if (!Pad_Snap(s_plDev[0], &r))
        return 0;
    xb = r.dButtons;
    if (xb & (1u << 0)) m |= 1u << XB_PHYS_UP;
    if (xb & (1u << 3)) m |= 1u << XB_PHYS_RIGHT;
    if (xb & (1u << 1)) m |= 1u << XB_PHYS_DOWN;
    if (xb & (1u << 2)) m |= 1u << XB_PHYS_LEFT;
    if (xb & (1u << 5)) m |= 1u << XB_PHYS_BACK;
    if (xb & (1u << 6)) m |= 1u << XB_PHYS_L3;
    if (xb & (1u << 7)) m |= 1u << XB_PHYS_R3;
    if (r.white > 0x20) m |= 1u << XB_PHYS_WHITE;
    if (r.black > 0x20) m |= 1u << XB_PHYS_BLACK;
    return m;
}

/* Raw BLACK/WHITE analog-button state, for quicksave_xbox.c. Reported straight
 * from the pad rather than through the PSX button word because with
 * bw_quick_save on these deliberately no longer appear there. Same 0x20
 * threshold the button mapping uses. Returns 0 when no pad has reported. */
int Pad_XboxBlackWhite(int* black, int* white)
{
    xid_gamepad_in r;

    if (black) *black = 0;
    if (white) *white = 0;
    if (!Pad_Snap(s_plDev[0], &r))
        return 0;
    if (black) *black = (r.black > 0x20);
    if (white) *white = (r.white > 0x20);
    return 1;
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
    /* REGISTER THE BUFFER, do not sample it.
     *
     * On PSX this hands libpad a POINTER that the pad DMA re-sends every frame:
     * the engine then just writes new actuator bytes into that buffer and the
     * hardware picks them up, WITHOUT calling PadSetAct again. func_8009E61C
     * relies on exactly that — its `field_0_23` latch means PadSetAct is called
     * only on configuration edges, not per effect. Sampling table[] here
     * therefore captured whatever the buffer held at that one instant (zero) and
     * held it forever, which is why rumble never fired once in a 65k-line
     * session with combat: not one [VIB] line. Pad_Poll now reads the live bytes
     * out of this buffer each frame.
     *
     * Port 1 uses &g_SysWork.field_2514.actuatorData_4 — a persistent global, so
     * the pointer stays valid; split screen's Player 2 engine registers its own
     * persistent buffer on socket 0x10. len 0 means "actuators off" on PSX. */
    int p = (socket >> 4) & 1;

    s_actTable[p]    = table;
    s_actLen[p]      = len;
    s_rumbleValid[p] = 1;
    {
        static int s_logged[2];
        if (!s_logged[p]) {
            s_logged[p] = 1;
            SH_DBG("[VIB] PadSetAct registered: socket=0x%x table=%p len=%d (buffer is polled live)",
                   socket, (void*)table, len);
        }
    }
}
