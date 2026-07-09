/*
 * fmv_xbox.c - Real FMV playback (intro movie + story cutscenes) for the Xbox
 * port: STR/MDEC video + interleaved XA audio, straight from the BIN on HDD.
 *
 * FMV_Play(file_idx, max_frames) is the game's ONLY entry point (open_main in
 * src/screens/stream/stream.c under SH_PC_PORT). Contract, mirrored from the
 * PC player (pc_port/src/fmv/fmv_player.cpp):
 *   - BLOCKS until the movie ends, max_frames video frames played (0 = all),
 *     or the player skips it (Start / any face button).
 *   - Returns 0 on completion/skip, -1 on open failure. The caller ignores the
 *     value and immediately continues its state machine, so the GPU frame
 *     state must be exactly "post-VSync" (an open back-buffer frame) — we
 *     guarantee that by presenting through the real VSync() itself.
 *
 * Pipeline (all main-thread):
 *   BIN (shared stdio FILE* from cd_xbox.c; safe: the FS queue is drained by
 *   open_main first and CdRead re-seeks absolutely) --str_demux--> per-frame
 *   MDEC bitstream --mdec.c--> RGB24 --> A8R8G8B8 into a double-buffered
 *   GPU texture (write-combined; sfence) --> fullscreen quad --> VSync(),
 *   which also runs Pad_Poll + Audio_XboxPump + the game's vblank callback.
 *   XA audio sectors the demuxer encounters are forwarded to xa_xbox.c's FMV
 *   stream mode (ADPCM decode + 48 kHz resample into the existing ring that
 *   Audio_RenderInto mixes into DirectSound). Like the PC player, FMV audio
 *   BYPASSES the Sd / XaPlayer bookkeeping (no Xa_SignalPlaybackFinished).
 *
 * Pacing: SH STRs are 15 fps. Pacing is wall-clock (GetTickCount), NOT the
 * VSync counter — that counter only advances while we WAIT, so a slow decode
 * would be invisible to it. If decode falls behind on the 733 MHz CPU, LATE
 * FRAMES ARE DROPPED (STR frames are intra-only, so every frame is a clean
 * resync point); audio is never starved because the demuxer forwards audio
 * sectors even for frames we drop. Consecutive drops are capped so the XA
 * ring (~680 ms) can't overflow while catching up.
 *
 * Deviations from the PC player: no AVI override path (ReadAVI/libjpeg is
 * PC-only), no SDL audio device (our XA ring/DirectSound instead), letterbox
 * fits against the fixed 640x480 surface (SH movies are 320x240 = exact fit).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fmv/mdec.h"       /* pc_port/src/fmv (on the include path via -Ipc_port/src) */
#include "fmv/str_demux.h"
#include "gpu_nv2a.h"
#include "sh_log.h"

/* --- HAL externs (C links by name) ------------------------------------------*/
extern void  Cd_XboxInit(void);                    /* cd_xbox.c */
extern FILE* Cd_XboxGetBinFile(void);
extern int   VSync(int mode);                      /* psx_libgpu_xbox.c */
extern unsigned short Pad_XboxButtons(void);       /* pad_xbox.c (active-low) */
extern void  Xa_XboxStreamBegin(void);             /* xa_xbox.c FMV stream mode */
extern void  Xa_XboxStreamFeedSector(const unsigned char* sector2336);
extern void  Xa_XboxStreamEnd(void);
extern unsigned int Xa_XboxStreamRingLevel(void);
extern unsigned int Xa_XboxStreamSectorsFed(void);

/* --- file_idx -> BIN sector map ----------------------------------------------
 * Copied verbatim from pc_port/src/fmv/fmv_player.cpp (sector offsets from
 * filetable.c.inc entries 2044..2073, the XA/MOVIE block on SH USA). Entries
 * 0..8 are XA-only voice banks; 9..29 are the video+audio cutscenes. The
 * demuxer skips audio sectors in the video path, so one table serves both. */
typedef struct {
    const char*  name;
    unsigned int base_sector;
    unsigned int n_sectors;
} FmvFileEntry;

