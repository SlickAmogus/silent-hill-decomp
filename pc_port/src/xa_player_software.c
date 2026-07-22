#include "xa_player.h"
#include "sh_log.h"
#include "main/fileinfo.h"   /* g_FileXaLoc[] — XA file disc-sector offsets */
#include "pc_config.h"       /* g_PcConfig.cutsceneLineGapMs */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <SDL_timer.h>
#include <PsyX/PsyX_audio.h>
#include <psyq/libspu.h>

/* [XATIME] diagnostic: measure actual wall-clock voice playback duration vs
 * expected, and the gap between consecutive voice fires, to determine whether
 * cutscene dialogue races because voices drain prematurely (bug) or because
 * there is no inter-line pacing (lost PSX CD-load latency). */
static Uint32 s_xaPlayStartMs = 0;
static Uint32 s_xaPrevFireMs  = 0;

/* PSX end-of-voice pacing. PSX had no end-of-XA interrupt: a voice "ended"
 * when the vblank watchdog (sd_call.c, D_800C1688: elapsed > audioLength+32)
 * queued the stop task — i.e. every line held its 'streaming' state for
 * ~0.53s (32 vblanks) PAST the audio, and that pad is the authored
 * inter-line rhythm of every voiced cutscene. Host-side drain fires the moment
 * the samples end, so each line advanced ~0.5s early and long dialogs
 * compressed, running the voices ahead of the scene. Hold the finished
 * signal until the PSX watchdog moment (play start + (length+32)/60 s). */
static Uint32 s_xaPadEndMs = 0;

/* PSX CD-seek inter-line gap. The pause between cutscene voice lines came from
 * BOTH the authored ~J page timers (tuned longer than the voice) AND the CD
 * seek to the next clip. PC honors the ~J timers but loads instantly, so lines
 * whose ~J timer ~= the voice length run together ("talking over each other").
 * Hold the voiced page-advance an extra g_PcConfig.cutsceneLineGapMs past the
 * REAL audio end — set at Play (audio-end estimate + gap). Consumed only by the
 * cutscene page-advance gate (map_msg_display.c pcVoiceHold), and only as a
 * MINIMUM there (the page still requires mapMsgTimer==0), so a line that already
 * has an authored gap >= this is untouched — it does NOT reintroduce the
 * d9a34e548 blanket-pad Flauros desync (that pad extended EVERY line). */
static Uint32 s_xaVoiceGapEndMs = 0;

int PcSoftwareXa_VoiceGapHold(void)
{
    return g_PcConfig.cutsceneLineGapMs > 0 && s_xaVoiceGapEndMs != 0 &&
           SDL_GetTicks() < s_xaVoiceGapEndMs;
}

/* Console-freeze hold: the console zeroes game dt but host audio kept playing,
 * running the voice ahead of the frozen scene. While held, the source is
 * paused and Update does nothing; on release the pad/diagnostic clocks are
 * shifted by the held duration so pacing resumes where it left off. */
static int    s_xaPauseHold    = 0;
static Uint32 s_xaPauseStartMs = 0;

/* Resolved from main_pc.c (where -data sets it). */
extern const char* PcPort_GetGameDiscPath(void);

/* BIN/CUE disc image sector geometry. Each "raw" sector on a PSX BIN/CUE
 * image is 2352 bytes:
 *   12 bytes sync pattern
 *    4 bytes header (min/sec/frame/mode)
 *    8 bytes Mode 2 subheader (file, channel, submode, coding, dup×4)
 * 2316 bytes user data
 *    8 bytes EDC + Q parity (or unused for Form 2)
 * The XA decoder operates on the 2336-byte slice that starts at the
 * subheader (i.e. offset +16 into the raw sector) — matching the layout
 * of extracted XA files (which are pre-extracted disc sectors with the
 * 16-byte sync+header stripped). So to read XA sector K of fileIdx N
 * from the disc image, seek to:
 *   (g_FileXaLoc[N] + K) * 2352 + 16     and read 2336 bytes. */
