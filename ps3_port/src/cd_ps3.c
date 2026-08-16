/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * cd_ps3.c - PSX CD-ROM (libcd) on PS3, backed by the game's BIN disc image.
 *
 * Port of xbox_port/src/cd_xbox.c via the 360's version. The sector logic is
 * identical and deliberately so: CdControl(CdlSetloc) sets the read sector,
 * CdRead copies the 2048-byte user data out of each 2352-byte Mode-2/Form-1
 * sector (payload at offset 24: 12 sync + 3 addr + 1 mode + 8 subheader). The
 * PSX async model collapses to synchronous reads, so CdReadSync/CdSync report
 * complete immediately.
 *
 * What differs per console is only how the BIN is FOUND; fs_ps3.c has already
 * picked the directory and filename.
 *
 * Self-contained CdlLOC + Cdl* constants, matching the Xbox and 360 ports: this
 * TU never includes <libcd.h>, so it cannot clash with the decomp types. C links
 * by name and CdlLOC is the standard {minute,second,sector,track} BCD layout, so
 * it matches the game's libcd prototypes at the ABI level.
 *
 * Note the BIN is read with plain newlib stdio. That keeps this TU free of
 * <ppu-types.h> and, more usefully, means the CD-rate throttle and the buffered
 * gulp read behave exactly as they do on the other ports.
 */
#include <stdio.h>
#include <string.h>

#include "sh_log.h"
#include "fs_ps3.h"

char g_CdBinPath[SH_PS3_PATH_MAX * 2] = "NOT FOUND";

typedef unsigned char u8;

#define CDL_SETLOC      0x02
#define CDL_COMPLETE    0x02
#define BIN_SECTOR_SIZE 2352
#define BIN_DATA_OFFSET 24
#define BIN_DATA_SIZE   2048

typedef struct { u8 minute, second, sector, track; } CdlLOC;

static FILE* s_bin       = NULL;
static int   s_curSector = 0;

static int DecodeBcd(u8 b)  { return (b >> 4) * 10 + (b & 0x0F); }
static u8  EncodeBcd(int i) { return (u8)(((i / 10) << 4) | (i % 10)); }

CdlLOC* CdIntToPos(int i, CdlLOC* p)
{
    i += 150;                              /* LBA -> absolute MSF (2-second lead-in) */
    p->sector = EncodeBcd(i % 75);
    p->second = EncodeBcd((i / 75) % 60);
    p->minute = EncodeBcd((i / 75) / 60);
    return p;
}

static int CdPosToInt(const CdlLOC* p)
{
    return 75 * (60 * DecodeBcd(p->minute) + DecodeBcd(p->second)) + DecodeBcd(p->sector) - 150;
}

/* The XA/FMV raw-sector readers pull 2352-byte sectors one at a time; an
 * unbuffered handle turns each into a filesystem round trip. */
static char s_binStdioBuf[32 * 1024];

void Cd_XboxInit(void)
{
    char path[SH_PS3_PATH_MAX * 2];

    if (s_bin)
        return;

    Sh3Fs_Init();
    if (!Sh3Fs_BinName()[0]) {
        snprintf(g_CdBinPath, sizeof(g_CdBinPath), "NOT FOUND");
        SH_DBG("[CD] no .bin located by fs layer");
        return;
    }

    snprintf(path, sizeof(path), "%s%s", Sh3Fs_DataRoot(), Sh3Fs_BinName());
    s_bin = fopen(path, "rb");
    if (!s_bin) {
        snprintf(g_CdBinPath, sizeof(g_CdBinPath), "OPEN FAILED");
        SH_DBG("[CD] fopen('%s') FAILED", path);
        return;
    }

    setvbuf(s_bin, s_binStdioBuf, _IOFBF, sizeof(s_binStdioBuf));
    snprintf(g_CdBinPath, sizeof(g_CdBinPath), "%s", path);
    SH_DBG("[CD] bin open: '%s'", path);
}

void CdInit(void) { Cd_XboxInit(); }

int CdControl(unsigned char com, unsigned char* param, unsigned char* result)
{
    (void)result;
    if (com == CDL_SETLOC && param)
        s_curSector = CdPosToInt((const CdlLOC*)param);
    return 1;
}

int CdControlB(unsigned char com, unsigned char* param, unsigned char* result)
{
    return CdControl(com, param, result);
}

