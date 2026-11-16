/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The wait between a voice line being asked for and being heard, as the
 * PlayStation had it.
 *
 * On the console every sound job is a task in one queue, and
 * Sd_TaskPoolExecute runs one state of the task at its head per game frame.
 * A line is two or three of them:
 *   Sd_XaAudioStop  fade the last line out 24 a frame, mute, CdlPause, clean up;
 *   Sd_XaAudioStop  again when the request came from a page turn: SD_Call(19)
 *                   queues one, the play request queues another, and the pool
 *                   never merges an entry with the one at its head;
 *   Sd_XaAudioPlay  set up, CdlSetmode, filter, CdlSetfilter, sector, CdlSeekL,
 *                   CdlReadN once the seek has completed, switch the CD input on.
 * and it waits its turn behind whatever else is queued. A change of music is
 * Sd_StopBgm (fade the track 4 a frame), Sd_VabLoad (the song's sound bank,
 * read 25 sectors at a time) and Sd_KdtLoad (its sequence), two to three
 * seconds of frames and reads in front of the line.
 *
 * Sd_AudioStreamingCheck() reports 4 from the request until CdlReadN is sent,
 * and a voiced page (Gfx_MapMsg_Draw) draws nothing and does not run its timer
 * until that has come and gone. Subtitle and voice therefore arrive together,
 * and every scene's message timers were authored on top of the wait.
 *
 * The wait is a count of frames more than anything the drive does: sixteen
 * for a page turn, of which the seek is one or two. So its length is the
 * console's frame time, and that was the scene's own. Outside gameplay the
 * frame loop waits for one vblank after the frame's work, whatever that took;
 * in DuckStation the cafe scene runs at four vblanks a frame (15 fps) with
 * both characters in shot and three with one, a second or more per line.
 * ConsoleFrameMs() estimates it from how much the frame draws.
 *
 * The PC port runs those tasks in a frame or a few and its player starts at
 * once, so this file replays the console's side on a shadow of the pool and
 * the task states, stepped at the console's frame time, with a model of the
 * drive answering the commands. It is fed the same requests as the real pool
 * and gives back two things: whether the page is still held
 * (Pc_XaSeek_PageGate) and how long the player holds the line's first sound
 * (Pc_XaSeek_LineDelayMs). Dialog Pause (cutscene_line_gap_ms) scales the
 * whole sequence: 300 is the console, 0 switches it off.
 *
 * Drive timings are DuckStation's, which were measured against hardware: a
 * command is acknowledged after about 0.74 ms; a pause from reading takes one
 * turn of the disc back to the hold point; a jump that leaves the sled where
 * it is takes about 50 ms below a distance that shrinks from ~330 sectors at
 * the start of the disc to ~60 at its end, and about 100 ms above it; a sled
 * move takes 0.3 s to 0.9 s; the first sector after CdlReadN waits for the
 * hold point to come round. They are written here from those figures, and the
 * task states were checked frame for frame against a trace of the game
 * running in it. */
#include "xa_player.h"
#include "pc_config.h"
#include "sh_log.h"
#include "common.h"
#include "game.h"
#include "main/fileinfo.h"
#include "bodyprog/sound/sound_system.h"
#include <SDL_timer.h>
#include <math.h>
#include <string.h>

extern int      g_PsxCdHeadLba;
extern unsigned g_PsxCdHeadEpoch;
extern int      g_PcFastForward;
extern int      g_PcFramePacketBytes;
extern int      GetVideoMode(void);

#define XS_POOL          32
#define XS_XA_COUNT      727
#define XS_AUDIO_COUNT   127
#define XS_NORMAL_MS     300.0            /* cutscene_line_gap_ms of the "Normal" preset */
#define XS_SECTOR_MS     (1000.0 / 150.0) /* CdlModeSpeed: voices and data both read at double speed */
#define XS_ACK_MS        0.738
#define XS_PAUSE_IDLE_MS 0.218
#define XS_PAUSE_MIN_MS  29.5
#define XS_JITTER_MS     0.8
#define XS_SLED_SECTORS  7200
#define XS_DISC_SECTORS  (72.0 * 60.0 * 75.0)
#define XS_PI            3.14159265358979
#define XS_VAB_CHUNK     0xC800           /* VAB_BUFFER_LIMIT: a bank is read and sent to the SPU in pieces this size */

/* The console's frame time from the frame's primitives.
 *
 * Work per frame against the bytes of primitives the game had written when
 * Sd_TaskPoolExecute ran, both read out of DuckStation: in gameplay, where a
 * frame over two vblanks is not rounded, 39.7 ms at 56 KB, 46.5 ms at 61 KB
 * and 49.8 ms at 80 KB; in the cafe scene, three vblanks a frame below about
 * 69 KB, four above, still four at 111 KB. One slope fits both, with the
 * scene 3.7 ms dearer than gameplay at the same load. A frame that draws no
 * world (a prompt over a frozen picture) runs in one vblank.
 *
 * The port's count for the same frame runs 1.3 to 1.9 times the console's:
 * its primitives carry a 12-byte tag where the console's is 4, and its mesh
 * drawers keep polygons out to a wider bound. Scaled by this, the cafe
 * scene's two-shots come out at four vblanks and its one-character shots at
 * three, as in DuckStation; the window's aspect does not change the count. */
