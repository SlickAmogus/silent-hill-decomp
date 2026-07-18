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
#include <string.h>
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
/* stdio buffer for the BIN handle. The XA/FMV raw-sector readers pull 2352-byte
 * sectors one gulp at a time; an unbuffered handle turns each into a syscall. */
static char s_binStdioBuf[32 * 1024];
static void Cd_SetStreamBuffer(void)
{
    if (s_bin)
        setvbuf(s_bin, s_binStdioBuf, _IOFBF, sizeof(s_binStdioBuf));
}

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
            Cd_SetStreamBuffer();
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
                Cd_SetStreamBuffer();
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

/* Read `sectors` 2048-byte data sectors from the current position into buf.
 *
 * fsqueue_3.c calls this ONCE with a whole file's sector count and CdReadSync
 * immediately reports "done", so the PSX's async sector trickle collapses into
 * one blocking burst inside a single frame — this is the hitch the player feels
 * on room transitions. The old loop made that burst as expensive as possible:
 * an fseek AND an fread per 2048-byte sector, i.e. two stdio calls per sector
 * for a file that can run to thousands of sectors.
 *
 * The sectors are contiguous, so seek ONCE and stream them in gulps: one fread
 * of up to CD_GULP_SECTORS raw 2352-byte sectors, then memcpy the 2048-byte
 * payloads out (the 304 bytes of sync/header/ECC per sector are what stop us
 * reading straight into dst). That is ~32x fewer stdio round trips. */
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
                break;                     /* short read / EOF: stop, keep what we have */

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

/* --- raw sector access (XA streaming, xa_xbox.c) ----------------------------
 * XA audio lives in Mode-2/Form-2 sectors: 2324-byte payloads with the 8-byte
 * subheader (file/channel/submode/codinginfo) at raw offset 16 — the cooked
 * 2048-byte path above cannot carry them. Reads `sectors` full 2352-byte raw
 * sectors starting at LBA `lbn` into buf; returns 1 on success, 0 on no-BIN /
 * short read. Independent of the cooked path: s_curSector is untouched and
 * CdRead re-seeks absolutely on every call, so interleaving is safe (both run
 * on the main thread). */
int Cd_XboxReadRaw(unsigned int lbn, unsigned char* buf, int sectors)
{
    if (!s_bin || !buf || sectors <= 0)
        return 0;
    if (fseek(s_bin, (long)lbn * BIN_SECTOR_SIZE, SEEK_SET) != 0)
        return 0;
    return fread(buf, BIN_SECTOR_SIZE, (size_t)sectors, s_bin) == (size_t)sectors;
}

/* FMV streaming (fmv_xbox.c): str_demux pulls raw sectors through stdio, so it
 * gets the open BIN handle instead of a second fopen. Sharing is safe: every
 * reader here re-seeks absolutely per call, and the game drains the FS queue
 * (Fs_QueueWaitForEmpty in open_main) before FMV playback, so nothing
 * interleaves mid-movie. All main-thread. */
FILE* Cd_XboxGetBinFile(void)
{
    Cd_XboxInit();
    return s_bin;
}
