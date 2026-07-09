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
 * AVI OVERRIDE (PC parity): before touching the BIN, FMV_Play looks for the
 * same MJPG+PCM AVI files the PC modding community distributes, as
 * D:\fmv\<name>.avi or D:\gamedata\fmv\<name>.avi (cd_xbox.c's two-location
 * convention; <name> = s_fmvFiles[].name, e.g. C1_20670.avi). If found and
 * usable (MJPG, <= 640x480), it plays through the SAME frame-texture/quad/
 * VSync/pacing/drop/skip machinery, with the fps generalized from the AVI
 * header (TimeBetweenFrames) instead of the hardcoded 15; audio chunks
 * (16-bit PCM, mono/stereo, any rate) feed the same 48 kHz ring via
 * xa_xbox.c's Xa_XboxStreamBeginPcm/FeedPcm. Parsing = avi_read.c (C port of
 * pc_port's ReadAVI), decode = nxdk's libjpeg-turbo (jpeg_mem_src). Any
 * missing/unusable AVI falls back to the MDEC/BIN path below (zero-setup
 * default).
 *
 * Deviations from the PC player: no SDL audio device (our XA ring/DirectSound
 * instead), letterbox fits against the fixed 640x480 surface (SH movies are
 * 320x240 = exact fit), and oversized AVIs are rejected to MDEC instead of
 * decoded (640x480 texture cap; JPEG decode at 733 MHz).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

#include <jpeglib.h>        /* nxdk libjpeg-turbo (lib/libjpeg; linked by nxdk core) */

#include "fmv/mdec.h"       /* pc_port/src/fmv (on the include path via -Ipc_port/src) */
#include "fmv/str_demux.h"
#include "avi_read.h"       /* xbox_port/include — C port of pc_port ReadAVI */
#include "gpu_nv2a.h"
#include "sh_log.h"

/* --- HAL externs (C links by name) ------------------------------------------*/
extern void  Cd_XboxInit(void);                    /* cd_xbox.c */
extern FILE* Cd_XboxGetBinFile(void);
extern int   VSync(int mode);                      /* psx_libgpu_xbox.c */
extern unsigned short Pad_XboxButtons(void);       /* pad_xbox.c (active-low) */
extern void  Xa_XboxStreamBegin(void);             /* xa_xbox.c FMV stream mode */
extern void  Xa_XboxStreamFeedSector(const unsigned char* sector2336);
extern void  Xa_XboxStreamBeginPcm(int sampleRate, int stereo);  /* AVI PCM feed */
extern void  Xa_XboxStreamFeedPcm(const short* pcm, int sampleCount);
extern unsigned int Xa_XboxStreamRingFree(void);
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
         * movies are all 320x240 (2 x 300 KB once per session), and the AVI
         * override is capped at 640x480, so worst case one extra grow to
         * 2 x 1.2 MB. */
        s_tex[0] = (uint32_t*)GpuNv2a_AllocTexMem(texBytes);
        s_tex[1] = (uint32_t*)GpuNv2a_AllocTexMem(texBytes);
        s_texCap = (s_tex[0] && s_tex[1]) ? texBytes : 0;
    }
    return (s_rgb != NULL) && (s_tex[0] != NULL) && (s_tex[1] != NULL);
}

/* RGB24 -> A8R8G8B8 into a GPU frame texture; fence the write-combined stores
 * before the GPU reads them (shared by the MDEC and AVI paths). */
