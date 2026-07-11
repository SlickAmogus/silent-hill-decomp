/*
 * mcard_xbox.c - PSX kernel event ring + memory card HAL for the Xbox port.
 *
 * Port of PsyCross's pc_port/PsyCross/src/psx/libapi.c (read-only reference):
 *   1. the 16-slot PSX event system: OpenEvent(class, spec, mode, func) hands
 *      out 1-based handles, DeliverEvent(class, spec) marks matching events
 *      pending (and fires EvMdINTR callbacks), TestEvent consumes the pending
 *      flag. The game's memcard FSM (src/bodyprog/memcard.c) polls these:
 *      SwCARD/HwCARD with EvSpIOE (done) / EvSpTIMOUT (no card) / EvSpNEW /
 *      EvSpERROR, plus HwCARD EvSpUNKNOWN.
 *   2. the 128KB .MCD-file-backed PSX memory card: BIOS funcs (_card_info /
 *      _card_load / _card_read / _card_write / _card_format ...) + the
 *      "buXX:" device layer (open/close/read/write/lseek/firstfile/nextfile/
 *      erase/format) with on-card McDirEntry bookkeeping.
 *
 * Xbox-native storage: ONE card image, "0.MCD" (131072 bytes), in the
 * dashboard-managed save area E:\UDATA\SH010000\ (TitleMeta.xbx gives the
 * dashboard entry the name "Silent Hill"); falls back to D:\SilentHill\save\
 * next to the XBE. Path resolution + dir creation live in fs_xbox.c
 * (XboxFs_ResolveSaveDir) so this TU never includes <windows.h> — winbase.h
 * #defines OpenEvent -> OpenEventA and would break the PSX symbol.
 *
 * Channel model: PSX channel 0 (port 1) = the one HDD-backed card. Every
 * other channel (port 2 / multitap) reports EvSpTIMOUT = "not connected",
 * exactly like an empty slot on real hardware. Card ops always RETURN 1
 * (command accepted) and signal failure through the events — returning 0
 * would spin the FSM's retry loops forever (MemCard_State_Init case 1 has
 * no retry cap).
 *
 * NOTE the on-card dir-entry name field is 21 bytes: "BASLUS-00707SILENT00"
 * is exactly 20 chars, the NUL lives in byte 21. A 20-byte field truncates
 * and open() never finds the file again (see PsyCross's McDirEntry comment).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psyq/libapi.h>    /* PsyCross libapi decls + kernel.h: SwCARD/HwCARD, EvSp*, EvMd*, DIRENTRY */
#include <psyq/sys/file.h>  /* FCREAT */
#include "sh_log.h"

/* fs_xbox.c: mounts E:, creates E:\UDATA\SH010000 (+TitleMeta.xbx) or the
 * D:\SilentHill\save fallback; returns 0 if no location is writable. */
extern int XboxFs_ResolveSaveDir(char* out, int outSize);

/* ---- rate-limited logging (the FSM background-polls _card_info forever) -- */
static int Mcrd_LogGate(int* counter)
{
    (*counter)++;
    return (*counter <= 16) || ((*counter & 255) == 0);
}

/* ============================================================================
 * PSX event system (ported 1:1 from PsyCross libapi.c lines 24-279, minus the
 * RCnt2 timer registration: on Xbox the MIDI/boot-logo timers are pumped from
 * the game's own VSyncCallback each vblank — see psx_libgpu_xbox.c — so an
 * RCntCNT2 event just occupies a slot and never fires, same as before).
 * ============================================================================
 */
#define MAX_EVENTS 16

typedef struct {
    int used;
    int enabled;
    int pending;            /* set by DeliverEvent, consumed by TestEvent */
    unsigned int ev_class;  /* e.g. SwCARD, HwCARD, RCntCNT2 */
    int ev_spec;
    int ev_mode;
    long (*callback)(void);
} PsxEvent;

static PsxEvent g_events[MAX_EVENTS] = { 0 };

