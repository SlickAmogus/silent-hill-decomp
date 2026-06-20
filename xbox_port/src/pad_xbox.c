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

static unsigned char* s_padBuf  = 0;
static xid_dev_t*     s_xid      = 0;
static xid_gamepad_in s_report;
static int            s_haveReport = 0;

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
void Pad_Poll(void)
{
    usbh_pooling_hubs();   /* pump the USB host stack (fires Pad_ReadCb) */

    if (!s_padBuf)
        return;

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
        #undef PRESS
        s_padBuf[0] = 0x00;
        s_padBuf[1] = 0x41;
        s_padBuf[2] = (unsigned char)(psx & 0xFF);
        s_padBuf[3] = (unsigned char)((psx >> 8) & 0xFF);
        s_padBuf[4] = 0x80; s_padBuf[5] = 0x80;
        s_padBuf[6] = 0x80; s_padBuf[7] = 0x80;
    } else {
        Pad_FillIdle(s_padBuf);
    }
}

int PadGetState(int port)  { (void)port; return s_xid ? 6 : 6; }  /* 6 = stable */
int PadInfoMode(void)      { return 0; }
int PadSetMainMode(void)   { return 0; }
int PadSetActAlign(void)   { return 0; }
int PadSetAct(void)        { return 0; }
int PadChkVsync(void)      { return 0; }
