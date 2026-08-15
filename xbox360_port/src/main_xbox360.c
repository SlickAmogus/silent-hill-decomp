/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * main_xbox360.c - Xbox 360 entry point (mirror of xbox_port/src/main_xbox.c).
 *
 * Milestone 2 scope: bring up libXenon, prove the log sink writes somewhere a
 * USB stick can be read back from, and hold a frame. The game loop is not
 * called yet -- the HAL it needs (GPU/audio/pad/FS) does not exist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xenos/xenos.h>
#include <console/console.h>
#include <xenon_smc/xenon_smc.h>
#include <usb/usbmain.h>
#include <diskio/ata.h>
#include <input/input.h>
#include <ppc/timebase.h>

int main(void)
{
    xenos_init(VIDEO_MODE_AUTO);
    console_init();

    printf("Silent Hill - Xbox 360 port\n");
    printf("libXenon up, timebase=%llu\n", (unsigned long long)mftb());

    /* USB has to be enumerated before the log sink can reach a stick. */
    usb_init();
    usb_do_poll();

    for (;;)
    {
        console_clrline();
        usb_do_poll();
    }

    return 0;
}