#define XS_PSX_PER_PC_BYTE  0.56
#define XS_WORK_MS_PER_BYTE 0.00036
#define XS_WORK_SCENE_MS    25.3
#define XS_WORK_PLAY_MS     21.6
#define XS_WORK_IDLE_MS     6.0
#define XS_WORLD_BYTES      8000.0

enum { XS_STOP_FADE, XS_STOP_MUTE, XS_STOP_PAUSE, XS_STOP_CLEANUP };
/* e_AudioLoadState, shared by the bank and sequence loads. */
enum
{
    XS_LOAD_RESET,
    XS_LOAD_STOP,
    XS_LOAD_SETOFF,
    XS_LOAD_FILE,
    XS_LOAD_CHECK,
    XS_LOAD_MOVE,
    XS_LOAD_SETNEXT,
    XS_LOAD_MOVENEXT,
    XS_LOAD_MOVELAST,
    XS_LOAD_FINALIZE
};
enum
{
    XS_PLAY_INIT,
    XS_PLAY_SETMODE,
    XS_PLAY_FILTER,
    XS_PLAY_SETFILTER,
    XS_PLAY_LBA,
    XS_PLAY_SEEK,
    XS_PLAY_READ,
    XS_PLAY_ENABLE
};

enum { XS_REQ_PLAY, XS_REQ_STOP, XS_REQ_PRELOAD, XS_REQ_STOP_ACTIVE, XS_REQ_TASK };

typedef struct
{
    int      type;
    int      arg;        /* the clip, or the task */
    int      bgmVol;
    int      lastVab[3];
    unsigned frame;      /* the game frame that asked */
} XaRequest;

#define XS_QUEUE 16

typedef struct
{
    u8  pool[XS_POOL]; /* g_Sd_TaskPool */
    int stopState;
    int playState;
    int preState;
    int loadState;     /* g_Sd_AudioStreamingStates.audioLoadState_0 */
    int loading;       /* g_Sd_AudioWork.isAudioLoading_15 */
    int loadLba;
    int loadSize;
    int loadMoved;     /* g_Sd_DataMoved */
    int lastVab[3];    /* g_Sd_AudioWork.lastVabAudioLoadedIdx_8 */
    int bgmVol;        /* gSDVolConfig.volumeBgm_8 */
    int volXa;         /* gSDVolConfig.volumeXa_0 */
    int lineVol;       /* what the play task's first state will set it to */
    int idxCheck;      /* g_Sd_AudioWork.xaAudioIdxCheck_2 */
    int idx4;          /* g_Sd_AudioWork.xaAudioIdx_4 */
    int playIdx;
    int preIdx;
    int startPending;  /* D_800C37DC */
    int notPlaying;    /* g_Sd_AudioWork.isXaNotPlaying_16 */
    double wdStartMs;  /* D_800C1688.field_8, as a time */
    int    wdLen;      /* D_800C1688.field_0 */
    int    wdElapsed;  /* D_800C1688.field_4 */

    double   busyUntilMs; /* CdSync reports the last command complete from here */
    double   seekDoneMs;
    double   dataDoneMs;  /* CdReadSync reports the read in flight done from here */
    int      reading;
    double   readStartMs;
    int      readStartLba;
    int      holdLba;
    double   holdSinceMs;
    int      holdPhase;
    unsigned epoch;

    int    gridOn;
    int    gridPending;
    double nextTickMs;
    double lineFrameMs; /* the frame time the line on its way was planned with */

    unsigned reqSeq;   /* play requests so far */
    unsigned playSeq;  /* the request the running play task serves */
    unsigned readSeq;  /* the last request whose CdlReadN went out, or was withdrawn */
    unsigned heardSeq; /* the last request whose CD input was switched on */
    double   reqMs;
    double   readNMs;
    double   audioMs;

    XaRequest queue[XS_QUEUE]; /* requests of later game frames, one frame's worth let in per pass */
    int       queued;
    unsigned  batchFrame;      /* the game frame whose requests went in last */
    int       ticked;          /* a pass has run since they did */
    unsigned  askSeq;          /* play requests made: in the pool, or waiting here */

    int    logHead;
    int    logTarget;
    int    logStops;
    int    logFades;
    double logSeekMs;
} XaShadow;

/* No page waits longer than this (times the Dialog Pause scale) for its
 * line, whatever the shadow believes: the longest real case, a line behind
 * a change of music with a large bank, is about five seconds. */
#define XS_PAGE_LIMIT_MS 8000.0

static XaShadow s_live;
static int      s_ready       = 0;
static int      s_paused      = 0;
static double   s_pauseStart  = 0.0;
static double   s_lastNow     = 0.0;
static Uint32   s_lineDelayMs = 0;
static double   s_loadBytes   = -1.0;
static unsigned s_gameFrame   = 0;

static double NowMs(void)
{
    return (double)SDL_GetTicks();
}

static double Scale(void)
{
    const int gap = g_PcConfig.cutsceneLineGapMs;

    return gap > 0 ? gap / XS_NORMAL_MS : 0.0;
}