static void Fmv_UploadRgb(uint32_t* dst, const uint8_t* src, int pixels)
{
    int i;

    for (i = 0; i < pixels; i++, src += 3)
        dst[i] = 0xFF000000u | ((uint32_t)src[0] << 16)
               | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
    __asm__ __volatile__("sfence" ::: "memory");
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

/* ===========================================================================
 * AVI override path (MJPG video + PCM audio) — PC player parity
 * ===========================================================================*/

/* Hard cap: the frame textures / RGB scratch are sized w*h, and per-frame JPEG
 * decode above 640x480 would not hold pace on the 733 MHz CPU anyway. Bigger
 * AVIs are rejected back to the MDEC path (never crash). */
#define FMV_AVI_MAX_W 640
#define FMV_AVI_MAX_H 480

/* ms offset of frame slot `frames` at tbfUs microseconds per frame (64-bit
 * intermediate: 71 min of 66.7 ms frames stays inside 32 bits after /1000). */
static DWORD Fmv_SlotMs(int frames, int tbfUs)
{
    return (DWORD)(((uint64_t)(unsigned)frames * (unsigned)tbfUs) / 1000u);
}

/* --- libjpeg decode ------------------------------------------------------
 * Port of the PC player's UnpackJPEG (fmv_player.cpp) with one addition: the
 * default libjpeg error manager exit()s the process on a corrupt frame — on
 * Xbox that would bounce to the dashboard, so error_exit longjmps back and
 * the frame is dropped as a decode failure instead. */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
} FmvJpegErr;

static void Fmv_JpegErrorExit(j_common_ptr cinfo)
{
    longjmp(((FmvJpegErr*)cinfo->err)->jb, 1);
}

static void Fmv_JpegOutputMessage(j_common_ptr cinfo)
{
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    SH_DBG("[FMV] jpeg: %s", msg);
}

/* Decode one MJPG frame to tightly-packed RGB24 in dst (capacity maxW*maxH*3).
 * Returns 0 + real dims on success, -1 on decode failure / oversize. */