#define BIN_SECTOR_SIZE     2352
#define BIN_SECTOR_HDR_SIZE 16

// Minimal struct definitions matching decomp layout
typedef struct {
    uint8_t xaFileIdx_0;
    uint8_t pad_1[3];
    uint32_t sector_4_bits : 24;
    uint8_t field_4_24;
    uint32_t audioLength_8_bits : 24;
    uint8_t field_8_24;
} s_XaItemData;

// Externs from decomp
extern s_XaItemData g_XaItemData[727];

// PC wrapper to signal playback finished
extern void Xa_SignalPlaybackFinished(void);
void PcSoftwareXa_Stop(void);

/* Shared disc-image handle for XA streaming. Opened lazily on the first
 * playback (rather than at init) so the player still loads gracefully
 * when there's no disc image (e.g. headless tests). */
static FILE* s_BinFile = NULL;

/* Lazily open <gamedata>/Silent Hill (USA).bin. Returns 1 on success.
 * Idempotent — safe to call before every read. */
static int EnsureBinOpen(void) {
    if (s_BinFile) return 1;

    /* Same disc image main_pc resolved (US or PAL), so XA sector reads line up
     * with the region's g_FileTable / g_FileXaLoc offsets. */
    const char* path = PcPort_GetGameDiscPath();
    if (!path[0]) return 0;

    s_BinFile = fopen(path, "rb");
    if (!s_BinFile) {
        return 0;
    }
    return 1;
}

/* XA-ADPCM filter tables. NOTE: XA has 5 filters (0..4), unlike SPU which has 4.
 * Values from PSX documentation / DuckStation cdrom.cpp DecodeXAADPCMChunks. */