/* 263 lines of 3413 GPU clocks at 53.693175 MHz, and PAL's 314 of 3406 at
 * 53.203425 MHz: the game's modes are not interlaced. */
static double VblankHz(void)
{
    return GetVideoMode() == 1 ? 49.761 : 59.826;
}

static double ConsoleFrameMs(void)
{
    const double vbl   = 1000.0 / VblankHz();
    const double bytes = s_loadBytes > 0.0 ? s_loadBytes : 0.0;
    const int    play  = (g_SysWork.sysState == SysState_Gameplay);
    double       work  = bytes * XS_WORK_MS_PER_BYTE;
    double       frame;

    if (bytes < XS_WORLD_BYTES)
        work += XS_WORK_IDLE_MS;
    else
        work += play ? XS_WORK_PLAY_MS : XS_WORK_SCENE_MS;

    /* The frame loop's two branches: gameplay waits until two vblanks have
     * gone by and no longer; everything else waits for the next vblank. */
    if (play)
        frame = (work < 2.0 * vbl) ? 2.0 * vbl : work;
    else
        frame = (floor(work / vbl) + 1.0) * vbl;

    /* Four vblanks is the slowest frame the game was built to take (its
     * delta time stops there), and the port can draw far more than the
     * console ever did: wider views, longer draw distances, denser models. */
    return (frame > 4.0 * vbl) ? 4.0 * vbl : frame;
}

static long long Vblanks(double ms)
{
    return (long long)(ms * VblankHz() / 1000.0);
}

static int XaValid(int idx)
{
    return idx >= 0 && idx < XS_XA_COUNT && g_XaItemData[idx].xaFileIdx_0 != 0;
}

static int XaTarget(int idx)
{
    if (!XaValid(idx))
        return 0;
    return (int)g_FileXaLoc[g_XaItemData[idx].xaFileIdx_0] + (int)g_XaItemData[idx].sector_4;
}

static int XaLength(int idx)
{
    return XaValid(idx) ? (int)g_XaItemData[idx].audioLength_8 : 0;
}

/* Constant linear velocity, tracks 1.6 um apart. The speed and the first
 * track's radius are the pair that puts DuckStation's turns where its pause
 * times show them: 8 sectors at the start of the disc, 9 at LBA 21000, 10
 * from 22900 to 25400, 11 from 39400. */
static int SectorsPerTurn(int lba)
{
    const double seconds = ((lba > 0 ? lba : 0) + 150) / 75.0;
    const double radius  = sqrt(0.02167 * 0.02167 + 1.2 * 1.6e-6 * seconds / XS_PI);
    const int    n       = (int)(2.0 * XS_PI * radius * 75.0 / 1.2);

    return n < 8 ? 8 : n;
}

static double SeekMs(int from, int to)
{
    const int spt  = SectorsPerTurn(from);
    const int diff = (to > from) ? to - from : from - to;
    double    ms;

    if (from < to && diff <= spt)
    {
        /* Just ahead on this turn: the drive reads its way there. */
        ms = XS_SECTOR_MS * (diff < 2 ? 2 : diff);
    }
    else if (from >= to && from - spt <= to)
    {
        /* Within a turn behind: one track back, then forward to it. */
        const int fwd = to - (from - spt);

        ms = XS_SECTOR_MS * (fwd < 1 ? 1 : fwd);
    }
    else if (diff < XS_SLED_SECTORS)
    {
        double minute = (from + 150) / 4500.0;
        double fine;

        if (minute < 1.0)  minute = 1.0;
        if (minute > 72.0) minute = 72.0;
        fine = 330.0 - (270.0 / log(72.0)) * log(minute);
        ms   = (diff < fine) ? 50.0 : 100.0;
    }
    else
    {
        ms = 1000.0 * (0.05 + 0.34 * log((double)diff) / log(XS_DISC_SECTORS) + 0.51 * diff / XS_DISC_SECTORS);
    }
    return ms + XS_JITTER_MS;
}

static void PoolAdd(XaShadow* s, int task)
{
    int i, y;

    /* Sd_TaskPoolAdd, index for index: the entry at the head is never
     * looked at, which is what leaves two stop tasks in a row. */
    if (task == 2)
    {
        for (i = 1; i < XS_POOL - 2; i++)
        {
            if (s->pool[i] == 1)
            {
                for (y = i; y < XS_POOL - 2; y++)
                    s->pool[y] = s->pool[y + 1];
                s->pool[XS_POOL - 1] = 0;
                s->startPending      = 0;
                s->readSeq           = s->reqSeq;
            }
        }
    }

    for (i = 1; i < XS_POOL - 2; i++)
    {
        if (s->pool[i] == task)
        {
            for (y = i; y < XS_POOL - 2; y++)
                s->pool[y] = s->pool[y + 1];
            s->pool[XS_POOL - 1] = 0;
        }
    }

    for (i = 0; i < XS_POOL - 1; i++)
    {
        if (s->pool[i] == 0)
        {
            s->pool[i] = (u8)task;
            break;
        }
    }
}

static void PoolUpdate(XaShadow* s)
{
    if (s->pool[0] != 0)
    {
        memmove(s->pool, s->pool + 1, XS_POOL - 1);
        s->pool[XS_POOL - 1] = 0;
    }
}

