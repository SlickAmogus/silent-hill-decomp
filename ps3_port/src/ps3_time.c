/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ps3_time.c - time-base queries for the PS3 port.
 *
 * Includes PSL1GHT and NO decomp headers, which is the contract described in
 * ps3_hal.h: the two header sets define incompatible u64/s64 and cannot share
 * a translation unit.
 */
#include <sys/systime.h>

#include "ps3_hal.h"

unsigned long long Ps3_TimebaseFreq(void)
{
    return (unsigned long long)sysGetTimebaseFrequency();
}

void Ps3_SleepMs(unsigned ms)
{
    if (ms)
        sysUsleep(ms * 1000u);
    else
        sysUsleep(0);   /* plain yield */
}
