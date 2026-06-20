/*
 * pad_xbox.c - PSX controller (libpad) on Xbox.
 *
 * The game calls PadInitDirect(&g_GameWork.rawController, ...) once, then reads
 * that raw PSX pad buffer every frame (Joy_ReadP1 -> Joy_ControllerDataUpdate,
 * which does heldBtnFlags = ~digitalButtons). PSX buttons are ACTIVE-LOW
 * (0 = pressed), so an unfilled all-zero buffer reads as EVERY button held — the
 * cause of the garbage input on the first boots. We capture the buffer pointer
 * and refresh it each frame (Pad_Poll, called from VSync) to a valid idle state.
 *
 * TODO (next, with live testing): read the real USB pad via nxdk's XID driver and
 * map it into the PSX format below — usbh_core_init() + usbh_xid_init() +
 * usbh_install_xid_conn_callback(); pump usbh_pooling_hubs() in Pad_Poll; start a
 * usbh_xid_read() on connect and re-arm it in the completion callback; then map
 * xid_gamepad_in.dButtons (dpad/start/back/L3/R3) + a/b/x/y/black/white/l/r +
 * sticks into digitalButtons (active-low) and the 4 stick bytes. If that read
 * fails or no pad is present, we keep the idle fill below — so no regression.
 *
 * PSX s_AnalogController layout (see include/bodyprog/sys/joy.h):
 *   [0] status        : 0x00 connected, 0xFF disconnected
 *   [1] recv:4|term:4 : 0x41 = 16-button digital pad, 2 button bytes
 *   [2..3] digitalButtons (u16, active-low)
 *   [4..7] rightX, rightY, leftX, leftY (0x80 = neutral)
 */

static volatile unsigned char* s_padBuf = 0;

static void Pad_FillIdle(unsigned char* b)
{
    b[0] = 0x00;                         /* connected */
    b[1] = 0x41;                         /* digital pad */
    b[2] = 0xFF; b[3] = 0xFF;            /* all buttons released (active-low) */
    b[4] = 0x80; b[5] = 0x80;            /* right stick neutral */
    b[6] = 0x80; b[7] = 0x80;            /* left stick neutral */
}

void PadInitDirect(void* pad1, void* pad2)
{
    (void)pad2;
    s_padBuf = (unsigned char*)pad1;
    if (s_padBuf)
        Pad_FillIdle((unsigned char*)s_padBuf);   /* avoid an all-pressed first frame */
}

void PadStartCom(void) { }

/* Refresh the PSX pad buffer; called once per frame from VSync. Real USB read
 * goes here later (see TODO above); idle for now. */
void Pad_Poll(void)
{
    if (s_padBuf)
        Pad_FillIdle((unsigned char*)s_padBuf);
}

int PadGetState(int port)  { (void)port; return 6; }   /* 6 = PadStateStable */
int PadInfoMode(void)      { return 0; }
int PadSetMainMode(void)   { return 0; }
int PadSetActAlign(void)   { return 0; }
int PadSetAct(void)        { return 0; }
int PadChkVsync(void)      { return 0; }