/* A data read or a file seek moved the head since the drive was last used
 * for a voice (PsyCross counts those). */
static void DriveSync(XaShadow* s, double t)
{
    if (s->epoch != g_PsxCdHeadEpoch)
    {
        s->epoch       = g_PsxCdHeadEpoch;
        s->reading     = 0;
        s->holdLba     = g_PsxCdHeadLba;
        s->holdSinceMs = t;
        s->holdPhase   = 0;
    }
}

static int DriveHead(const XaShadow* s, double t, double scale)
{
    int spt, turned;

    if (s->reading)
    {
        if (t <= s->readStartMs)
            return s->readStartLba;
        return s->readStartLba + (int)((t - s->readStartMs) / XS_SECTOR_MS);
    }

    /* Holding: the head circles the turn that ends two sectors past the
     * hold point. */
    spt    = SectorsPerTurn(s->holdLba);
    turned = (t > s->holdSinceMs) ? (int)((t - s->holdSinceMs) / (XS_SECTOR_MS * scale)) : 0;
    return s->holdLba + 2 - spt + (s->holdPhase + turned) % spt;
}

static int DriveIdle(const XaShadow* s, double t)
{
    return t >= s->busyUntilMs;
}

static int DrivePause(XaShadow* s, double t, double scale)
{
    if (!DriveIdle(s, t))
        return 0;

    DriveSync(s, t);
    if (s->reading)
    {
        int    head, spt;
        double ms;

        /* The drive refuses a pause until its first sector is in. */
        if (t < s->readStartMs)
            return 0;

        head = DriveHead(s, t, scale);
        spt  = SectorsPerTurn(head);
        ms   = (spt - 2) * XS_SECTOR_MS - XS_SECTOR_MS / 2.0;
        if (ms < XS_PAUSE_MIN_MS)
            ms = XS_PAUSE_MIN_MS;

        s->reading     = 0;
        s->holdLba     = head;
        s->holdSinceMs = t;
        s->holdPhase   = spt - 2;
        s->busyUntilMs = t + (XS_ACK_MS + ms) * scale;
    }
    else
    {
        s->busyUntilMs = t + (XS_ACK_MS + XS_PAUSE_IDLE_MS) * scale;
    }
    return 1;
}

static void DriveSeek(XaShadow* s, double t, int target, double scale)
{
    int    head;
    double ms;

    DriveSync(s, t);
    head = DriveHead(s, t, scale);
    ms   = SeekMs(head, target);

    s->reading     = 0;
    s->busyUntilMs = t + (XS_ACK_MS + ms) * scale;
    s->seekDoneMs  = s->busyUntilMs;
    s->holdLba     = target;
    s->holdSinceMs = s->seekDoneMs;
    s->holdPhase   = SectorsPerTurn(target) - 4;

    s->logHead   = head;
    s->logTarget = target;
    s->logSeekMs = ms;
}

static void DriveRead(XaShadow* s, double t, double scale)
{
    const int spt    = SectorsPerTurn(s->holdLba);
    const int turned = (t > s->seekDoneMs) ? (int)((t - s->seekDoneMs) / (XS_SECTOR_MS * scale)) : 0;
    const int at     = s->holdLba + 2 - spt + (s->holdPhase + turned) % spt;
    int       wait;

    if (at < s->holdLba)
        wait = (s->holdLba - at < 2) ? 2 : s->holdLba - at;
    else
        wait = s->holdLba - (at - spt);

    s->reading      = 1;
    s->readStartLba = s->holdLba;
    s->readStartMs  = t + (XS_ACK_MS + (1 + wait) * XS_SECTOR_MS + XS_JITTER_MS) * scale;
    s->busyUntilMs  = t + XS_ACK_MS * scale;
}

/* CdRead of a file: the drive seeks to the Setloc position and reads, its
 * first sector three sector times after the seek; with the last one in, the
 * library pauses the drive, and CdReadSync reports the read done once that
 * pause has completed. */
static void DriveData(XaShadow* s, double t, int lba, int sectors, double scale)
{
    const int    end   = lba + sectors;
    const int    spt   = SectorsPerTurn(end);
    const double seek  = SeekMs(DriveHead(s, t, scale), lba);
    double       pause = (spt - 2) * XS_SECTOR_MS - XS_ACK_MS;

    if (pause < XS_PAUSE_MIN_MS)
        pause = XS_PAUSE_MIN_MS;

    s->reading     = 0;
    s->dataDoneMs  = t + (XS_ACK_MS + seek + (2 + sectors) * XS_SECTOR_MS + XS_ACK_MS + pause) * scale;
    s->busyUntilMs = s->dataDoneMs;
    s->holdLba     = end - 1;
    s->holdSinceMs = s->dataDoneMs;
    s->holdPhase   = spt - 2;
}

static void StopStep(XaShadow* s, double t, double scale)
{
    switch (s->stopState)
    {
        case XS_STOP_FADE:
            s->volXa -= 24;
            s->logFades++;
            if (s->volXa < 2)
                s->stopState = XS_STOP_MUTE;
            break;

        case XS_STOP_MUTE:
            s->volXa     = 0;
            s->stopState = XS_STOP_PAUSE;
            break;

        case XS_STOP_PAUSE:
            if (DrivePause(s, t, scale))
                s->stopState = XS_STOP_CLEANUP;
            break;

        default:
            s->idx4      = 0;
            s->stopState = XS_STOP_FADE;
            s->logStops++;
            PoolUpdate(s);
            break;
    }
}