static const FmvFileEntry s_fmvFiles[] = {
    { "05_02152", 0x099bf,  2152 },  /* 0  - in-game voice line (XA-only) */
    { "10_04432", 0x0a227,  4432 },  /* 1 */
    { "15_07496", 0x0b377,  7496 },  /* 2 */
    { "20_06552", 0x0d0bf,  6552 },  /* 3 */
    { "25_03904", 0x0ea57,  3904 },  /* 4 */
    { "30_04056", 0x0f997,  4056 },  /* 5 */
    { "35_26008", 0x1096f, 26008 },  /* 6 */
    { "40_10384", 0x16f07, 10384 },  /* 7 */
    { "45_28784", 0x19797, 28784 },  /* 8 */
    { "C1_20670", 0x20807, 20670 },  /* 9  - intro (US) */
    { "C2_20670", 0x258c5, 20670 },  /* 10 - intro (JP) */
    { "M1_03500", 0x2a983,  3500 },  /* 11 - opening */
    { "M2_01190", 0x2b72f,  1190 },  /* 12 */
    { "M3_02570", 0x2bbd5,  2570 },  /* 13 */
    { "M4_02490", 0x2c5df,  2490 },  /* 14 */
    { "M5_03140", 0x2cf99,  3140 },  /* 15 */
    { "M6_02112", 0x2dbdd,  2112 },  /* 16 */
    { "M7_01536", 0x2e41d,  1536 },  /* 17 */
    { "M8_03039", 0x2ea1d,  3039 },  /* 18 */
    { "M9_01730", 0x2f5fc,  1730 },  /* 19 */
    { "MA_03590", 0x2fcbe,  3590 },  /* 20 */
    { "MB_04850", 0x30ac4,  4850 },  /* 21 */
    { "MC_01930", 0x31db6,  1930 },  /* 22 */
    { "MD_03780", 0x32540,  3780 },  /* 23 */
    { "ME_03300", 0x33404,  3300 },  /* 24 */
    { "Z1_16180", 0x340e8, 16180 },  /* 25 */
    { "Z3_02340", 0x3801c,  2340 },  /* 26 */
    { "Z4_01590", 0x38940,  1590 },  /* 27 */
    { "ZC_14392", 0x38f76, 14392 },  /* 28 */
    { "ZZ_14239", 0x3c7ae, 14239 },  /* 29 */
};

#define FMV_FILE_COUNT       (sizeof(s_fmvFiles) / sizeof(s_fmvFiles[0]))
#define FIRST_XA_FILE_IDX    2044   /* FILE_XA_05_02152 (fileenum.h.USA.inc) */

/* 15 fps -> one video frame per 1000*FMV_FRAME_MS_NUM/FMV_FRAME_MS_DEN ms. */
#define FMV_FRAME_MS_NUM     200    /* 1000/15 = 200/3 ms */
#define FMV_FRAME_MS_DEN     3
/* Each dropped frame feeds ~66 ms of audio without consuming any; cap the
 * catch-up burst so the ~680 ms XA ring can't overflow (drop cap of 4 =
 * ~266 ms pushed ahead, on top of the low steady-state ring level). */
#define FMV_MAX_CONSEC_DROPS 4

/* --- skip: Start or any face button ------------------------------------------
 * PSX digitalButtons bits (active-low, see pad_xbox.c): 3=Start, 12=Triangle,
 * 13=Circle, 14=Cross, 15=Square. Pad_Poll refreshes the buffer inside every
 * VSync we run, so reads here are at most one vblank stale. */
#define FMV_SKIP_MASK 0xF008u

static int Fmv_SkipHeld(void)
{
    return ((~(unsigned)Pad_XboxButtons()) & FMV_SKIP_MASK) != 0;
}

/* Skip is armed only after the buttons have been seen released once — the
 * press that STARTED the movie (menu confirm) is usually still held on the
 * first iterations and would otherwise skip at frame 0 (PC parity). */
static int Fmv_SkipPoll(int* armed)
{
    int held = Fmv_SkipHeld();
    if (!*armed) {
        if (!held)
            *armed = 1;
        return 0;
    }
    return held;
}

/* --- buffers (persistent, reused across movies; no per-movie leaks) ----------*/
static uint16_t  s_bs[STR_FRAME_BS_MAX_HALFWORDS];  /* 32 KB frame bitstream */
static uint8_t*  s_rgb    = NULL;                   /* RGB24 decode target */
static int       s_rgbCap = 0;
static uint32_t* s_tex[2] = { NULL, NULL };         /* A8R8G8B8, GPU-visible */
static int       s_texCap = 0;                      /* bytes per tex buffer */

static int Fmv_EnsureBuffers(int w, int h)
{
    int rgbBytes = w * h * 3;
    int texBytes = w * h * 4;

    if (rgbBytes > s_rgbCap) {
        free(s_rgb);
        s_rgb    = (uint8_t*)malloc((size_t)rgbBytes);
        s_rgbCap = s_rgb ? rgbBytes : 0;
    }
    if (texBytes > s_texCap) {
        /* GpuNv2a_AllocTexMem has no free; grow-only "leak" is bounded — SH
         * movies are all 320x240, so this normally allocates exactly once
         * (2 x 300 KB) for the whole session. */
        s_tex[0] = (uint32_t*)GpuNv2a_AllocTexMem(texBytes);
        s_tex[1] = (uint32_t*)GpuNv2a_AllocTexMem(texBytes);
        s_texCap = (s_tex[0] && s_tex[1]) ? texBytes : 0;
    }
    return (s_rgb != NULL) && (s_tex[0] != NULL) && (s_tex[1] != NULL);
}

