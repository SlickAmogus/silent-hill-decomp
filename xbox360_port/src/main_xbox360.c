/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main_xbox360.c - Xbox 360 entry point (mirror of xbox_port/src/main_xbox.c).
 *
 * Milestone 2 scope: bring libXenon up, mount the USB stick, prove the log sink
 * writes somewhere readable on a PC, and hold a frame. The game loop is not
 * called yet -- the HAL it needs (GPU/audio/pad/FS) does not exist. The point of
 * this build is to validate the deployment path (BadUpdate -> FreeMyXe -> XeLL
 * -> xenon.elf) on real hardware BEFORE any of that HAL gets written, because
 * the exploit is ~30% success and non-persistent, so a boot test is expensive
 * and worth de-risking on its own.
 */

#include <stdio.h>

#include <xenos/xenos.h>
#include <console/console.h>
#include <usb/usbmain.h>
#include <libfat/fat.h>
#include <ppc/timebase.h>

#include "sh_log.h"
#include "fs_xbox360.h"

/* Both live in sh_log_xbox360.c: port-specific, so not in the shared sh_log.h. */
extern const char* g_ShLogPath;
extern void        SH_DebugLogFlush(void);

/* cd_xbox360.c */
extern char  g_CdBinPath[];
extern void  Cd_XboxInit(void);
extern int   Cd_XboxSelfTest(char* idOut, int idOutSize);

int main(void)
{
    uint64_t bootTb;
    int      fatOk;
    unsigned frame = 0;

    xenos_init(VIDEO_MODE_AUTO);
    console_init();

    bootTb = mftb();
    printf("Silent Hill - Xbox 360 port (milestone 2 boot test)\n");

    /* USB has to enumerate before libfat can find the stick to mount uda:. */
    usb_init();
    usb_do_poll();

    fatOk = fatInitDefault();
    printf("fatInitDefault: %s\n", fatOk ? "ok" : "FAILED");

    SH_DebugLogInit();
    printf("log: %s\n", g_ShLogPath);

    SH_DBG("[BOOT] Silent Hill 360 alive, timebase=%llu", (unsigned long long)bootTb);
    SH_DBG("[BOOT] fat=%d log=%s", fatOk, g_ShLogPath);

    /* Locate and open the disc image, and actually pull a sector through the
     * cooked path. Finding the file proves nothing on its own -- a wrong sector
     * size or a truncated rip both survive fopen and only fail on read. */
    {
        int  haveBin = Sh360Fs_Init();
        char pvd[8];

        Cd_XboxInit();
        printf("bin: %s\n", g_CdBinPath);
        SH_DBG("[BOOT] root='%s' bin='%s' path='%s' found=%d",
               Sh360Fs_DataRoot(), Sh360Fs_BinName(), g_CdBinPath, haveBin);

        if (Cd_XboxSelfTest(pvd, sizeof(pvd))) {
            SH_DBG("[BOOT] PVD id='%s' (expect CD001)", pvd);
            printf("PVD id: %s (expect CD001)\n", pvd);
        } else {
            SH_DBG("[BOOT] BIN not open - game data unavailable");
            printf("bin: NOT OPEN - game data unavailable\n");
        }
    }
    SH_DebugLogFlush();

    printf("Boot test running. Press eject/power to exit.\n");

    for (;;)
    {
        usb_do_poll();
        /* One line a second or so, so a log pulled off the stick proves the loop
         * kept running rather than just that main() was reached. */
        if ((++frame % 60) == 0)
        {
            SH_DBG("[BOOT] alive frame=%u tb=%llu", frame, (unsigned long long)mftb());
            SH_DebugLogFlush();
        }
    }

    return 0;
}