static void PlayStep(XaShadow* s, double t, double scale)
{
    switch (s->playState)
    {
        case XS_PLAY_INIT:
            s->volXa     = s->lineVol;
            s->playIdx   = s->idxCheck;
            s->playSeq   = s->reqSeq;
            s->playState = XS_PLAY_SETMODE;
            break;

        case XS_PLAY_SETMODE:
        case XS_PLAY_SETFILTER:
            if (DriveIdle(s, t))
            {
                s->busyUntilMs = t + XS_ACK_MS * scale;
                s->playState++;
            }
            break;

        case XS_PLAY_FILTER:
            s->playState = XS_PLAY_SETFILTER;
            break;

        case XS_PLAY_LBA:
            s->wdLen     = XaLength(s->playIdx) + 32;
            s->playState = XS_PLAY_SEEK;
            break;

        case XS_PLAY_SEEK:
            if (DriveIdle(s, t))
            {
                DriveSeek(s, t, XaTarget(s->playIdx), scale);
                s->playState = XS_PLAY_READ;
            }
            break;

        case XS_PLAY_READ:
            if (DriveIdle(s, t))
            {
                DriveRead(s, t, scale);
                s->startPending = 0;
                s->readSeq      = s->playSeq;
                s->readNMs      = t;
                s->playState    = XS_PLAY_ENABLE;
            }
            break;

        default:
            s->idx4       = s->playIdx;
            s->wdStartMs  = t;
            s->wdElapsed  = 0;
            s->notPlaying = 0;
            s->heardSeq   = s->playSeq;
            s->audioMs    = (s->readStartMs > t) ? s->readStartMs : t;
            s->playState  = XS_PLAY_INIT;
            PoolUpdate(s);
            break;
    }
}

static void PreloadStep(XaShadow* s, double t, double scale)
{
    switch (s->preState)
    {
        case 0:
            s->preIdx   = s->idxCheck;
            s->preState = 1;
            break;

        case 1:
        case 3:
            if (DriveIdle(s, t))
            {
                s->busyUntilMs = t + XS_ACK_MS * scale;
                s->preState++;
            }
            break;

        case 2:
            s->preState = 3;
            break;

        case 4:
            s->wdLen    = XaLength(s->preIdx) + 32;
            s->preState = 5;
            break;

        case 5:
            if (DriveIdle(s, t))
            {
                DriveSeek(s, t, XaTarget(s->preIdx), scale);
                s->preState = 6;
            }
            break;

        default:
            if (DrivePause(s, t, scale))
            {
                s->preState     = 0;
                s->startPending = 0;
                PoolUpdate(s);
            }
            break;
    }
}

static const s_AudioItemData* AudioItem(int idx)
{
    return (idx >= 0 && idx < XS_AUDIO_COUNT) ? &g_AudioData[idx] : NULL;
}

static void LoadDone(XaShadow* s)
{
    s->loadState = XS_LOAD_RESET;
    s->loading   = 0;
    PoolUpdate(s);
}

/* The Setloc and CdRead states shared by both loads: 1 once the read is on
 * its way. */
static int LoadRead(XaShadow* s, double t, int sectorOfs, int bytes, int next, double scale)
{
    if (!DriveIdle(s, t))
        return 0;

    if (s->loadState == XS_LOAD_SETOFF || s->loadState == XS_LOAD_SETNEXT)
        s->busyUntilMs = t + XS_ACK_MS * scale;
    else
        DriveData(s, t, s->loadLba + sectorOfs, (bytes + 2047) / 2048, scale);
    s->loadState = next;
    return 1;
}

/* Sd_VabLoad. Handing a piece to the SPU is over within the frame, so the
 * two states that wait for it never do. */