static const int16_t g_FilterPos[16] = {0, 60, 115,  98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const int16_t g_FilterNeg[16] = {0,  0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// XA sector constants
#define XA_SECTOR_SIZE 2336
#define XA_PAYLOAD_OFFSET 8
#define XA_GROUPS_PER_SECTOR 18
#define XA_GROUP_SIZE 128
#define XA_SAMPLES_PER_SECTOR 4032  // 18 * 224 samples total (stereo: 2016 per channel)

#define XA_SECTORS_PER_BUFFER 4
#define XA_SAMPLES_PER_BUFFER (XA_SECTORS_PER_BUFFER * XA_SAMPLES_PER_SECTOR / 2)  // stereo
#define XA_QUEUE_TARGET_FRAMES (XA_SAMPLES_PER_BUFFER * 4)

typedef struct {
    int16_t* pcmBuffer;  // temp decode buffer

    uint16_t xaIdx;
    /* Disc-absolute sector where this XA file's data begins. Pulled from
     * g_FileXaLoc[fileIdx] at Play time. currentSector is XA-file-relative
     * (sector 0 = first sector of the XA file), so the disc offset for a
     * read is (baseSector + currentSector + s) * 2352 + 16. */
    uint32_t baseSector;
    uint32_t totalSectors;
    uint32_t remainingSectors;
    uint32_t currentSector;

    int sampleRate;
    int isStereo;

    /* XA channel multiplex filter: only sectors whose subheader matches
     * (filterFile, filterChannel) belong to the current voice line.
     * Other sectors interleaved on disc carry different channels and
     * must be skipped. Set from the first sector when Play starts. */
    uint8_t filterFile;
    uint8_t filterChannel;

    int isPlaying;
    int finishSignaled;

    /* ADPCM history per channel — int32 to hold UNCLAMPED filter feedback,
     * which is critical for accurate IIR prediction near saturation. */
    int32_t lastSamples[2][2];  /* [channel][prev0=newer, prev1=older] */

} XaPlayerState;

static XaPlayerState g_XaPlayer = {0};

/* Master XA (FMV/voice) volume multiplier in [0,1], from config/console/options. */
extern float g_PcXaVolume;

// Clamp s32 to s16
static int16_t ClampS16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

// Parse XA subheader (byte 3 = coding info)
static void ParseXaSubheader(uint8_t coding, int* outStereo, int* outSampleRate) {
    *outStereo = coding & 1;
    *outSampleRate = (coding >> 2) & 3;  // 0=37800Hz, 1=18900Hz
}

/* Decode 28 ADPCM samples from one sub-block of a sound group.
 *
 * Each sound group is 128 bytes:
 *   header[0..15]: 4 used header bytes repeated 4 times. headers[0..7] = sub-block descriptors.
 *   words[16..127]: 28 LE u32 words. Each word packs 8 nibbles (one per sub-block):
 *     byte 0: low nibble = sb0, high nibble = sb1
 *     byte 1: low nibble = sb2, high nibble = sb3
 *     byte 2: low nibble = sb4, high nibble = sb5
 *     byte 3: low nibble = sb6, high nibble = sb7
 *
 * For 4-bit stereo, sub-blocks alternate L,R,L,R,L,R,L,R (sb=0..7 -> L,R,L,R,L,R,L,R).
 * For 4-bit mono, all sub-blocks feed the single channel sequentially.
 */
static void DecodeSubblock4bit(const uint8_t* group, int sb, int32_t prev[2],
                               int16_t outSamples[28]) {
    const uint8_t* headers = group + 4;        /* 8 header bytes, one per sub-block */
    const uint8_t* words   = group + 16;       /* 28 LE u32 words */

    int shift     = headers[sb] & 0xF;
    int filterIdx = (headers[sb] >> 4) & 0x7;  /* XA: 5 filters (0..4) */
    int16_t fpos  = g_FilterPos[filterIdx];
    int16_t fneg  = g_FilterNeg[filterIdx];
    int byteIdx   = sb >> 1;                   /* which byte in the word holds this sb's nibble */
    int nibShift  = (sb & 1) ? 4 : 0;          /* low or high nibble */

    for (int w = 0; w < 28; w++) {
        uint8_t byte = words[w * 4 + byteIdx];
        int nibble = (byte >> nibShift) & 0xF;

        /* Sign-extend 4-bit nibble into 16-bit, then arithmetic-shift right by `shift`. */
        int32_t sample = ((int16_t)(nibble << 12)) >> shift;

        /* Apply IIR filter with rounding (+32). vgmstream xa_decoder.c:
         * sample = sample + ((coef1 * hist1 + coef2 * hist2 + 32) >> 6); */
        sample += ((int32_t)prev[0] * fpos + (int32_t)prev[1] * fneg + 32) >> 6;

        /* History feeds back the UNCLAMPED value for IIR precision; clamp only on output. */
        prev[1] = prev[0];
        prev[0] = sample;                   /* full int32 precision */
        outSamples[w] = ClampS16(sample);
    }
}

/* Decode one XA sector into a PCM buffer.
 * Returns the number of int16 samples written (stereo = interleaved L/R pairs counted as 2).
 */
static int DecodeXaSector(const uint8_t* sector, int16_t* pcmOut) {
    uint8_t coding = sector[3];
    int isStereo = coding & 1;
    int bitDepth = (coding >> 4) & 1;

    if (bitDepth != 0) {
        return 0;
    }

    int16_t* out = pcmOut;
    int written = 0;

    for (int g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const uint8_t* group = sector + XA_PAYLOAD_OFFSET + g * XA_GROUP_SIZE;

        if (isStereo) {
            /* 8 sub-blocks per group = 4 stereo pairs of 28 samples each.
             * For each pair p (0..3): sb=2p is L, sb=2p+1 is R, sharing 28 stereo positions. */
            for (int p = 0; p < 4; p++) {
                int16_t leftSamples[28], rightSamples[28];
                DecodeSubblock4bit(group, 2 * p,     g_XaPlayer.lastSamples[0], leftSamples);
                DecodeSubblock4bit(group, 2 * p + 1, g_XaPlayer.lastSamples[1], rightSamples);
                for (int s = 0; s < 28; s++) {
                    *out++ = leftSamples[s];
                    *out++ = rightSamples[s];
                }
                written += 56;  /* 28 stereo pairs = 56 ints */
            }
        } else {
            /* Mono: all 8 sub-blocks feed channel 0 sequentially. */
            for (int sb = 0; sb < 8; sb++) {
                int16_t samples[28];
                DecodeSubblock4bit(group, sb, g_XaPlayer.lastSamples[0], samples);
                for (int s = 0; s < 28; s++) *out++ = samples[s];
                written += 28;
            }
        }
    }

    return written;
}

/* Begin streaming XA file `fileIdx` (1..9). Validates the index, ensures
 * the BIN is open, and returns the disc-absolute base sector for the XA
 * file. Returns 0 on failure (caller should bail). g_FileXaLoc[0] is a
 * sentinel zero so a 0 return on success is impossible for valid indices. */
static uint32_t BeginXaStream(int fileIdx) {
    if (fileIdx < 1 || fileIdx > 9) {
        return 0;
    }
    if (!EnsureBinOpen()) {
        return 0;
    }
    uint32_t baseSector = g_FileXaLoc[fileIdx];
    if (baseSector == 0) {
        return 0;
    }
    return baseSector;
}

/* Read one 2336-byte XA logical sector from the BIN. sectorIndex is the
 * sector offset within the XA file (0 = first sector of the file).
 * Returns 1 on success. */
static int ReadXaSectorFromBin(uint32_t baseSector, uint32_t sectorIndex,
                               uint8_t* outBuf /*[XA_SECTOR_SIZE]*/) {
    if (!s_BinFile) return 0;
    long offset = (long)(baseSector + sectorIndex) * BIN_SECTOR_SIZE
                + BIN_SECTOR_HDR_SIZE;
    if (fseek(s_BinFile, offset, SEEK_SET) != 0) {
        return 0;
    }
    return fread(outBuf, 1, XA_SECTOR_SIZE, s_BinFile) == XA_SECTOR_SIZE;
}

/* Calculate the number of MATCHED (this clip's channel) sectors from the
 * authored duration in VSync frames. Per-channel samples per sector: 4032
 * total 4-bit samples, halved for stereo. Samples per 1/60s frame = rate/60.
 * Every voice clip on the USA disc is stereo 37800 (verified by reading all
 * 726 subheaders: sectors = 630*frames/2016), but PAL/JP discs go through
 * this path too, so derive from the parsed format instead of assuming. */
static uint32_t CalculateSectorsFromDuration(uint32_t vyncFrames, int sampleRate, int isStereo) {
    uint32_t samplesPerFrame  = (uint32_t)sampleRate / 60u;
    uint32_t samplesPerSector = isStereo ? 2016u : 4032u;
    uint32_t totalSamples     = samplesPerFrame * vyncFrames;
    return (totalSamples + samplesPerSector - 1) / samplesPerSector; // round up
}

// Initialize playback for a specific XA index
void PcSoftwareXa_Play(uint16_t xaIdx) {
    /* Every rejection must clear the streaming-state flags:
     * Sd_XaAudioPlayTaskAdd sets xaAudioIdx_4/D_800C37DC BEFORE the task
     * pool reaches us, so bailing without the signal leaves
     * Sd_AudioStreamingCheck() == 1 forever — scene scripts waiting for the
     * voice to "finish" stall and Bgm_Init blocks for the rest of the map. */
    if (xaIdx >= 727) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u out of range", xaIdx);
        Xa_SignalPlaybackFinished();
        return;
    }

    s_XaItemData* item = &g_XaItemData[xaIdx];
    uint16_t fileIdx = item->xaFileIdx_0;

    if (fileIdx < 1 || fileIdx > 9) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u out of range (zero g_XaItemData row?)", xaIdx, fileIdx);
        Xa_SignalPlaybackFinished();
        return;
    }


    // Stop any current playback
    if (g_XaPlayer.isPlaying) {
        PcSoftwareXa_Stop();
    }

    // Resolve disc base sector for this XA file (and open the BIN if needed)
    uint32_t baseSector = BeginXaStream(fileIdx);
    if (!baseSector) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u — BeginXaStream failed (disc image?)", xaIdx, fileIdx);
        Xa_SignalPlaybackFinished();
        return;
    }

    // Peek the first sector's subheader to learn the audio format AND the
    // (file, channel) filter — XA streams are multiplexed across many channels.
    uint8_t firstSector[XA_SECTOR_SIZE];
    if (!ReadXaSectorFromBin(baseSector, item->sector_4_bits, firstSector)) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u sector=%u — first-sector read failed", xaIdx, fileIdx, (uint32_t)item->sector_4_bits);
        Xa_SignalPlaybackFinished();
        return;
    }
    const uint8_t* headBuf = firstSector; /* first 8 bytes = subheader */
    int isStereo, srCode;
    ParseXaSubheader(headBuf[3], &isStereo, &srCode);
    int sampleRate = (srCode == 0) ? 37800 : 18900;

    uint8_t filterFile    = headBuf[0];
    uint8_t filterChannel = headBuf[1];


    // Calculate number of sectors to read
    // audioLength_8 is in VSync frames; convert to sectors
    uint32_t numSectors = CalculateSectorsFromDuration(item->audioLength_8_bits, sampleRate, isStereo);

    g_XaPlayer.baseSector = baseSector;
    g_XaPlayer.xaIdx = xaIdx;
    g_XaPlayer.currentSector = item->sector_4_bits;
    g_XaPlayer.totalSectors = numSectors;
    g_XaPlayer.remainingSectors = numSectors;
    g_XaPlayer.sampleRate = sampleRate;
    g_XaPlayer.isStereo = isStereo;
    g_XaPlayer.filterFile = filterFile;
    g_XaPlayer.filterChannel = filterChannel;
    g_XaPlayer.isPlaying = 1;
    g_XaPlayer.finishSignaled = 0;
    {
        Uint32 nowMs = SDL_GetTicks();
        uint32_t expMs = (uint32_t)(((uint64_t)numSectors * (XA_SAMPLES_PER_SECTOR / 2u) * 1000u) / (unsigned)sampleRate);
        SH_DBG("[XATIME] Play xaIdx=%u sectors=%u expMs=%u gapSinceLastFireMs=%u",
               xaIdx, numSectors, expMs, s_xaPrevFireMs ? (nowMs - s_xaPrevFireMs) : 0);
        s_xaPrevFireMs  = nowMs;
        s_xaPlayStartMs = nowMs;
        s_xaPadEndMs    = nowMs + (((uint32_t)item->audioLength_8_bits + 32u) * 1000u) / 60u;
        /* Real audio end (sample-accurate expMs) + the configured inter-line gap;
         * the cutscene page-advance holds until here so the next line's voice
         * doesn't fire back-to-back. */
        s_xaVoiceGapEndMs = nowMs + expMs + (uint32_t)g_PcConfig.cutsceneLineGapMs;
    }
    SH_DBG("[XA] Play xaIdx=%u file=%u sector=%u sectors=%u %s %dHz filter=(%u,%u)",
           xaIdx, fileIdx, (uint32_t)item->sector_4_bits, numSectors,
           isStereo ? "stereo" : "mono", sampleRate, filterFile, filterChannel);
    /* Reset per-track ADPCM filter state. */
    memset(g_XaPlayer.lastSamples, 0, sizeof(g_XaPlayer.lastSamples));

    if (!g_XaPlayer.pcmBuffer) {
        g_XaPlayer.pcmBuffer = malloc(XA_SECTORS_PER_BUFFER * XA_SAMPLES_PER_SECTOR * 2 * sizeof(int16_t));
    }
    PsyX_AudioResetXa();
    PsyX_AudioSetXaMasterGain(g_PcXaVolume);
    SpuSetCommonCDVolume(0x7F00, 0x7F00);

    /* Always reset gain to full at the start of a new track. The game's
     * audio task pool emits a Sd_SetVolXa(0,0) "mute-before-seek" early
     * in gameplay (sd_call.c:1073, inside Sd_XaPreLoadAudio case 0).
     * On PSX the matching restore happens after the seek completes; on
     * PC the seek path is a no-op via PsyCross's CdControl, so the
     * restore never fires and the SPU CD volume stays at zero. Without
     * this reset, every voice
     * line after the first ~20 plays silently (cafe cutscene voices
     * still work because they precede the mute event). */

}