/* --- fullscreen video quad ----------------------------------------------------
 * Screen-space verts for GpuNv2a_EmitTris: pos in pixels, col 0..1 (pixel
 * shader = tex * col, so 1.0 shows the texture unmodified), UV in TEXELS,
 * spec (fog add) = 0, blend off. Re-issued per present because FrameBegin
 * rebinds the white texture + default state each frame. */
static void Fmv_DrawFrame(const uint32_t* tex, int w, int h)
{
    ShVertex v[6];
    float    x0 = 0.0f, y0 = 0.0f, x1 = 640.0f, y1 = 480.0f;
    float    va = (h > 0) ? ((float)w / (float)h) : (4.0f / 3.0f);
    int      i;

    /* Aspect-fit letterbox against the fixed 640x480 surface (PC parity);
     * 320x240 sources fill the screen exactly. */
    if (va > (4.0f / 3.0f)) {
        float hh = 640.0f / va;
        y0 = (480.0f - hh) * 0.5f;
        y1 = y0 + hh;
    } else if (va < (4.0f / 3.0f)) {
        float ww = 480.0f * va;
        x0 = (640.0f - ww) * 0.5f;
        x1 = x0 + ww;
    }

    memset(v, 0, sizeof(v));
    for (i = 0; i < 6; i++) {
        v[i].col[0] = 1.0f;
        v[i].col[1] = 1.0f;
        v[i].col[2] = 1.0f;
        v[i].col[3] = 1.0f;
    }
    v[0].pos[0] = x0; v[0].pos[1] = y0; v[0].tex[0] = 0.0f;     v[0].tex[1] = 0.0f;
    v[1].pos[0] = x1; v[1].pos[1] = y0; v[1].tex[0] = (float)w; v[1].tex[1] = 0.0f;
    v[2].pos[0] = x0; v[2].pos[1] = y1; v[2].tex[0] = 0.0f;     v[2].tex[1] = (float)h;
    v[3] = v[1];
    v[4].pos[0] = x1; v[4].pos[1] = y1; v[4].tex[0] = (float)w; v[4].tex[1] = (float)h;
    v[5] = v[2];

    GpuNv2a_SetBlendMode(0);
    /* The game may have left a draw-env clip active; full-screen for the movie.
     * Safe to leave: the game's next PutDrawEnv (every frame) restores it. */
    GpuNv2a_SetScissor(0, 0, 0, 0);
    GpuNv2a_BindTexture(tex, w, h);
    GpuNv2a_EmitTris(v, 6);
}

/* str_demux audio-sector callback: forward to the XA FMV stream (decodes into
 * the 48 kHz ring; DirectSound output happens via Audio_XboxPump in VSync). */
static void Fmv_OnAudioSector(const uint8_t* sector, void* user)
{
    (void)user;
    Xa_XboxStreamFeedSector(sector);
}