static void VabStep(XaShadow* s, double t, int task, double scale)
{
    const s_AudioItemData* item = AudioItem(task - 160);
    int                    left;

    if (item == NULL)
    {
        LoadDone(s);
        return;
    }

    left = s->loadSize - s->loadMoved;
    switch (s->loadState)
    {
        case XS_LOAD_RESET:
            if (item->typeIdx_0 >= 1 && item->typeIdx_0 <= 3)
            {
                if (s->lastVab[item->typeIdx_0 - 1] == task)
                {
                    LoadDone(s);
                    break;
                }
                s->lastVab[item->typeIdx_0 - 1] = task;
            }
            s->loadLba   = item->fileOffset_8;
            s->loadSize  = (int)item->fileSize_4;
            s->loadState = XS_LOAD_STOP;
            break;

        case XS_LOAD_STOP:
            s->loadMoved = 0;
            s->loadState = XS_LOAD_SETOFF;
            break;

        case XS_LOAD_SETOFF:
            DriveSync(s, t);
            LoadRead(s, t, 0, 0, XS_LOAD_FILE, scale);
            break;

        case XS_LOAD_FILE:
            LoadRead(s, t, 0, (s->loadSize < XS_VAB_CHUNK) ? s->loadSize : XS_VAB_CHUNK, XS_LOAD_CHECK, scale);
            break;

        case XS_LOAD_CHECK:
            if (t >= s->dataDoneMs)
                s->loadState = XS_LOAD_MOVE;
            break;

        case XS_LOAD_MOVE:
            if (s->loadSize < XS_VAB_CHUNK)
            {
                s->loadMoved = s->loadSize;
                s->loadState = XS_LOAD_FINALIZE;
            }
            else
            {
                s->loadMoved = XS_VAB_CHUNK;
                s->loadState = XS_LOAD_SETNEXT;
            }
            break;

        case XS_LOAD_SETNEXT:
            LoadRead(s, t, 0, 0, XS_LOAD_MOVENEXT, scale);
            break;

        case XS_LOAD_MOVENEXT:
            LoadRead(s, t, (s->loadMoved + 2047) / 2048, (left < XS_VAB_CHUNK) ? left : XS_VAB_CHUNK, XS_LOAD_MOVELAST, scale);
            break;

        case XS_LOAD_MOVELAST:
            if (t < s->dataDoneMs)
                break;
            if (left < XS_VAB_CHUNK)
            {
                s->loadMoved = s->loadSize;
                s->loadState = XS_LOAD_FINALIZE;
            }
            else
            {
                s->loadMoved += XS_VAB_CHUNK;
                s->loadState  = XS_LOAD_SETNEXT;
            }
            break;

        default:
            LoadDone(s);
            break;
    }
}

/* Sd_KdtLoad. */
static void KdtStep(XaShadow* s, double t, int task, double scale)
{
    const s_AudioItemData* item = AudioItem(54 + task);

    if (item == NULL)
    {
        LoadDone(s);
        return;
    }

    switch (s->loadState)
    {
        case XS_LOAD_RESET:
            s->loadLba   = item->fileOffset_8;
            s->loadSize  = (int)item->fileSize_4;
            s->loadState = XS_LOAD_STOP;
            break;

        case XS_LOAD_STOP:
            s->loadState = XS_LOAD_SETOFF;
            break;

        case XS_LOAD_SETOFF:
            DriveSync(s, t);
            LoadRead(s, t, 0, 0, XS_LOAD_FILE, scale);
            break;

        case XS_LOAD_FILE:
            LoadRead(s, t, 0, s->loadSize, XS_LOAD_CHECK, scale);
            break;

        default:
            if (t >= s->dataDoneMs)
                LoadDone(s);
            break;
    }
}

/* One console frame of Sd_TaskPoolExecute. */
static void Tick(XaShadow* s, double t, double scale)
{
    const int task = s->pool[0];

    switch (task)
    {
        case 0: break;
        case 1: PlayStep(s, t, scale);    break;
        case 2: StopStep(s, t, scale);    break;
        case 6: PreloadStep(s, t, scale); break;

        case 7: /* func_80046A70: the track starts, at full volume */
            s->bgmVol = 40;
            PoolUpdate(s);
            break;

        case 8: /* Sd_StopBgm */
            if (s->bgmVol > 0)
                s->bgmVol -= 4;
            if (s->bgmVol <= 0)
            {
                s->bgmVol = 0;
                PoolUpdate(s);
            }
            break;

        default:
            if (task >= 160)
                VabStep(s, t, task, scale);
            else if (task >= 32)
                KdtStep(s, t, task, scale);
            else
                PoolUpdate(s);
            break;
    }

    if (s->idx4 != 0)
        s->wdElapsed = (int)(Vblanks(t) - Vblanks(s->wdStartMs));

    /* The watchdog that ends a line: audio length plus 32 vblanks after the
     * CD input came on, the stop task follows. */
    if (s->wdElapsed > s->wdLen && task == 0)
    {
        if (!s->notPlaying)
            PoolAdd(s, 2);
        s->wdStartMs = t;
        s->wdElapsed = 0;
    }
}

/* A line on its way keeps the frame time its player's delay was worked out
 * with, so its page and its voice stay together through a change of shot. */
static double FrameMs(const XaShadow* s)
{
    return (s->lineFrameMs > 0.0 && s->heardSeq != s->askSeq) ? s->lineFrameMs : ConsoleFrameMs();
}