void PcSoftwareXa_Stop(void) {

    if (g_XaPlayer.isPlaying) {
        SH_DBG("[XA] Stop xaIdx=%u (remaining=%u/%u sectors)",
               (unsigned)g_XaPlayer.xaIdx, g_XaPlayer.remainingSectors, g_XaPlayer.totalSectors);
    }

    PsyX_AudioResetXa();

    g_XaPlayer.isPlaying = 0;
    /* Clear all the streaming-state flags that Sd_AudioStreamingCheck consults.
     * Skip when this Stop is the queued-Stop-before-Play in Sd_XaAudioPlayTaskAdd:
     * in that case Sd_TaskPoolExecute case 2 already preserves xaAudioIdx_4
     * for the upcoming Play, and the about-to-fire Play will re-set the flags. */
    Xa_SignalPlaybackFinished();
}

void PcSoftwareXa_PlayWithParams(uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors) {
    // Alternative entry point (not used yet)
    (void)fileIdx;
    (void)sectorOffset;
    (void)numSectors;
    PcSoftwareXa_Play(xaIdx);
}

/* Decode up to XA_SECTORS_PER_BUFFER matching sectors and queue them into the
 * unified software-SPU CD input. Returns source PCM frames queued. */
static int FillAndQueueOne(void) {
    if (g_XaPlayer.remainingSectors == 0) return 0;

    int wantedMatches = (g_XaPlayer.remainingSectors > XA_SECTORS_PER_BUFFER)
                      ? XA_SECTORS_PER_BUFFER : (int)g_XaPlayer.remainingSectors;
    int16_t* pcmPtr = g_XaPlayer.pcmBuffer;
    int totalSamples = 0;
    int matchedCount = 0;
    /* Cap raw scan to avoid runaway if the channel ends prematurely. */
    int scanCap = wantedMatches * 32;

    while (matchedCount < wantedMatches && scanCap-- > 0) {
        uint8_t sectorData[XA_SECTOR_SIZE];
        if (!ReadXaSectorFromBin(g_XaPlayer.baseSector,
                                 g_XaPlayer.currentSector,
                                 sectorData)) {
            g_XaPlayer.remainingSectors = 0;
            break;
        }
        g_XaPlayer.currentSector++;

        /* Skip if this sector doesn't belong to our (file, channel) stream. */
        if (sectorData[0] != g_XaPlayer.filterFile ||
            sectorData[1] != g_XaPlayer.filterChannel) {
            continue;
        }

        int written = DecodeXaSector(sectorData, pcmPtr);
        pcmPtr += written;
        totalSamples += written;
        matchedCount++;
    }

    if (matchedCount == 0) {
        /* Channel ended early (scan cap or EOF without a matching sector).
         * Declare the stream drained so PcSoftwareXa_Update can finish cleanly —
         * leaving remainingSectors nonzero strands isPlaying=1 with an empty
         * queue and the finished signal never fires. */
        g_XaPlayer.remainingSectors = 0;
        return 0;
    }
    g_XaPlayer.remainingSectors -= matchedCount;

    int channels = g_XaPlayer.isStereo ? 2 : 1;
    int frames = totalSamples / channels;
    if (!PsyX_AudioPushXaFrames(g_XaPlayer.pcmBuffer, (uint32_t)frames,
                                (uint32_t)g_XaPlayer.sampleRate, (uint32_t)channels)) {
        return 0;
    }
    return frames;
}

