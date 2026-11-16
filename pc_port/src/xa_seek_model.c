/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The wait between a voice line being asked for and being heard, as the
 * PlayStation had it.
 *
 * On the console a line is two or three sound tasks, run one state per game
 * frame by Sd_TaskPoolExecute:
 *   Sd_XaAudioStop  fade the last line out 24 a frame, mute, CdlPause, clean up;
 *   Sd_XaAudioStop  again when the request came from a page turn: SD_Call(19)
 *                   queues one, the play request queues another, and the pool
 *                   never merges an entry with the one at its head;
 *   Sd_XaAudioPlay  set up, CdlSetmode, filter, CdlSetfilter, sector, CdlSeekL,
 *                   CdlReadN once the seek has completed, switch the CD input on.
 * Every command waits for the one before it (Sd_CdPrimitiveCmdTry polls
 * CdSync), so the seek is the part that varies.
 *
 * Sd_AudioStreamingCheck() reports 4 from the request until CdlReadN is sent,
 * and for that long a voiced page (Gfx_MapMsg_Draw) draws nothing and does not
 * run its timer. Subtitle and voice therefore arrive together, a third to two
 * thirds of a second after the page before them ended, and every scene's
 * message timers were authored on top of that.
 *
 * The PC port runs those tasks in one frame and its player starts at once, so
 * this file replays the console's side on a shadow of the pool and the task
 * states, stepped at the console's frame rate, with a model of the drive
 * answering the commands. It is fed the same requests as the real pool and
 * gives back two things: whether the page is still held (Pc_XaSeek_PageGate)
 * and how long the player holds the line's first sound
 * (Pc_XaSeek_LineDelayMs). Dialog Pause (cutscene_line_gap_ms) scales the
 * whole sequence: 300 is the console, 0 switches it off.
 *
 * Drive timings are the ones DuckStation measured against hardware: a command
 * is acknowledged after about 0.74 ms; a pause from reading takes one turn of
 * the disc back to the hold point; a jump that leaves the sled where it is
 * takes about 50 ms below a distance that shrinks from ~330 sectors at the
 * start of the disc to ~60 at its end, and about 100 ms above it; a sled move
 * takes 0.3 s to 0.9 s; the first sector after CdlReadN waits for the hold
 * point to come round. They are written here from those figures. */
#include "xa_player.h"
#include "pc_config.h"
#include "sh_log.h"
#include "common.h"
#include "main/fileinfo.h"
#include "bodyprog/sound/sound_system.h"
#include <SDL_timer.h>
#include <math.h>
#include <string.h>

extern int      g_PsxCdHeadLba;
extern unsigned g_PsxCdHeadEpoch;
extern s32      g_IntervalVBlanks;
extern int      g_PcFastForward;
extern int      GetVideoMode(void);

#define XS_POOL          32
#define XS_XA_COUNT      727
#define XS_NORMAL_MS     300.0            /* cutscene_line_gap_ms of the "Normal" preset */
#define XS_SECTOR_MS     (1000.0 / 150.0) /* CdlModeSpeed: the voice stream reads at double speed */
#define XS_ACK_MS        0.738
#define XS_PAUSE_IDLE_MS 0.218
#define XS_PAUSE_MIN_MS  29.5
#define XS_JITTER_MS     4.1              /* 0.5 ms up to 1 ms and a sector on hardware; the middle */
#define XS_SLED_SECTORS  7200
#define XS_DISC_SECTORS  (72.0 * 60.0 * 75.0)
#define XS_PI            3.14159265358979

enum { XS_STOP_FADE, XS_STOP_MUTE, XS_STOP_PAUSE, XS_STOP_CLEANUP };
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

typedef struct
{
    u8  pool[XS_POOL]; /* g_Sd_TaskPool, holding only tasks 1 (play), 2 (stop), 6 (preload) */
    int stopState;
    int playState;
    int preState;
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

    unsigned reqSeq;   /* play requests so far */
    unsigned playSeq;  /* the request the running play task serves */
    unsigned readSeq;  /* the last request whose CdlReadN went out, or was withdrawn */
    unsigned heardSeq; /* the last request whose CD input was switched on */
    double   reqMs;
    double   readNMs;
    double   audioMs;

    int    logHead;
    int    logTarget;
    int    logStops;
    int    logFades;
    double logSeekMs;
} XaShadow;

/* No page waits longer than this (times the Dialog Pause scale) for its
 * line, whatever the shadow believes: the longest real case, a full-disc seek
 * behind two stop tasks, is about a second and a half. */
#define XS_PAGE_LIMIT_MS 4000.0

static XaShadow s_live;
static int      s_ready       = 0;
static int      s_paused      = 0;
static double   s_pauseStart  = 0.0;
static double   s_lastNow     = 0.0;
static Uint32   s_lineDelayMs = 0;

static double NowMs(void)
{
    return (double)SDL_GetTicks();
}

static double Scale(void)
{
    const int gap = g_PcConfig.cutsceneLineGapMs;

    return gap > 0 ? gap / XS_NORMAL_MS : 0.0;
}

static double VblankHz(void)
{
    return GetVideoMode() == 1 ? 50.0 : 60.0;
}