int OpenEvent(unsigned int ev_class, int ev_spec, int ev_mode, long (*func)())
{
    int i;
    for (i = 0; i < MAX_EVENTS; i++) {
        if (!g_events[i].used) {
            g_events[i].used = 1;
            g_events[i].enabled = 0;
            g_events[i].pending = 0;
            g_events[i].ev_class = ev_class;
            g_events[i].ev_spec = ev_spec;
            g_events[i].ev_mode = ev_mode;
            g_events[i].callback = func;
            return i + 1; /* 1-based handle */
        }
    }
    return -1;
}

int CloseEvent(unsigned int event)
{
    int idx = (int)event - 1;
    if (idx >= 0 && idx < MAX_EVENTS) {
        g_events[idx].used = 0;
        g_events[idx].enabled = 0;
        g_events[idx].callback = NULL;
    }
    return 0;
}

int EnableEvent(unsigned int event)
{
    int idx = (int)event - 1;
    if (idx >= 0 && idx < MAX_EVENTS && g_events[idx].used) {
        g_events[idx].enabled = 1;
    }
    return 0;
}

int DisableEvent(unsigned int event)
{
    int idx = (int)event - 1;
    if (idx >= 0 && idx < MAX_EVENTS) {
        g_events[idx].enabled = 0;
    }
    return 0;
}

int WaitEvent(unsigned int event)
{
    (void)event;
    return 0;
}

int TestEvent(unsigned int event)
{
    int idx = (int)event - 1;
    if (idx >= 0 && idx < MAX_EVENTS && g_events[idx].used && g_events[idx].pending) {
        g_events[idx].pending = 0;
        return 1;
    }
    return 0;
}

void DeliverEvent(unsigned int ev1, int ev2)
{
    /* Mark all matching events as pending; if EvMdINTR, also fire callback
     * (the game's HwCARD handlers latch g_MemCard_Work.lastEventHw there). */
    int i;
    for (i = 0; i < MAX_EVENTS; i++) {
        if (!g_events[i].used) continue;
        if (g_events[i].ev_class != ev1) continue;
        if (g_events[i].ev_spec != ev2) continue;
        g_events[i].pending = 1;
        if ((g_events[i].ev_mode & EvMdINTR) && g_events[i].callback) {
            g_events[i].callback();
        }
    }
}

void UnDeliverEvent(unsigned int ev1, int ev2)
{
    int i;
    for (i = 0; i < MAX_EVENTS; i++) {
        if (!g_events[i].used) continue;
        if (g_events[i].ev_class != ev1) continue;
        if (g_events[i].ev_spec != ev2) continue;
        g_events[i].pending = 0;
    }
}

/* ============================================================================
 * PSX memory card backing storage (0.MCD), ported from PsyCross libapi.c
 * lines 567-1109.
 *
 * Layout (PSX standard, 128KB per card):
 *   block 0 (frames 0..63, 8192 bytes) = directory
 *     frame 0       = "MC" header (xor checksum in byte 127)
 *     frames 1..15  = 15 directory entries (128 bytes each)
 *     frames 16..35 = broken-sector list
 *   blocks 1..15 = 15 file data slots (8192 bytes each)
 * ============================================================================
 */
#define MC_FRAME_SIZE       128
#define MC_BLOCK_SIZE       8192
#define MC_NUM_BLOCKS       16
#define MC_TOTAL_SIZE       (MC_NUM_BLOCKS * MC_BLOCK_SIZE)   /* 131072 */
#define MC_DIR_ENTRY_COUNT  15

#define MC_DIR_ATTR_FREE          0xA0
#define MC_DIR_ATTR_FIRST_OR_ONLY 0x50

#pragma pack(push,1)
typedef struct {
    unsigned int   attr;        /* state | block-count */
    unsigned int   size;        /* bytes */
    unsigned short unknown;
    char           name[21];    /* 20 chars + NUL — see file header comment */
    char           padding[97]; /* pad to 128 bytes (4+4+2+21+97) */
} McDirEntry;
#pragma pack(pop)

static char s_saveDir[96]    = "";   /* "E:\UDATA\SH010000" or "D:\SilentHill\save" */
static int  s_cardOk         = 0;    /* a writable save location exists */
static int  s_currentChannel = 0;
static int  s_lastCardOk[2]  = { 0, 0 };