void PcSoftwareXa_Update(void) {
    if (s_xaPauseHold) return;
    if (!g_XaPlayer.isPlaying) return;

    while (g_XaPlayer.remainingSectors > 0 &&
           PsyX_AudioGetQueuedXaFrames() < XA_QUEUE_TARGET_FRAMES) {
        if (FillAndQueueOne() <= 0)
            break;
    }

    if (g_XaPlayer.remainingSectors == 0 && !g_XaPlayer.finishSignaled) {
        g_XaPlayer.finishSignaled = 1;
        PsyX_AudioFinishXa();
    }
    if (g_XaPlayer.remainingSectors != 0 || !PsyX_AudioIsXaDrained())
        return;

    /* PSX pacing: hold the finished signal until the vblank-watchdog
     * moment (see s_xaPadEndMs). An explicit PcSoftwareXa_Stop (skip / next
     * line preempting) still signals immediately. Wrap-safe compare. */
    if ((Sint32)(SDL_GetTicks() - s_xaPadEndMs) < 0) {
        return;
    }
    SH_DBG("[XA] finished xaIdx=%u (drained) playedMs=%u", (unsigned)g_XaPlayer.xaIdx,
           (unsigned)(SDL_GetTicks() - s_xaPlayStartMs));
    g_XaPlayer.isPlaying = 0;
    Xa_SignalPlaybackFinished();
}

