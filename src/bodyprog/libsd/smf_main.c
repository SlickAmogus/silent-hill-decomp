#include "common.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif

#include <psyq/libapi.h>

#include "bodyprog/libsd.h"

bool sd_interrupt_start_flag = false;
s16  sd_keyoff_mode          = 0;
// 2 bytes of padding.
bool sd_mono_st_flag         = false;
s32  sd_reverb_mode          = 0;
u32  body_partly_size        = 0;

#include "smf_tables.h"

s32 sd_reserved_voice = 24;
u32 spu_reverb_sw     = 0;

u32 spu_ch_tbl[24] = {
    1 << 0,
    1 << 1,
    1 << 2,
    1 << 3,
    1 << 4,
    1 << 5,
    1 << 6,
    1 << 7,
    1 << 8,
    1 << 9,
    1 << 10,
    1 << 11,
    1 << 12,
    1 << 13,
    1 << 14,
    1 << 15,
    1 << 16,
    1 << 17,
    1 << 18,
    1 << 19,
    1 << 20,
    1 << 21,
    1 << 22,
    1 << 23
};

bool sd_int_flag    = false;
bool sd_int_flag2   = false;
bool smf_start_flag = false;
s32  sd_timer_sync  = 0;
u32  timer_count[5] = { 0x1999, 0x2000, 0x4000, 0xFFFF, 0x1A80 };
bool sd_timer_flag  = false;
s32  time_flag      = 0xC0;
s32  smf_file_no    = 0;
u32  print_start    = 0;
char eof_char[3]    = { 0xFF, 0x2F, 0x00 };
// 1 byte of padding.
s32  chantype[18]   = { 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 1, 1, 2, 0, 0, 0 };

extern bool sd_timer_flag; // TODO: Only used in this file.

bool smf_timer(void) // 0x800A6D18
{
    if (!sd_interrupt_start_flag || sd_int_flag)
    {
        return true;
    }

    if (!sd_int_flag2)
    {
        sd_int_flag2 = true;
        midi_smf_main();

        if (sd_timer_sync >= 11)
        {
            midi_vsync();
            SdAutoKeyOffCheck();
            sd_timer_sync = 0;
        }

        sd_int_flag2 = false;
        sd_timer_sync++;
    }

    return false;
}

void smf_timer_set(void) // 0x800A6DC0
{
    if (!sd_timer_flag)
    {
        sd_timer_flag = true;

        EnterCriticalSection();
        sd_timer_event = OpenEvent(RCntCNT2, EvSpINT, EvMdINTR, smf_timer);
        EnableEvent(sd_timer_event);
        SetRCnt(RCntCNT2, 7328, RCntMdINTR); // ~30Hz?
        StartRCnt(RCntCNT2);
        ExitCriticalSection();

        sd_int_flag   = false;
        sd_timer_flag = false;
    }
}

void smf_timer_end(void) // 0x800A6E58
{
    sd_timer_flag = true;

    EnterCriticalSection();
    StopRCnt(RCntCNT2);
    DisableEvent(sd_timer_event);
    CloseEvent(sd_timer_event);
    ExitCriticalSection();

    sd_timer_flag = false;
    sd_int_flag   = false;
}

void smf_timer_stop(void) // 0x800A6EC8
{
    sd_timer_flag = true;

    EnterCriticalSection();
    StopRCnt(RCntCNT2);
    ExitCriticalSection();

    sd_timer_flag = false;
    sd_int_flag   = false;
}

void smf_vsync(void) // 0x800A6F14
{
    if (sd_int_flag2)
    {
        return;
    }

    sd_int_flag2 = true;

#if defined(SH_XBOX_PORT)
    /* CLOCK THE SEQUENCE BY REAL TIME, NOT BY FRAMES.
     *
     * Xbox has no RCnt2 hardware timer -- g_rcnt2_timer_active is a permanent 0
     * stub (xbox_compat_globals.c) -- so the per-VSync advance below IS the
     * sequence clock. Ten ticks per frame equals 600Hz only while the frame rate
     * holds 60; when the air-screamer fight halves it the MUSIC PLAYS AT HALF
     * SPEED. That is why a heavy scene felt like the whole game had slowed down
     * rather than merely gone choppy: the score slowed with it.
     *
     * Advancing by elapsed milliseconds keeps tempo independent of frame rate.
     * 600 ticks/sec reproduces today's behaviour exactly at 60fps, so a good
     * frame rate sounds unchanged and only the drop case is corrected. The carry
     * keeps the fractional remainder so tempo does not drift. Long gaps (map
     * loads) are clamped instead of fast-forwarding the song to catch up. */
    if (smf_start_flag)
    {
        extern int      GpuNv2a_Ms(void);
        static unsigned s_smfLastMs = 0;
        static int      s_smfCarry  = 0;
        unsigned        now = (unsigned)GpuNv2a_Ms();
        unsigned        dt;
        int             ticks;

        if (s_smfLastMs == 0)
            s_smfLastMs = now;
        dt = now - s_smfLastMs;
        if (dt > 250)
            dt = 250;                       /* after a hitch, resume — don't sprint */
        s_smfLastMs = now;

        /* 577.8Hz -- the rate of the RCnt2 timer that IS the sequence clock when
         * sd_tick_mode==1, which is the mode this game uses. The 10-per-frame
         * path was PSX's ALTERNATE clock for tick_mode 0/>=4 and works out to
         * 600Hz at 60fps: 3.8% sharp, which is the "something still sounds a
         * little off" left after the frame-rate coupling was fixed. */
        s_smfCarry += (int)dt * 5778;       /* 577.8 ticks per 1000 ms */
        ticks       = s_smfCarry / 10000;
        s_smfCarry -= ticks * 10000;

        while (ticks-- > 0)
            midi_smf_main();
    }
#else
#ifdef SH_PC_PORT
    /* BGM-slightly-fast + cutscene note-buzz ROOT: the sequencer is driven by
     * BOTH the RCnt2 hardware timer (smf_timer @ ~577.8Hz, started because
     * sd_tick_mode==1) AND this per-VSync 10x advance (vsync.c -> SdSeqCalledTbyT
     * -> smf_vsync). On PSX tick_mode 1-3 means the TIMER is the sequence clock;
     * the per-frame 10x advance is the alternate clock for tick_mode 0/>=4. On PC
     * both ran, double-driving the sequence -> BGM plays fast and short notes
     * retrigger every frame (the 0x6b840 buzz). When the RCnt2 timer is active it
     * is the sole sequence clock; smf_vsync then only does modulation below. */
    extern int g_rcnt2_timer_active;
    if (smf_start_flag && !g_rcnt2_timer_active)
#else
    if (smf_start_flag)
#endif
    {
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
        midi_smf_main();
    }
#endif /* SH_XBOX_PORT */

    midi_vsync();
    SdAutoKeyOffCheck();
    sd_int_flag2 = false;
}