/* Read `sectors` 2048-byte data sectors from the current position into buf.
 *
 * fsqueue_3.c calls this ONCE with a whole file's sector count and CdReadSync
 * immediately reports done, so the PSX's async sector trickle collapses into one
 * blocking burst inside a single frame. The sectors are contiguous, so seek once
 * and stream them in gulps rather than paying two stdio calls per sector: one
 * fread of up to CD_GULP_SECTORS raw sectors, then memcpy the payloads out (the
 * 304 bytes of sync/header/ECC per sector are what stop us reading straight into
 * dst). */
#define CD_GULP_SECTORS 32
static unsigned char s_gulpBuf[CD_GULP_SECTORS * BIN_SECTOR_SIZE];

int CdRead(int sectors, unsigned long* buf, int mode)
{
    unsigned char* dst = (unsigned char*)buf;
    int done = 0;

    (void)mode;
    if (!s_bin || !buf || sectors <= 0)
        return 1;

    if (fseek(s_bin, (long)s_curSector * BIN_SECTOR_SIZE, SEEK_SET) == 0) {
        while (done < sectors) {
            int    want = sectors - done;
            int    got;
            size_t bytes;
            int    i;

            if (want > CD_GULP_SECTORS)
                want = CD_GULP_SECTORS;

            bytes = fread(s_gulpBuf, 1, (size_t)want * BIN_SECTOR_SIZE, s_bin);
            got   = (int)(bytes / BIN_SECTOR_SIZE);
            if (got <= 0)
                break;                     /* short read / EOF: keep what we have */

            for (i = 0; i < got; i++)
                memcpy(dst + (size_t)(done + i) * BIN_DATA_SIZE,
                       s_gulpBuf + (size_t)i * BIN_SECTOR_SIZE + BIN_DATA_OFFSET,
                       BIN_DATA_SIZE);
            done += got;
        }
    }

    s_curSector += sectors;
    return 1;
}

int CdReadSync(int mode, unsigned char* result) { (void)mode; (void)result; return 0; }            /* 0 = done */
int CdSync(int mode, unsigned char* result)     { (void)mode; (void)result; return CDL_COMPLETE; }
void* CdSearchFile(void) { return 0; }   /* game uses the static file table, not search */

/* Raw sector access for XA streaming. XA audio lives in Mode-2/Form-2 sectors:
 * 2324-byte payloads with the subheader at raw offset 16, which the cooked
 * 2048-byte path above cannot carry. Independent of that path -- s_curSector is
 * untouched and CdRead re-seeks absolutely every call, so interleaving is safe
 * (both run on the main thread). */
int Cd_XboxReadRaw(unsigned int lbn, unsigned char* buf, int sectors)
{
    if (!s_bin || !buf || sectors <= 0)
        return 0;
    if (fseek(s_bin, (long)lbn * BIN_SECTOR_SIZE, SEEK_SET) != 0)
        return 0;
    return fread(buf, BIN_SECTOR_SIZE, (size_t)sectors, s_bin) == (size_t)sectors;
}

FILE* Cd_XboxGetBinFile(void)
{
    Cd_XboxInit();
    return s_bin;
}

/* Read LBA 16, the ISO9660 Primary Volume Descriptor, and hand back its 5-byte
 * standard identifier. Locating and opening the BIN proves very little on its
 * own: a wrong sector size, a bad payload offset, a 2048-byte-per-sector rip or
 * a truncated file all survive fopen and only show up on a read. "CD001" coming
 * back means sector size, payload offset and the BCD position maths are all
 * right at once. Lives here because CdlLOC is deliberately local to this TU.
 * Returns 1 if the BIN is open and a sector was read. */
int Cd_XboxSelfTest(char* idOut, int idOutSize)
{
    static unsigned long sec[BIN_DATA_SIZE / sizeof(unsigned long)];
    CdlLOC pos;

    if (idOut && idOutSize > 0)
        idOut[0] = '\0';
    Cd_XboxInit();
    if (!s_bin)
        return 0;

    CdIntToPos(16, &pos);
    CdControl(CDL_SETLOC, (unsigned char*)&pos, 0);
    CdRead(1, sec, 0);

    if (idOut && idOutSize > 5) {
        memcpy(idOut, (const char*)sec + 1, 5);   /* PVD id sits at offset 1 */
        idOut[5] = '\0';
    }
    return 1;
}