/* True only while the voice is ACTUALLY producing audio — the true-drain
 * condition at PcSoftwareXa_Update above, negated, but EXCLUDING the
 * s_xaPadEndMs tail. isPlaying stays 1
 * through the pad window (the isPlaying=0 clear is behind the pad guard), so
 * during the ~490ms pad this returns 0 while Sd_AudioStreamingCheck() still
 * reports 1.
 *
 * The subtitle page-advance gate (pcVoiceHold, map_msg_display.c) uses THIS
 * instead of the padded streaming flag: on PSX pages advanced on the authored
 * ~J page timer alone (no voice gate), so gating on the padded flag added a
 * redundant ~0.5s inter-line gap that accumulated across a voiced cutscene
 * (the map6_s04 Flauros desync). Releasing at real audio drain restores the
 * authored pacing while still preventing PC's instant next-line SD_Call from
 * cutting a genuinely-still-playing voice (the PR#17 anti-overlap fix). The
 * pad itself stays intact for its other consumers (the map6_s04 step-43
 * inter-DMS barrier, BGM transitions). */
int PcSoftwareXa_IsVoiceAudioDraining(void) {
    if (!g_XaPlayer.isPlaying) {
        return 0;
    }
    if (g_XaPlayer.remainingSectors > 0) {
        return 1;
    }
    return !PsyX_AudioIsXaDrained();
}