static double FrameMs(void)
{
    int vbl = g_IntervalVBlanks;

    /* Dialogue runs in the frame loop's wait-one-vblank branch, where the
     * console took the two its frame needed; a 1 here is a PC menu state. */
    if (vbl < 2) vbl = 2;
    if (vbl > 4) vbl = 4;
    return 1000.0 * vbl / VblankHz();
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

/* Constant linear velocity: 1.3 m/s past the head, tracks 1.6 um apart,
 * the first one 25 mm out. */
static int SectorsPerTurn(int lba)
{
    const double seconds = ((lba > 0 ? lba : 0) + 150) / 75.0;
    const double radius  = sqrt(0.025 * 0.025 + 1.3 * 1.6e-6 * seconds / XS_PI);
    const int    n       = (int)(2.0 * XS_PI * radius * 75.0 / 1.3);

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

/* One console frame of Sd_TaskPoolExecute. */
static void Tick(XaShadow* s, double t, double scale)
{
    const int task = s->pool[0];

    switch (task)
    {
        case 1: PlayStep(s, t, scale);    break;
        case 2: StopStep(s, t, scale);    break;
        case 6: PreloadStep(s, t, scale); break;
        default: break;
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

static void Advance(XaShadow* s, double now, double scale)
{
    int guard = 0;

    while (s->gridOn && s->nextTickMs <= now)
    {
        Tick(s, s->nextTickMs, scale);
        s->nextTickMs += FrameMs() * scale;
        if (s->pool[0] == 0 && s->idx4 == 0)
            s->gridOn = 0;
        if (++guard > 20000)
        {
            s->nextTickMs = now + FrameMs() * scale;
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

static void Requested(XaShadow* s)
{
    if (!s->gridOn)
        s->gridPending = 1;
}

void Pc_XaSeek_PlayRequest(uint16_t xaIdx)
{
    const double now = NowMs();
    XaShadow*    s   = Live(now);

    if (s == NULL)
        return;

    /* Sd_XaAudioPlayTaskAdd, in its order: a play still queued behind the
     * head is withdrawn by the stop, and takes the pending flag with it. */
    s->idxCheck     = xaIdx;
    s->startPending = 1;
    s->wdStartMs    = now;
    s->wdElapsed    = 0;
    s->reqMs        = now;
    s->logStops     = 0;
    s->logFades     = 0;
    PoolAdd(s, 2);
    s->idx4 = xaIdx;
    PoolAdd(s, 1);
    s->reqSeq++;
    Requested(s);
}

void Pc_XaSeek_StopRequest(void)
{
    XaShadow* s = Live(NowMs());

    if (s == NULL)
        return;
    PoolAdd(s, 2);
    s->notPlaying = 1;
    Requested(s);
}

void Pc_XaSeek_PreloadRequest(uint16_t xaIdx)
{
    XaShadow* s = Live(NowMs());

    if (s == NULL)
        return;
    s->idxCheck     = xaIdx;
    s->startPending = 1;
    if (s->idx4 != 0)
        PoolAdd(s, 2);
    PoolAdd(s, 6);
    Requested(s);
}

void Pc_XaSeek_StopIfActive(void)
{
    XaShadow* s = Live(NowMs());

    if (s == NULL || s->idx4 == 0)
        return;
    PoolAdd(s, 2);
    Requested(s);
}

void Pc_XaSeek_Frame(void)
{
    const double now = NowMs();
    XaShadow*    s   = Live(now);

    if (s == NULL || s_paused)
        return;

    /* The console ran the first state at the end of the frame that asked. */
    if (s->gridPending)
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

    s->lineVol = xaVol;

    /* Run a copy ahead to the frame this line's CD input comes on. */
    c     = *s;
    frame = FrameMs() * scale;
    if (c.gridPending && !c.gridOn)
    {
        c.gridOn     = 1;
        c.nextTickMs = now;
    }
    for (i = 0; i < 900 && c.heardSeq != s->reqSeq && c.gridOn; i++)
    {
        Tick(&c, c.nextTickMs, scale);
        c.nextTickMs += frame;
    }
    if (c.heardSeq != s->reqSeq)
        return;

    page  = (c.readNMs > now) ? c.readNMs - now : 0.0;
    voice = (c.audioMs > now) ? c.audioMs - now : 0.0;
    if (voice > 5000.0)
        voice = 5000.0;
    s_lineDelayMs = (Uint32)voice;

    SH_DBG("[XASEEK] t=%.0f xa=%u head=%d target=%d seek=%.0fms stops=%d fades=%d frame=%.1fms page=%.0fms voice=%.0fms",
           now, (unsigned)xaIdx, c.logHead, c.logTarget, c.logSeekMs, c.logStops, c.logFades, frame, page, voice);
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

    if (s->readSeq != s->reqSeq && !s_paused &&
        now - s->reqMs > XS_PAGE_LIMIT_MS * (Scale() > 1.0 ? Scale() : 1.0))
    {
        SH_DBG("[XASEEK] page released after %.0f ms without its line (task %d, play state %d)",
               now - s->reqMs, (int)s->pool[0], s->playState);
        s->readSeq      = s->reqSeq;
        s->startPending = 0;
    }

    /* Sd_AudioStreamingCheck() == 4, on the shadow's flags. */
    if (s->idx4 == 0 && s->preState == 0 && s->startPending)
    {
        if (s->pool[0] != 0 || s->gridPending)
            return 2;
        s->startPending = 0;
    }
    return (s->readSeq != s->reqSeq) ? 1 : 0;
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
        s_live.readStartMs += held;
        s_live.holdSinceMs += held;
        s_live.nextTickMs  += held;
        s_live.reqMs       += held;
        s_live.readNMs     += held;
        s_live.audioMs     += held;
        s_lastNow           = now;
    }
}