/* log-gate counters for the hot poll paths */
static int s_logCardInfo, s_logCardClear, s_logCardLoad, s_logDirRead;

/* PSX file-handle table for open()/read()/write()/lseek()/close(). */
typedef struct {
    int  used;
    int  channel;
    int  dirIndex;       /* 1..15 */
    int  flags;
    long offset;
    long size;
} McFileHandle;
static McFileHandle s_handles[16] = { 0 };

/* Channel model — PsyCross-EXACT (hardware-proven the hard way): the BIOS card
 * funcs mask every channel with (chan & 1) onto TWO always-connected cards
 * (0.MCD / 1.MCD), and _card_info NEVER reports not-connected. This file's
 * first version reported "chan!=0 -> EvSpTIMOUT" (real-PSX-style empty slots)
 * and the memcard element layer wedged polling channel 18 forever at the
 * KCET-logo check — the game's FSM was only ever validated against PsyCross's
 * all-channels-connected model. TIMOUT stays only for the one Xbox-only
 * degradation PsyCross can't have: no writable save location at all. */

static const char* mc_path_for_channel(int chan)
{
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s\\%d.MCD", s_saveDir, chan);
    return buf;
}

static FILE* mc_fopen(int chan, const char* mode)
{
    return fopen(mc_path_for_channel(chan), mode);
}

/* Build a "fresh card" image: header magic + free dir + 0xFF data. */
static void mc_format_buffer(unsigned char* buf)
{
    int i;
    memset(buf, 0xFF, MC_TOTAL_SIZE);
    /* Frame 0: "MC" magic at offset 0; xor checksum at offset 127. */
    memset(buf, 0, MC_FRAME_SIZE);
    buf[0] = 'M';
    buf[1] = 'C';
    {
        unsigned char xorck = 0;
        for (i = 0; i < MC_FRAME_SIZE - 1; i++) xorck ^= buf[i];
        buf[MC_FRAME_SIZE - 1] = xorck;
    }
    /* Frames 1..15: free directory entries. */
    for (i = 0; i < MC_DIR_ENTRY_COUNT; i++) {
        McDirEntry* d = (McDirEntry*)(buf + (1 + i) * MC_FRAME_SIZE);
        memset(d, 0, sizeof(*d));
        d->attr = MC_DIR_ATTR_FREE;
    }
    /* Frames 16..35: broken-sector list (0xFF, unused). */
    memset(buf + 16 * MC_FRAME_SIZE, 0xFF, 20 * MC_FRAME_SIZE);
}

/* Write a freshly-formatted card image to disk. Returns 1 on success. */
static int mc_write_fresh_card(int chan)
{
    unsigned char* fresh;
    FILE*          f;
    size_t         n;

    fresh = (unsigned char*)malloc(MC_TOTAL_SIZE);
    if (!fresh) return 0;
    mc_format_buffer(fresh);

    f = mc_fopen(chan, "wb");
    if (!f) { free(fresh); return 0; }
    n = fwrite(fresh, 1, MC_TOTAL_SIZE, f);
    fflush(f);
    fclose(f);
    free(fresh);

    if (n != MC_TOTAL_SIZE) {
        SH_DBG("[MCRD] format WRITE FAILED at %s (wrote %d of %d)",
               mc_path_for_channel(chan), (int)n, MC_TOTAL_SIZE);
        return 0;
    }
    SH_DBG("[MCRD] formatted new card at %s", mc_path_for_channel(chan));
    return 1;
}

/* Ensure the card file exists with the right size; (re)format lazily if not. */
static int mc_ensure_card(int chan)
{
    FILE* f;
    long  sz;

    if (!s_cardOk) return 0;

    f = mc_fopen(chan, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
        if (sz == MC_TOTAL_SIZE) return 1;
        SH_DBG("[MCRD] card at %s has wrong size %ld (want %d); reformatting",
               mc_path_for_channel(chan), sz, MC_TOTAL_SIZE);
    }
    return mc_write_fresh_card(chan);
}