int FMV_Play(int file_idx, int max_frames)
{
    const FmvFileEntry* e;
    FILE*        bin;
    str_stream_t stream;
    mdec_ctx_t   ctx;
    int   ctxInit   = 0;
    int   w = 0, h = 0;
    int   done = 0, dropped = 0, presented = 0, consecDrops = 0, decodeFails = 0;
    int   skipArmed = 0, skipped = 0;
    int   texCur    = 0;
    DWORD baseMs    = 0;
    int   haveBase  = 0;
    int   table_idx = file_idx - FIRST_XA_FILE_IDX;

    if (table_idx < 0 || table_idx >= (int)FMV_FILE_COUNT) {
        SH_DBG("[FMV] file_idx %d out of range (table_idx=%d)", file_idx, table_idx);
        return -1;
    }
    e = &s_fmvFiles[table_idx];

    Cd_XboxInit();
    bin = Cd_XboxGetBinFile();
    if (!bin) {
        SH_DBG("[FMV] no disc image - cannot play %s", e->name);
        return -1;
    }
    if (!str_open(&stream, bin, e->base_sector, e->n_sectors)) {
        SH_DBG("[FMV] str_open failed for %s", e->name);
        return -1;
    }
    stream.audio_cb   = Fmv_OnAudioSector;
    stream.audio_user = NULL;

    SH_DBG("[FMV] start %s file_idx=%d base=%u sectors=%u maxFrames=%d (15fps)",
           e->name, file_idx, e->base_sector, e->n_sectors, max_frames);

    Xa_XboxStreamBegin();

    for (;;) {
        str_frame_info_t info;
        size_t bsHalfwords = 0;
        int    r;

        /* Demux the next video frame; audio sectors seen on the way are fed
         * to the XA ring via the callback (even for frames we then drop). */
        r = str_read_frame(&stream, s_bs, &info, &bsHalfwords);
        if (r <= 0) {
            if (r < 0)
                SH_DBG("[FMV] demux error %d at frame %d", r, done);
            break;
        }
        if (max_frames > 0 && done >= max_frames)
            break;

        if (info.width != w || info.height != h) {
            w = info.width;
            h = info.height;
            if (w <= 0 || h <= 0 || (w & 15) || (h & 15) || !Fmv_EnsureBuffers(w, h)) {
                SH_DBG("[FMV] bad dims / buffer alloc failed (%dx%d)", w, h);
                break;
            }
            if (!ctxInit) {
                mdec_init(&ctx, w, h);
                ctxInit = 1;
            } else {
                ctx.width  = w;   /* mid-stream dim change (defensive; PC reallocs too) */
                ctx.height = h;
            }
            SH_DBG("[FMV] video %dx%d, %d sectors/frame", w, h, info.n_sectors);
        }

        /* Late? (This frame's display slot fully passed.) Drop it — decode +
         * present skipped, pacing index still advances so A/V resyncs. */
        if (haveBase && consecDrops < FMV_MAX_CONSEC_DROPS &&
            (DWORD)(GetTickCount() - baseMs) >=
                (DWORD)(((done + 1) * FMV_FRAME_MS_NUM) / FMV_FRAME_MS_DEN)) {
            done++;
            dropped++;
            consecDrops++;
            continue;
        }
        consecDrops = 0;

        if (mdec_decode_frame(&ctx, (const uint8_t*)s_bs, bsHalfwords * 2u, s_rgb) < 0) {
            /* PC parity: emit a black frame so timing still ticks. */
            memset(s_rgb, 0, (size_t)(w * h * 3));
            decodeFails++;
        }

        /* RGB24 -> A8R8G8B8 into the texture NOT bound by recent presents
         * (double buffer; at 15 fps the other one was last read >=4 vblanks
         * ago), then fence the write-combined stores before the GPU reads. */
        {
            uint32_t*      dst = s_tex[texCur ^ 1];
            const uint8_t* src = s_rgb;
            int            n   = w * h;
            int            i;

            for (i = 0; i < n; i++, src += 3)
                dst[i] = 0xFF000000u | ((uint32_t)src[0] << 16)
                       | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
            __asm__ __volatile__("sfence" ::: "memory");
            texCur ^= 1;
        }

        /* Present, then hold (re-presenting each vblank) until this frame's
         * 1/15 s slot ends. VSync(SyncMode_Wait) = FrameEnd -> vblank wait ->
         * FrameBegin plus Pad_Poll + Audio_XboxPump + the game's vblank
         * callback — the exact sequence the game itself runs per frame, so
         * the GPU/timer state is game-normal when FMV_Play returns. */
        Fmv_DrawFrame(s_tex[texCur], w, h);
        VSync(0);
        presented++;
        if (!haveBase) {
            haveBase = 1;
            baseMs   = GetTickCount();   /* frame 0 visible now */
        }
        if (Fmv_SkipPoll(&skipArmed))
            skipped = 1;

        {
            DWORD slotEnd = baseMs + (DWORD)(((done + 1) * FMV_FRAME_MS_NUM) / FMV_FRAME_MS_DEN);
            while (!skipped && (int)(slotEnd - GetTickCount()) > 0) {
                Fmv_DrawFrame(s_tex[texCur], w, h);
                VSync(0);
                presented++;
                if (Fmv_SkipPoll(&skipArmed))
                    skipped = 1;
            }
        }

        done++;

        if ((done % 150) == 0)
            SH_DBG("[FMV] frame=%d dropped=%d fails=%d ring=%u fed=%u",
                   done, dropped, decodeFails,
                   Xa_XboxStreamRingLevel(), Xa_XboxStreamSectorsFed());

        if (skipped) {
            SH_DBG("[FMV] skipped at frame %d", done);
            break;
        }
    }

    Xa_XboxStreamEnd();   /* silence: FMV audio out of the mixer immediately */

    /* Wait (capped ~1 s) for the skip buttons to release so the held press
     * doesn't edge into the next state's Joy update as a phantom Confirm
     * (PC parity). VSync keeps the pad + audio pumped while waiting. */
    {
        int waitVbls = 0;
        while (waitVbls < 60 && Fmv_SkipHeld()) {
            VSync(0);
            waitVbls++;
        }
    }

    SH_DBG("[FMV] end %s: %d frames (%d dropped, %d decode fails, %d presents)",
           e->name, done, dropped, decodeFails, presented);
    return 0;
}