void PcSoftwareXa_SetPauseHold(int hold) {
    hold = hold ? 1 : 0;
    if (hold == s_xaPauseHold) {
        return;
    }
    s_xaPauseHold = hold;

    if (hold) {
        s_xaPauseStartMs = SDL_GetTicks();
        PsyX_AudioSetXaPaused(1);
    } else {
        Uint32 heldMs = SDL_GetTicks() - s_xaPauseStartMs;

        /* Shift the pacing/diagnostic clocks so the held time doesn't count
         * as playback: the pad watchdog and [XATIME] resume where the freeze
         * began. */
        s_xaPadEndMs    += heldMs;
        s_xaPlayStartMs += heldMs;
        s_xaPrevFireMs  += heldMs;
        s_xaVoiceGapEndMs += heldMs;
        PsyX_AudioSetXaPaused(0);
    }
}

void PcSoftwareXa_SetVolume(int16_t volLeft, int16_t volRight) {
    if (volLeft < 0) volLeft = 0;
    if (volLeft > 127) volLeft = 127;
    if (volRight < 0) volRight = 0;
    if (volRight > 127) volRight = 127;
    SpuSetCommonCDVolume((short)(volLeft << 8), (short)(volRight << 8));
}

/* Set the master XA volume [0,1] and re-apply it to the live source so an
 * in-game options-menu / console change is audible immediately. */
void PcSoftwareXa_SetMasterVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_PcXaVolume = v;
    PsyX_AudioSetXaMasterGain((double)v);
}