static int Fmv_UnpackJPEG(unsigned char* src, unsigned src_len,
                          unsigned char* dst, int maxW, int maxH,
                          int* out_w, int* out_h)
{
    struct jpeg_decompress_struct cinfo;
    FmvJpegErr                    jerr;
    unsigned char*                scanline;

    cinfo.err               = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = Fmv_JpegErrorExit;
    jerr.pub.output_message = Fmv_JpegOutputMessage;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    if ((int)cinfo.image_width > maxW || (int)cinfo.image_height > maxH) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    cinfo.out_color_space = JCS_RGB;   /* force RGB24 even for grayscale JPEGs */

    jpeg_start_decompress(&cinfo);
    if (cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    *out_w = (int)cinfo.output_width;
    *out_h = (int)cinfo.output_height;

    for (scanline = dst;
         cinfo.output_scanline < cinfo.output_height;
         scanline += cinfo.output_width * cinfo.output_components)
    {
        jpeg_read_scanlines(&cinfo, &scanline, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* Locate the override: D:\fmv\<name>.avi then D:\gamedata\fmv\<name>.avi
 * (cd_xbox.c's two-location convention; FATX is case-insensitive but the .AVI
 * spelling is tried anyway, PC parity). Returns 1 + path in out. */
static int Fmv_FindAvi(const char* name, char* out, int cap)
{
    static const char* const dirs[] = { "D:\\fmv\\", "D:\\gamedata\\fmv\\" };
    static const char* const exts[] = { ".avi", ".AVI" };
    int d, x;

    for (d = 0; d < 2; d++) {
        for (x = 0; x < 2; x++) {
            FILE* f;
            snprintf(out, (size_t)cap, "%s%s%s", dirs[d], name, exts[x]);
            f = fopen(out, "rb");
            if (f) {
                fclose(f);
                return 1;
            }
        }
    }
    return 0;
}

/* Play an MJPG+PCM AVI through the shared present/pacing/drop/skip machinery.
 * Returns 0 on completion or user skip; -1 when the file is unusable (parse /
 * codec / size / buffers) so the caller falls back to the MDEC path. */
static int Fmv_PlayAvi(const FmvFileEntry* e, int file_idx, const char* path,
                       int max_frames)
{
    avi_reader_t avi;
    int      w, h, tbfUs, fpsX100;
    int      audioOn = 0, audioRate = 0, audioStereo = 0;
    int      done = 0, dropped = 0, presented = 0, consecDrops = 0, decodeFails = 0;
    int      skipArmed = 0, skipped = 0;
    int      texCur = 0, curW = 0, curH = 0;
    DWORD    baseMs = 0;
    int      haveBase = 0;
    unsigned cursor = 0;

    if (!Avi_Open(&avi, path)) {
        SH_DBG("[FMV] avi open/parse failed: %s -> MDEC", path);
        return -1;
    }

    w = avi.vid.image_width;
    h = avi.vid.image_height;

    if (strcmp(avi.vid.compression_type, "MJPG") != 0) {
        SH_DBG("[FMV] avi codec '%s' unsupported (MJPG only): %s -> MDEC",
               avi.vid.compression_type, path);
        Avi_Close(&avi);
        return -1;
    }
    if (w <= 0 || h <= 0 || w > FMV_AVI_MAX_W || h > FMV_AVI_MAX_H) {
        SH_DBG("[FMV] avi %dx%d exceeds %dx%d cap: %s -> MDEC",
               w, h, FMV_AVI_MAX_W, FMV_AVI_MAX_H, path);
        Avi_Close(&avi);
        return -1;
    }
    if (!Fmv_EnsureBuffers(w, h)) {
        SH_DBG("[FMV] avi buffer alloc failed (%dx%d): %s -> MDEC", w, h, path);
        Avi_Close(&avi);
        return -1;
    }

    tbfUs = avi.hdr.TimeBetweenFrames;
    if (tbfUs <= 0)
        tbfUs = 66667;   /* PC parity fallback: 15 fps (PSX STR default) */

    if (avi.auds.samples_per_second > 0 && avi.auds.channels > 0) {
        if (avi.auds.format == 1 && avi.auds.bits_per_sample == 16 &&
            (avi.auds.channels == 1 || avi.auds.channels == 2)) {
            audioOn     = 1;
            audioRate   = avi.auds.samples_per_second;
            audioStereo = (avi.auds.channels == 2);
        } else {
            SH_DBG("[FMV] avi audio unsupported (fmt=%d bits=%d ch=%d) - video only",
                   avi.auds.format, avi.auds.bits_per_sample, avi.auds.channels);
        }
    }

    fpsX100 = (int)(100000000u / (unsigned)tbfUs);
    SH_DBG("[FMV] avi override: %s %dx%d fps=%d.%02d audio=%dhz ch%d",
           path, w, h, fpsX100 / 100, fpsX100 % 100,
           audioOn ? audioRate : 0, audioOn ? avi.auds.channels : 0);
    SH_DBG("[FMV] start %s file_idx=%d avi=1 frames=%d chunks=%u maxFrames=%d",
           e->name, file_idx, avi.hdr.TotalNumberOfFrames, avi.index_count,
           max_frames);

    if (audioOn)
        Xa_XboxStreamBeginPcm(audioRate, audioStereo);

    for (;;) {
        unsigned char* chunk = NULL;
        int            ctype = 0;
        int            len;

        if (skipped) {
            SH_DBG("[FMV] skipped at frame %d", done);
            break;
        }

        len = Avi_NextChunk(&avi, AVI_CTYPE_VIDEO | AVI_CTYPE_AUDIO,
                            &cursor, &ctype, &chunk);
        if (len < 0)
            break;   /* index exhausted (or read error) — movie over */

        if (ctype == AVI_CTYPE_AUDIO) {
            /* PCM chunk -> the 48 kHz ring, in slices so even a badly
             * interleaved (audio-front-loaded) AVI can't overflow the ~680 ms
             * ring: wait for space by re-presenting the current frame (the
             * mixer drains 800 frames per VSync). */
            if (audioOn && len >= 2) {
                int total    = len / 2;                      /* int16 samples */
                int off      = 0;
                int chFactor = audioStereo ? 2 : 1;

                while (off < total && !skipped) {
                    int      slice = total - off;
                    unsigned need;
                    int      guard = 0;

                    if (slice > 8192)
                        slice = 8192;
                    need = (unsigned)(((uint64_t)(unsigned)(slice / chFactor)
                                       * 48000u) / (unsigned)audioRate) + 64u;

                    while (Xa_XboxStreamRingFree() < need && guard < 120) {
                        if (haveBase)
                            Fmv_DrawFrame(s_tex[texCur], curW, curH);
                        VSync(0);
                        if (Fmv_SkipPoll(&skipArmed))
                            skipped = 1;
                        guard++;
                    }
                    Xa_XboxStreamFeedPcm((const short*)chunk + off, slice);
                    off += slice;
                }
            }
            continue;
        }

        /* --- video chunk --- */
        if (max_frames > 0 && done >= max_frames)
            break;
        if (len <= 0)
            continue;

        /* Late? Drop it — decode + present skipped, pacing index still
         * advances so A/V resyncs (same rule as the MDEC path). */
        if (haveBase && consecDrops < FMV_MAX_CONSEC_DROPS &&
            (DWORD)(GetTickCount() - baseMs) >= Fmv_SlotMs(done + 1, tbfUs)) {
            done++;
            dropped++;
            consecDrops++;
            continue;
        }
        consecDrops = 0;

        {
            int fw = 0, fh = 0;

            if (Fmv_UnpackJPEG(chunk, (unsigned)len, s_rgb, w, h, &fw, &fh) != 0 ||
                fw <= 0 || fh <= 0) {
                /* PC parity: emit a black frame so timing still ticks. */
                memset(s_rgb, 0, (size_t)(w * h * 3));
                fw = w;
                fh = h;
                decodeFails++;
            }
            Fmv_UploadRgb(s_tex[texCur ^ 1], s_rgb, fw * fh);
            texCur ^= 1;
            curW = fw;
            curH = fh;
        }

        /* Present, then hold (re-presenting each vblank) until this frame's
         * slot ends — identical sequence to the MDEC path. */
        Fmv_DrawFrame(s_tex[texCur], curW, curH);
        VSync(0);
        presented++;
        if (!haveBase) {
            haveBase = 1;
            baseMs   = GetTickCount();   /* frame 0 visible now */
        }
        if (Fmv_SkipPoll(&skipArmed))
            skipped = 1;

        {
            DWORD slotEnd = baseMs + Fmv_SlotMs(done + 1, tbfUs);
            while (!skipped && (int)(slotEnd - GetTickCount()) > 0) {
                Fmv_DrawFrame(s_tex[texCur], curW, curH);
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
    }

    if (audioOn)
        Xa_XboxStreamEnd();   /* silence: FMV audio out of the mixer immediately */
    Avi_Close(&avi);          /* frees the per-movie index + chunk buffer */

    /* Wait (capped ~1 s) for the skip buttons to release — same phantom-
     * Confirm protection as the MDEC path. */
    {
        int waitVbls = 0;
        while (waitVbls < 60 && Fmv_SkipHeld()) {
            VSync(0);
            waitVbls++;
        }
    }

    SH_DBG("[FMV] end %s (avi): %d frames (%d dropped, %d decode fails, %d presents)",
           e->name, done, dropped, decodeFails, presented);
    return 0;
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

    /* AVI override first (PC parity): a usable D:\fmv\<name>.avi (or
     * D:\gamedata\fmv\<name>.avi) replaces the disc movie; an unusable one
     * (bad parse / codec / size) falls through to the MDEC/BIN path. */
    {
        char aviPath[96];

        if (Fmv_FindAvi(e->name, aviPath, sizeof(aviPath))) {
            if (Fmv_PlayAvi(e, file_idx, aviPath, max_frames) == 0)
                return 0;
        } else {
            static int s_noAviLogs = 0;   /* rate-limit: first 4 fallbacks only */
            if (s_noAviLogs < 4) {
                s_noAviLogs++;
                SH_DBG("[FMV] no avi for %s (checked D:\\fmv, D:\\gamedata\\fmv) -> MDEC",
                       e->name);
            }
        }
    }

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

        /* Upload into the texture NOT bound by recent presents (double
         * buffer; at 15 fps the other one was last read >=4 vblanks ago). */
        Fmv_UploadRgb(s_tex[texCur ^ 1], s_rgb, w * h);
        texCur ^= 1;

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