static void Apply(XaShadow* s, const XaRequest* r, double t)
{
    int i, music = 0, bank = 0;

    switch (r->type)
    {
        case XS_REQ_PLAY:
            /* Sd_XaAudioPlayTaskAdd, in its order: a play still queued behind
             * the head is withdrawn by the stop, and takes the pending flag
             * with it. */
            s->idxCheck     = r->arg;
            s->startPending = 1;
            s->wdStartMs    = t;
            s->wdElapsed    = 0;
            s->logStops     = 0;
            s->logFades     = 0;
            PoolAdd(s, 2);
            s->idx4 = r->arg;
            PoolAdd(s, 1);
            s->reqSeq++;
            break;

        case XS_REQ_STOP:
            PoolAdd(s, 2);
            s->notPlaying = 1;
            break;

        case XS_REQ_PRELOAD:
            s->idxCheck     = r->arg;
            s->startPending = 1;
            if (s->idx4 != 0)
                PoolAdd(s, 2);
            PoolAdd(s, 6);
            break;

        case XS_REQ_STOP_ACTIVE:
            if (s->idx4 != 0)
                PoolAdd(s, 2);
            break;

        default:
            /* The game's music volume and loaded banks are the console's at
             * this point unless the shadow still has a task of that kind
             * queued, which the port's pool has long since run. */
            for (i = 0; i < XS_POOL; i++)
            {
                music |= (s->pool[i] == 7 || s->pool[i] == 8);
                bank  |= (s->pool[i] >= 160);
            }
            if (!music)
                s->bgmVol = r->bgmVol;
            if (!bank)
            {
                for (i = 0; i < 3; i++)
                    s->lastVab[i] = r->lastVab[i];
            }
            if (r->arg >= 32)
                s->loading = 1;
            PoolAdd(s, r->arg);
            break;
    }

    if (!s->gridOn)
        s->gridPending = 1;
}

/* One console frame: a pass of the pool, then the next game frame's requests.
 *
 * On the console the requests of two game frames always had a pass between
 * them: a page turn stops the old line in one frame and the next line is
 * asked for in the next, by which time the stop has run a state (and a stop
 * that was on its last one has left the head, which decides whether the
 * request's own stop joins it or is merged away). The port's frames are a
 * quarter as long, so both would usually arrive between two passes. Requests
 * from a later game frame therefore wait here for the pass. */
static void Step(XaShadow* s, double t, double scale)
{
    Tick(s, t, scale);
    s->ticked = 1;

    if (s->queued > 0)
    {
        const unsigned frame = s->queue[0].frame;
        int            n     = 0;

        while (n < s->queued && s->queue[n].frame == frame)
        {
            Apply(s, &s->queue[n], t);
            n++;
        }
        memmove(s->queue, s->queue + n, (size_t)(s->queued - n) * sizeof(s->queue[0]));
        s->queued    -= n;
        s->batchFrame = frame;
        s->ticked     = 0;
    }
}

static void Advance(XaShadow* s, double now, double scale)
{
    int guard = 0;

    while (s->gridOn && s->nextTickMs <= now)
    {
        Step(s, s->nextTickMs, scale);
        s->nextTickMs += FrameMs(s) * scale;
        if (s->pool[0] == 0 && s->idx4 == 0 && s->queued == 0)
            s->gridOn = 0;
        if (++guard > 20000)
        {
            s->nextTickMs = now + FrameMs(s) * scale;
            break;
        }
    }
}

static void Reset(void)
{
    memset(&s_live, 0, sizeof(s_live));
    s_live.volXa   = 84; /* sd_work_init */
    s_live.lineVol = 84;
    s_live.holdLba = g_PsxCdHeadLba;
    s_live.epoch   = g_PsxCdHeadEpoch;
    s_live.ticked  = 1;
    s_ready        = 1;
}

/* The shadow brought up to now, or NULL with Dialog Pause off. */
static XaShadow* Live(double now)
{
    const double scale = Scale();

    if (scale <= 0.0)
    {
        s_ready = 0;
        return NULL;
    }
    /* SDL's millisecond count wraps after 49 days; every time held here would
     * then lie in the future. */
    if (!s_ready || now + 60000.0 < s_lastNow)
        Reset();
    s_lastNow = now;
    if (!s_paused)
        Advance(&s_live, now, scale);
    return &s_live;
}

static void Ask(int type, int arg, int bgmVol, const uint16_t* lastVab)
{
    const double now = NowMs();
    XaShadow*    s   = Live(now);
    XaRequest    r;
    int          i;

    if (s == NULL)
        return;

    r.type   = type;
    r.arg    = arg;
    r.bgmVol = bgmVol;
    r.frame  = s_gameFrame;
    for (i = 0; i < 3; i++)
        r.lastVab[i] = lastVab ? lastVab[i] : 0;

    /* A scene step that asks for the same thing every frame until it moves
     * on (the cafe's preload) made one request per pass on the console. */
    if (s->queued > 0)
    {
        const XaRequest* last = &s->queue[s->queued - 1];

        if (last->frame != r.frame && last->type == r.type && last->arg == r.arg)
            return;
    }

    if (type == XS_REQ_PLAY)
    {
        s->askSeq++;
        s->reqMs       = now;
        s->lineFrameMs = 0.0;
    }

    if (s->queued == XS_QUEUE)
    {
        for (i = 0; i < s->queued; i++)
            Apply(s, &s->queue[i], now);
        s->queued = 0;
        s->ticked = 1;
    }

    if (s->queued == 0 && (s->ticked || r.frame == s->batchFrame))
    {
        Apply(s, &r, now);
        s->batchFrame = r.frame;
        s->ticked     = 0;
    }
    else
    {
        s->queue[s->queued++] = r;
    }
}

void Pc_XaSeek_PlayRequest(uint16_t xaIdx)
{
    Ask(XS_REQ_PLAY, xaIdx, 0, NULL);
}

void Pc_XaSeek_StopRequest(void)
{
    Ask(XS_REQ_STOP, 0, 0, NULL);
}

