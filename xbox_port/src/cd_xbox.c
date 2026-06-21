/*
 * cd_xbox.c - PSX CD-ROM (libcd) on Xbox, backed by the game's BIN disc image on
 * the HDD (next to the XBE, on D:).
 *
 * Implements the synchronous slice of libcd the FS queue uses: CdControl(CdlSetloc)
 * sets the read sector, CdRead copies the 2048-byte user data out of each
 * 2352-byte Mode-2/Form-1 sector (data at offset 24: 12 sync + 3 addr + 1 mode +
 * 8 subheader). The PSX async model collapses to synchronous reads here, so
 * CdReadSync/CdSync report "complete" immediately.
 *
 * Self-contained (local CdlLOC + Cdl* constants) so it can use <windows.h> for
 * FindFirstFile without pulling in <libcd.h>/decomp type clashes. C links by name,
 * and CdlLOC is the standard {minute,second,sector,track} BCD layout, so this
 * matches the game's libcd prototypes at the ABI level.
 */
#include <windows.h>
#include <stdio.h>
#include "sh_log.h"

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

/* Open the game's BIN disc image, which lives next to the XBE on D: (the launch
 * dir; same place the log is written via fopen). Idempotent. Tries the expected
 * filenames by direct fopen first — FindFirstFile is unreliable on the nxdk D:
 * mount — then falls back to scanning D:\*.bin. */
void Cd_XboxInit(void)
{
    /* Check both layouts: next to the XBE (D: root) and in a gamedata\ subfolder
     * (matching the PC port's ./gamedata convention, so a copied PC setup works). */
    static const char* const names[] = {
        "D:\\Silent Hill (USA).bin",
        "D:\\gamedata\\Silent Hill (USA).bin",
        "D:\\Silent Hill (USA) (Track 1).bin",
        "D:\\gamedata\\Silent Hill (USA) (Track 1).bin",
        "D:\\Silent Hill (USA) (Track 01).bin",
        "D:\\gamedata\\Silent Hill (USA) (Track 01).bin",
        "D:\\Silent Hill.bin",
        "D:\\gamedata\\Silent Hill.bin",
    };
    static const char* const scanDirs[] = { "D:\\", "D:\\gamedata\\" };
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    unsigned         i;

    if (s_bin)
        return;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        s_bin = fopen(names[i], "rb");
        if (s_bin) {
            SH_DBG("[CD] disc image: %s", names[i]);
            return;
        }
        SH_DBG("[CD] not found: %s", names[i]);
    }

    /* Fallback: any *.bin in either directory. */
    for (i = 0; i < sizeof(scanDirs) / sizeof(scanDirs[0]); i++) {
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof(pattern), "%s*.bin", scanDirs[i]);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s%s", scanDirs[i], fd.cFileName);
            FindClose(h);
            s_bin = fopen(path, "rb");
            if (s_bin) {
                SH_DBG("[CD] disc image (scan): %s", path);
                return;
            }
        }
    }
    SH_DBG("[CD] NO .bin found on D:\\ or D:\\gamedata\\ — see filenames tried above");
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

/* Read `sectors` 2048-byte data sectors from the current position into buf. */
int CdRead(int sectors, unsigned long* buf, int mode)
{
    int i;
    unsigned char* dst = (unsigned char*)buf;

    (void)mode;
    if (!s_bin || !buf || sectors <= 0)
        return 1;

    for (i = 0; i < sectors; i++) {
        long off = (long)(s_curSector + i) * BIN_SECTOR_SIZE + BIN_DATA_OFFSET;
        if (fseek(s_bin, off, SEEK_SET) != 0)
            break;
        if (fread(dst + (size_t)i * BIN_DATA_SIZE, 1, BIN_DATA_SIZE, s_bin) != BIN_DATA_SIZE)
            break;
    }
    s_curSector += sectors;
    return 1;
}

int CdReadSync(int mode, unsigned char* result) { (void)mode; (void)result; return 0; }            /* 0 = done */
int CdSync(int mode, unsigned char* result)     { (void)mode; (void)result; return CDL_COMPLETE; }
void* CdSearchFile(void) { return 0; }   /* game uses the static file table, not search */