/* Read N bytes at byte offset; returns 1 on success. */
static int mc_read_at(int chan, long ofs, void* buf, int bytes)
{
    FILE*  f;
    size_t n;

    f = mc_fopen(chan, "rb");
    if (!f) return 0;
    if (fseek(f, ofs, SEEK_SET) != 0) { fclose(f); return 0; }
    n = fread(buf, 1, bytes, f);
    fclose(f);
    return (int)n == bytes;
}

/* Write N bytes at byte offset; returns 1 on success. */
static int mc_write_at(int chan, long ofs, const void* buf, int bytes)
{
    FILE*  f;
    size_t n;

    f = mc_fopen(chan, "rb+");
    if (!f) {
        if (!mc_ensure_card(chan)) return 0;   /* card missing — auto-create */
        f = mc_fopen(chan, "rb+");
        if (!f) return 0;
    }
    if (fseek(f, ofs, SEEK_SET) != 0) { fclose(f); return 0; }
    n = fwrite(buf, 1, bytes, f);
    fflush(f);
    fclose(f);
    return (int)n == bytes;
}

/* Find a directory entry by name. Returns 1..15 if found, 0 otherwise. */
static int mc_find_dir(int chan, const char* name, McDirEntry* outEntry)
{
    int i;
    for (i = 1; i <= MC_DIR_ENTRY_COUNT; i++) {
        McDirEntry e;
        if (!mc_read_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
        if ((e.attr & 0xF0) == MC_DIR_ATTR_FREE) continue;
        if (e.name[0] == '\0') continue;
        if (strncmp(e.name, name, sizeof(e.name)) == 0) {
            if (outEntry) *outEntry = e;
            return i;
        }
    }
    return 0;
}

/* Allocate a dir entry (data lives in block == dir index) for a new file. */
static int mc_alloc_dir(int chan, const char* name, int blocks)
{
    int i;
    for (i = 1; i <= MC_DIR_ENTRY_COUNT; i++) {
        McDirEntry e;
        if (!mc_read_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
        if ((e.attr & 0xF0) == MC_DIR_ATTR_FREE) {
            memset(&e, 0, sizeof(e));
            e.attr = MC_DIR_ATTR_FIRST_OR_ONLY | (blocks & 0x7);
            e.size = blocks * MC_BLOCK_SIZE;
            strncpy(e.name, name, sizeof(e.name) - 1);
            if (!mc_write_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
            return i;
        }
    }
    return 0;
}

/* Strip "buXX:" prefix if present, return remaining name. PsyCross arithmetic
 * VERBATIM: chan = X1*8 + X2 ("bu10:" -> 8 -> its own card file, exactly like
 * the PC port) — the game's FSM was only ever validated against this. */
static const char* mc_strip_prefix(const char* path, int* outChan)
{
    if (!path) return NULL;
    if (path[0] == 'b' && path[1] == 'u' && path[3] != '\0' && path[4] == ':') {
        if (outChan) *outChan = (path[2] - '0') * 8 + (path[3] - '0');
        return path + 5;
    }
    if (outChan) *outChan = 0;
    return path;
}

/* Deliver SwCARD + HwCARD EvSpIOE so the game's state machine advances. */
static void mc_deliver_iod(void)
{
    DeliverEvent(SwCARD, EvSpIOE);
    DeliverEvent(HwCARD, EvSpIOE);
}

/* No card in that slot / no writable storage: EvSpTIMOUT = "not connected".
 * The FSM turns this into MemCardResult_NotConnected -> "no memory card". */
static void mc_deliver_timeout(void)
{
    DeliverEvent(SwCARD, EvSpTIMOUT);
    DeliverEvent(HwCARD, EvSpTIMOUT);
}

/* ----- Xbox init (called once from main_xbox.c) ----- */

void Mcard_XboxInit(void)
{
    s_cardOk = XboxFs_ResolveSaveDir(s_saveDir, sizeof(s_saveDir));
    if (!s_cardOk) {
        /* Never hang the FSM: with no storage every op delivers EvSpTIMOUT
         * and the game just shows "no memory card". */
        SH_DBG("[MCRD] init: NO writable save location; card reports not-connected");
        return;
    }
    SH_DBG("[MCRD] card path = %s\\0.MCD", s_saveDir);
    mc_ensure_card(0);
}

/* ----- BIOS-level memcard funcs ----- */

void InitCARD(int val)
{
    (void)val;
    mc_ensure_card(0);
}

int StartCARD(void)
{
    return 1;
}

int StopCARD(void)
{
    return 1;
}

void _bu_init(void)
{
    mc_ensure_card(0);
}

int _card_info(int chan)
{
    if (!s_cardOk) {
        mc_deliver_timeout();
        if (Mcrd_LogGate(&s_logCardInfo))
            SH_DBG("[MCRD] card_info chan=%d -> TIMOUT (no storage) (n=%d)", chan, s_logCardInfo);
        return 1;
    }
    mc_ensure_card(chan & 1);
    s_lastCardOk[chan & 1] = 1;
    mc_deliver_iod();
    if (Mcrd_LogGate(&s_logCardInfo))
        SH_DBG("[MCRD] card_info chan=%d -> IOE (n=%d)", chan, s_logCardInfo);
    return 1;
}

int _card_clear(int chan)
{
    if (!s_cardOk) {
        mc_deliver_timeout();
        return 1;
    }
    mc_deliver_iod();
    if (Mcrd_LogGate(&s_logCardClear))
        SH_DBG("[MCRD] card_clear chan=%d -> IOE (n=%d)", chan, s_logCardClear);
    return 1;
}

int _card_load(int chan)
{
    s_currentChannel = chan & 1;
    if (!s_cardOk) {
        /* No storage: TIMOUT resolves the FSM to NotConnected (its State_Load
         * retry cap handles it); PsyCross would return 0 forever here. */
        mc_deliver_timeout();
        return 1;
    }
    if (!mc_ensure_card(s_currentChannel))
        return 0;                     /* PsyCross-exact: rejected, no event */
    s_lastCardOk[s_currentChannel] = 1;
    mc_deliver_iod();
    if (Mcrd_LogGate(&s_logCardLoad))
        SH_DBG("[MCRD] card_load chan=%d -> IOE (n=%d)", chan, s_logCardLoad);
    return 1;
}

int _card_auto(int val)
{
    (void)val;
    return 1;
}

void _new_card(void)
{
    /* PSX BIOS: marks card as "newly inserted" — clear cached state. */
    s_lastCardOk[0] = 0;
    s_lastCardOk[1] = 0;
}

int _card_status(int drv)
{
    return s_lastCardOk[drv & 1] ? 1 : 0;
}

int _card_wait(int drv)
{
    (void)drv;
    return 1;  /* sync model — operation already complete */
}

unsigned int _card_chan(void)
{
    return (unsigned int)s_currentChannel;
}

int _card_write(int chan, int frameIdx, unsigned char* buf)
{
    int c = chan & 1;
    if (!mc_ensure_card(c) ||
        !mc_write_at(c, (long)frameIdx * MC_FRAME_SIZE, buf, MC_FRAME_SIZE))
        return 0;                     /* PsyCross-exact: no event on failure
                                       * (State_FileReadWrite caps at 15 retries) */
    mc_deliver_iod();
    return 1;
}

int _card_read(int chan, int frameIdx, unsigned char* buf)
{
    int c = chan & 1;
    if (!mc_ensure_card(c) ||
        !mc_read_at(c, (long)frameIdx * MC_FRAME_SIZE, buf, MC_FRAME_SIZE))
        return 0;                     /* PsyCross-exact */
    mc_deliver_iod();
    return 1;
}

int _card_format(int chan)
{
    int c = chan & 1;
    if (!s_cardOk || !mc_write_fresh_card(c))
        return 0;                     /* PsyCross-exact */
    mc_deliver_iod();
    return 1;
}

/* ----- PSX-style file ops on "buXX:NAME" paths ----- */

int open(char* path, unsigned int flags)
{
    int         chan = 0;
    const char* name;
    int         blockCount;
    int         isCreate;
    int         dirIdx;
    int         h;

    if (!path) return -1;
    name = mc_strip_prefix(path, &chan);
    if (name == path) {
        /* Not a memcard path (e.g. fsqueue_3.c's "sim:" PCdrv reads).
         * Report failure so callers fall back, exactly as before. */
        return -1;
    }
    if (!mc_ensure_card(chan)) return -1;

    blockCount = (int)((flags >> 16) & 0xFFFF);
    isCreate   = (flags & FCREAT) != 0;

    dirIdx = mc_find_dir(chan, name, NULL);
    if (dirIdx == 0) {
        if (!isCreate) {
            SH_DBG("[MCRD] open '%s' (flags=0x%x): not found", path, flags);
            return -1;
        }
        if (blockCount <= 0 || blockCount > 15) blockCount = 1;
        dirIdx = mc_alloc_dir(chan, name, blockCount);
        if (dirIdx == 0) {
            SH_DBG("[MCRD] open '%s': CREATE FAILED (card full?)", path);
            return -1;
        }
        SH_DBG("[MCRD] open '%s': created, dir=%d blocks=%d", path, dirIdx, blockCount);
    }

    for (h = 1; h < (int)(sizeof(s_handles) / sizeof(s_handles[0])); h++) {
        if (!s_handles[h].used) {
            McDirEntry e;
            if (!mc_read_at(chan, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return -1;
            s_handles[h].used = 1;
            s_handles[h].channel = chan;
            s_handles[h].dirIndex = dirIdx;
            s_handles[h].flags = (int)flags;
            s_handles[h].offset = 0;
            s_handles[h].size = (long)e.size;
            if (s_handles[h].size <= 0) s_handles[h].size = MC_BLOCK_SIZE;
            SH_DBG("[MCRD] open '%s' flags=0x%x -> h=%d dir=%d size=%d",
                   path, flags, h, dirIdx, (int)s_handles[h].size);
            return h;
        }
    }
    return -1;
}

int close(int handle)
{
    if (handle < 1 || handle >= (int)(sizeof(s_handles) / sizeof(s_handles[0]))) return -1;
    if (!s_handles[handle].used) return -1;
    s_handles[handle].used = 0;
    return 0;
}

int lseek(int handle, int offset, int whence)
{
    long base;
    long p;

    if (handle < 1 || handle >= (int)(sizeof(s_handles) / sizeof(s_handles[0]))) return -1;
    if (!s_handles[handle].used) return -1;

    if (whence == 1)      base = s_handles[handle].offset;
    else if (whence == 2) base = s_handles[handle].size;
    else                  base = 0;

    p = base + offset;
    if (p < 0) return -1;
    s_handles[handle].offset = p;
    return (int)p;
}

int read(int handle, void* buf, int bytes)
{
    int  chan;
    long ofs;

    if (handle < 1 || handle >= (int)(sizeof(s_handles) / sizeof(s_handles[0]))) return -1;
    if (!s_handles[handle].used) return -1;

    chan = s_handles[handle].channel;
    /* file data lives in data block == dirIndex (blocks dirIndex..dirIndex+N-1) */
    ofs = (long)s_handles[handle].dirIndex * MC_BLOCK_SIZE + s_handles[handle].offset;
    if (!mc_read_at(chan, ofs, buf, bytes)) {
        SH_DBG("[MCRD] read h=%d ofs=%d len=%d FAILED", handle, (int)ofs, bytes);
        return -1;
    }
    SH_DBG("[MCRD] read h=%d dir=%d ofs=%d len=%d ok",
           handle, s_handles[handle].dirIndex, (int)s_handles[handle].offset, bytes);
    s_handles[handle].offset += bytes;
    mc_deliver_iod();
    return bytes;
}

int write(int handle, void* buf, int bytes)
{
    int  chan;
    long ofs;

    if (handle < 1 || handle >= (int)(sizeof(s_handles) / sizeof(s_handles[0]))) return -1;
    if (!s_handles[handle].used) return -1;

    chan = s_handles[handle].channel;
    ofs = (long)s_handles[handle].dirIndex * MC_BLOCK_SIZE + s_handles[handle].offset;
    if (!mc_write_at(chan, ofs, buf, bytes)) {
        SH_DBG("[MCRD] write h=%d ofs=%d len=%d FAILED", handle, (int)ofs, bytes);
        return -1;
    }
    SH_DBG("[MCRD] write h=%d dir=%d ofs=%d len=%d ok",
           handle, s_handles[handle].dirIndex, (int)s_handles[handle].offset, bytes);
    s_handles[handle].offset += bytes;
    mc_deliver_iod();
    return bytes;
}

int ioctl(int fd, int req, int arg)
{
    (void)fd; (void)req; (void)arg;
    return 0;
}

/* ----- Directory enumeration ----- */

struct DIRENTRY* nextfile(struct DIRENTRY* dir);

/* Cursor for firstfile/nextfile is embedded in DIRENTRY's system[] bytes. */
struct DIRENTRY* firstfile(char* path, struct DIRENTRY* dir)
{
    int         chan = 0;
    const char* pattern;

    if (!path || !dir) return NULL;
    pattern = mc_strip_prefix(path, &chan);
    (void)pattern;  /* the game only ever passes "*" */
    if (!mc_ensure_card(chan)) return NULL;
    memset(dir, 0, sizeof(*dir));
    dir->system[0] = (char)chan;
    dir->system[1] = 0;  /* next dirIdx to scan */
    if (Mcrd_LogGate(&s_logDirRead))
        SH_DBG("[MCRD] dir scan chan=%d (n=%d)", chan, s_logDirRead);
    return nextfile(dir);
}

struct DIRENTRY* nextfile(struct DIRENTRY* dir)
{
    int chan;
    int start;
    int i;

    if (!dir) return NULL;
    chan  = (int)(unsigned char)dir->system[0];
    start = (int)(unsigned char)dir->system[1] + 1;
    for (i = (start < 1 ? 1 : start); i <= MC_DIR_ENTRY_COUNT; i++) {
        McDirEntry e;
        if (!mc_read_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return NULL;
        if ((e.attr & 0xF0) == MC_DIR_ATTR_FREE) continue;
        if (e.name[0] == '\0') continue;
        dir->system[1] = (char)i;
        /* Full 20-char name + NUL: DIRENTRY.name is 21 bytes (PsyCross
         * kernel.h — the shared game code compiles against it too). */
        memcpy(dir->name, e.name, 20);
        dir->name[20] = '\0';
        dir->attr = e.attr;
        dir->size = e.size;
        dir->head = 0;
        dir->next = NULL;
        return dir;
    }
    return NULL;
}

int erase(char* path)
{
    int         chan = 0;
    const char* name;
    int         dirIdx;
    McDirEntry  e;

    if (!path) return 0;
    name = mc_strip_prefix(path, &chan);
    dirIdx = mc_find_dir(chan, name, NULL);
    if (dirIdx == 0) return 0;
    memset(&e, 0, sizeof(e));
    e.attr = MC_DIR_ATTR_FREE;
    if (!mc_write_at(chan, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
    SH_DBG("[MCRD] delete '%s' (dir=%d)", path, dirIdx);
    return 1;
}

int undelete(char* path)
{
    (void)path;
    return 0;
}

int format(char* path)
{
    int chan = 0;
    if (path) mc_strip_prefix(path, &chan);
    SH_DBG("[MCRD] format chan=%d", chan);
    return _card_format(chan);
}

/* const char* matches the pdclib <stdio.h> `rename` declaration (included
 * above for fopen); the PSX decl is commented out in the headers, same
 * situation PsyCross handles this way. */
int rename(const char* oldpath, const char* newpath)
{
    int         chanA = 0, chanB = 0;
    const char* oldname;
    const char* newname;
    int         dirIdx;
    McDirEntry  e;

    if (!oldpath || !newpath) return 0;
    oldname = mc_strip_prefix(oldpath, &chanA);
    newname = mc_strip_prefix(newpath, &chanB);
    if (chanA != chanB) return 0;
    dirIdx = mc_find_dir(chanA, oldname, NULL);
    if (dirIdx == 0) return 0;
    if (!mc_read_at(chanA, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
    memset(e.name, 0, sizeof(e.name));
    strncpy(e.name, newname, sizeof(e.name) - 1);
    if (!mc_write_at(chanA, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
    SH_DBG("[MCRD] rename '%s' -> '%s' (dir=%d)", oldpath, newpath, dirIdx);
    return 1;
}