void Pc_XaSeek_PreloadRequest(uint16_t xaIdx)
{
    Ask(XS_REQ_PRELOAD, xaIdx, 0, NULL);
}

void Pc_XaSeek_StopIfActive(void)
{
    Ask(XS_REQ_STOP_ACTIVE, 0, 0, NULL);
}

void Pc_XaSeek_TaskAdd(int task, int bgmVol, const uint16_t* lastVab)
{
    Ask(XS_REQ_TASK, task, bgmVol, lastVab);
}

void Pc_XaSeek_Frame(void)
{
    const double now = NowMs();
    XaShadow*    s;

    /* A quarter of each frame's load: a cut settles within a few frames and
     * a single odd frame does not move a boundary. */
    if (!s_paused)
    {
        const double bytes = g_PcFramePacketBytes * XS_PSX_PER_PC_BYTE;

        s_loadBytes = (s_loadBytes < 0.0) ? bytes : s_loadBytes + (bytes - s_loadBytes) * 0.25;
    }

    s = Live(now);
    s_gameFrame++;
    if (s == NULL || s_paused)
        return;

    /* The console ran the first state at the end of the frame that asked. */
    if (s->gridPending || (s->queued > 0 && !s->gridOn))
    {
        s->gridPending = 0;
        if (!s->gridOn)
        {
            s->gridOn     = 1;
            s->nextTickMs = now;
        }
        Advance(s, now, Scale());
    }
}

void Pc_XaSeek_OnPlayTask(uint16_t xaIdx, int xaVol)
{
    const double now   = NowMs();
    const double scale = Scale();
    XaShadow*    s     = Live(now);
    XaShadow     c;
    double       frame, page, voice;
    int          i;

    s_lineDelayMs = 0;
    if (s == NULL || !XaValid(xaIdx))
        return;

    s->lineVol     = xaVol;
    s->lineFrameMs = ConsoleFrameMs();

    /* Run a copy ahead to the frame this line's CD input comes on. */
    c     = *s;
    frame = s->lineFrameMs * scale;
    if ((c.gridPending || c.queued > 0) && !c.gridOn)
    {
        c.gridOn     = 1;
        c.nextTickMs = now;
    }
    for (i = 0; i < 900 && c.heardSeq != s->askSeq && c.gridOn; i++)
    {
        Step(&c, c.nextTickMs, scale);
        c.nextTickMs += frame;
    }
    if (c.heardSeq != s->askSeq)
        return;

    page  = (c.readNMs > now) ? c.readNMs - now : 0.0;
    voice = (c.audioMs > now) ? c.audioMs - now : 0.0;
    if (voice > XS_PAGE_LIMIT_MS * (scale > 1.0 ? scale : 1.0))
        voice = XS_PAGE_LIMIT_MS * (scale > 1.0 ? scale : 1.0);
    s_lineDelayMs = (Uint32)voice;

    SH_DBG("[XASEEK] t=%.0f xa=%u head=%d target=%d seek=%.0fms stops=%d fades=%d load=%.0f frame=%.1fms page=%.0fms voice=%.0fms",
           now, (unsigned)xaIdx, c.logHead, c.logTarget, c.logSeekMs, c.logStops, c.logFades, s_loadBytes, frame, page, voice);
}

uint32_t Pc_XaSeek_LineDelayMs(void)
{
    const uint32_t d = s_lineDelayMs;

    s_lineDelayMs = 0;
    return d;
}

int Pc_XaSeek_PageGate(void)
{
    XaShadow* s;
    double    now;

    /* Fast forward is for getting through a scene. */
    if (g_PcFastForward)
        return 0;

    now = NowMs();
    s   = Live(now);
    if (s == NULL)
        return 0;

    if (s->readSeq != s->askSeq && !s_paused &&
        now - s->reqMs > XS_PAGE_LIMIT_MS * (Scale() > 1.0 ? Scale() : 1.0))
    {
        SH_DBG("[XASEEK] page released after %.0f ms without its line (task %d, play state %d)",
               now - s->reqMs, (int)s->pool[0], s->playState);
        s->readSeq      = s->askSeq;
        s->startPending = 0;
    }

    /* Sd_AudioStreamingCheck() == 4, on the shadow's flags. */
    if (s->idx4 == 0 && !s->loading && s->preState == 0 && s->startPending)
    {
        if (s->pool[0] != 0 || s->gridPending || s->queued > 0)
            return 2;
        s->startPending = 0;
    }
    return (s->readSeq != s->askSeq) ? 1 : 0;
}

void Pc_XaSeek_SetPauseHold(int hold)
{
    const double now = NowMs();

    hold = hold ? 1 : 0;
    if (hold == s_paused)
        return;
    s_paused = hold;

    if (hold)
    {
        s_pauseStart = now;
    }
    else if (s_ready)
    {
        const double held = now - s_pauseStart;

        s_live.wdStartMs   += held;
        s_live.busyUntilMs += held;
        s_live.seekDoneMs  += held;
        s_live.dataDoneMs  += held;
        s_live.readStartMs += held;
        s_live.holdSinceMs += held;
        s_live.nextTickMs  += held;
        s_live.reqMs       += held;
        s_live.readNMs     += held;
        s_live.audioMs     += held;
        s_lastNow           = now;
    }
}
