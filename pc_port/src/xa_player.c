#include "xa_player.h"
#include "sh_log.h"
#include "main/fileinfo.h"   /* g_FileXaLoc[] — XA file disc-sector offsets */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL_timer.h>

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
 * inter-line rhythm of every voiced cutscene. OpenAL drain fires the moment
 * the samples end, so each line advanced ~0.5s early and long dialogs
 * compressed, running the voices ahead of the scene. Hold the finished
 * signal until the PSX watchdog moment (play start + (length+32)/60 s). */
static Uint32 s_xaPadEndMs = 0;

/* Console-freeze hold: the console zeroes game dt but OpenAL kept playing,
 * running the voice ahead of the frozen scene. While held, the source is
 * paused and Update does nothing; on release the pad/diagnostic clocks are
 * shifted by the held duration so pacing resumes where it left off. */
static int    s_xaPauseHold    = 0;
static Uint32 s_xaPauseStartMs = 0;

/* Resolved from main_pc.c (where -data sets it). */
extern const char* PcPort_GetGameDataPath(void);
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
 * of disc_extract/XA/*.xa (which are pre-extracted disc sectors with the
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

typedef struct {
    uint16_t cdErrorCount_0;
    uint16_t xaAudioIdxCheck_2;
    uint16_t xaAudioIdx_4;
    // ... rest not needed
} s_Sd_AudioWork;

// Externs from decomp
extern s_XaItemData g_XaItemData[727];

// PC wrapper to signal playback finished
extern void Xa_SignalPlaybackFinished(void);

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
#define XA_SUBHEADER_SIZE 8
#define XA_PAYLOAD_OFFSET 8
#define XA_GROUPS_PER_SECTOR 18
#define XA_GROUP_SIZE 128
#define XA_SAMPLES_PER_SECTOR 4032  // 18 * 224 samples total (stereo: 2016 per channel)

// OpenAL buffer management
#define XA_NUM_BUFFERS 8
#define XA_SECTORS_PER_BUFFER 4
#define XA_SAMPLES_PER_BUFFER (XA_SECTORS_PER_BUFFER * XA_SAMPLES_PER_SECTOR / 2)  // stereo

typedef struct {
    FILE* file;
    ALuint alSource;
    ALuint alBuffers[XA_NUM_BUFFERS];
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
    int bitDepth;

    /* XA channel multiplex filter: only sectors whose subheader matches
     * (filterFile, filterChannel) belong to the current voice line.
     * Other sectors interleaved on disc carry different channels and
     * must be skipped. Set from the first sector when Play starts. */
    uint8_t filterFile;
    uint8_t filterChannel;

    int isPlaying;
    int needsInitialFill;

    /* ADPCM history per channel — int32 to hold UNCLAMPED filter feedback,
     * which is critical for accurate IIR prediction near saturation. */
    int32_t lastSamples[2][2];  /* [channel][prev0=newer, prev1=older] */

    /* Debug: if non-zero, replicate L into R after decode (test if R is broken). */
    int debugForceMono;
} XaPlayerState;

static XaPlayerState g_XaPlayer = {0};

/* Master XA (FMV/voice) volume multiplier in [0,1], from config/console/options
 * menu. Applied on top of the game-driven per-track gain. s_XaGameGain caches
 * the last game-driven gain so a live master-volume change can be re-applied to
 * an already-playing source without waiting for the next Sd_SetVolXa. */
float g_PcXaVolume = 1.0f;
static float s_XaGameGain = 1.0f;

// Clamp s32 to s16
static int16_t ClampS16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

// Parse XA subheader (byte 3 = coding info)
static void ParseXaSubheader(uint8_t coding, int* outStereo, int* outSampleRate, int* outBitDepth) {
    *outStereo = coding & 1;
    *outSampleRate = (coding >> 2) & 3;  // 0=37800Hz, 1=18900Hz
    *outBitDepth = (coding >> 4) & 1;     // 0=4bit, 1=8bit
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

    /* One-time dump of the first group's headers from the very first sector
     * we ever decode in this session — so we can verify shift/filter values. */
    static int s_HasDumped = 0;
    if (!s_HasDumped) {
        s_HasDumped = 1;
        const uint8_t* g0 = sector + XA_PAYLOAD_OFFSET;
        for (int sb = 0; sb < 8; sb++) {
            uint8_t h = (g0 + 4)[sb];
        }
    }

    int16_t* out = pcmOut;
    int written = 0;
    int16_t* outStart = out;

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

    /* Diagnostics: dump min/max/range and a sample mid-sector slice. */
    static int s_DumpedSamples = 0;
    if (!s_DumpedSamples && isStereo && written >= 4032) {
        s_DumpedSamples = 1;
        int16_t lMin=32767, lMax=-32768, rMin=32767, rMax=-32768;
        long long lSum=0, rSum=0;
        for (int i = 0; i < written; i += 2) {
            int16_t lv = outStart[i], rv = outStart[i+1];
            if (lv < lMin) lMin = lv; if (lv > lMax) lMax = lv;
            if (rv < rMin) rMin = rv; if (rv > rMax) rMax = rv;
            lSum += lv; rSum += rv;
        }
        /* Mid-sector slice: stereo pairs at offset 1000 (sample ~T1000 of first sector) */
    }

#ifdef SH_XA_DUMP
    /* DEBUG: dump decoded PCM to a .wav file (first 8 sectors of the very first
     * track). Lets us listen to the raw decoded output independently of OpenAL.
     * Compile with -DSH_XA_DUMP to enable; off in release. */
    static FILE*    s_WavFile      = NULL;
    static int      s_WavSectors   = 0;
    static uint32_t s_WavDataBytes = 0;
    if (s_WavSectors < 8 && isStereo) {
        if (!s_WavFile) {
            s_WavFile = fopen("xa_dump.wav", "wb");
            if (s_WavFile) {
                /* Write a placeholder WAV header (will patch lengths on close). */
                uint8_t hdr[44] = {0};
                memcpy(hdr,    "RIFF", 4);
                memcpy(hdr+8,  "WAVE", 4);
                memcpy(hdr+12, "fmt ", 4);
                hdr[16] = 16;          /* fmt chunk size = 16 */
                hdr[20] = 1;           /* PCM */
                hdr[22] = 2;           /* channels */
                uint32_t sr = (uint32_t)g_XaPlayer.sampleRate;
                memcpy(hdr+24, &sr, 4);
                uint32_t br = sr * 2 * 2;  /* byte rate = sr * channels * bytesPerSample */
                memcpy(hdr+28, &br, 4);
                hdr[32] = 4;           /* block align = channels * bytesPerSample */
                hdr[34] = 16;          /* bits per sample */
                memcpy(hdr+36, "data", 4);
                fwrite(hdr, 1, 44, s_WavFile);
            }
        }
        if (s_WavFile) {
            fwrite(outStart, 1, written * sizeof(int16_t), s_WavFile);
            s_WavDataBytes += written * sizeof(int16_t);
            s_WavSectors++;
            if (s_WavSectors == 8) {
                /* Patch RIFF and data chunk sizes in header. */
                uint32_t dataSize = s_WavDataBytes;
                uint32_t riffSize = dataSize + 36;
                fseek(s_WavFile, 4, SEEK_SET);
                fwrite(&riffSize, 4, 1, s_WavFile);
                fseek(s_WavFile, 40, SEEK_SET);
                fwrite(&dataSize, 4, 1, s_WavFile);
                fclose(s_WavFile);
                s_WavFile = NULL;
            }
        }
    }
#endif

    /* DEBUG: force mono — duplicate L into R channel.
     * If voice becomes clean with this enabled, R-channel decode is broken. */
    if (g_XaPlayer.debugForceMono && isStereo) {
        for (int i = 0; i < written; i += 2) {
            outStart[i+1] = outStart[i];
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
void XaPlayer_Play(uint16_t xaIdx) {
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
        XaPlayer_Stop();
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
    int isStereo, srCode, bitDepth;
    ParseXaSubheader(headBuf[3], &isStereo, &srCode, &bitDepth);
    int sampleRate = (srCode == 0) ? 37800 : 18900;

    uint8_t filterFile    = headBuf[0];
    uint8_t filterChannel = headBuf[1];


    // Calculate number of sectors to read
    // audioLength_8 is in VSync frames; convert to sectors
    uint32_t numSectors = CalculateSectorsFromDuration(item->audioLength_8_bits, sampleRate, isStereo);

    g_XaPlayer.file = s_BinFile;   /* shared — never fclose'd per track */
    g_XaPlayer.baseSector = baseSector;
    g_XaPlayer.xaIdx = xaIdx;
    g_XaPlayer.currentSector = item->sector_4_bits;
    g_XaPlayer.totalSectors = numSectors;
    g_XaPlayer.remainingSectors = numSectors;
    g_XaPlayer.sampleRate = sampleRate;
    g_XaPlayer.isStereo = isStereo;
    g_XaPlayer.bitDepth = bitDepth;
    g_XaPlayer.filterFile = filterFile;
    g_XaPlayer.filterChannel = filterChannel;
    g_XaPlayer.isPlaying = 1;
    g_XaPlayer.needsInitialFill = 1;
    {
        Uint32 nowMs = SDL_GetTicks();
        uint32_t expMs = (uint32_t)(((uint64_t)numSectors * (XA_SAMPLES_PER_SECTOR / 2u) * 1000u) / (unsigned)sampleRate);
        SH_DBG("[XATIME] Play xaIdx=%u sectors=%u expMs=%u gapSinceLastFireMs=%u",
               xaIdx, numSectors, expMs, s_xaPrevFireMs ? (nowMs - s_xaPrevFireMs) : 0);
        s_xaPrevFireMs  = nowMs;
        s_xaPlayStartMs = nowMs;
        s_xaPadEndMs    = nowMs + (((uint32_t)item->audioLength_8_bits + 32u) * 1000u) / 60u;
    }
    SH_DBG("[XA] Play xaIdx=%u file=%u sector=%u sectors=%u %s %dHz filter=(%u,%u)",
           xaIdx, fileIdx, (uint32_t)item->sector_4_bits, numSectors,
           isStereo ? "stereo" : "mono", sampleRate, filterFile, filterChannel);
    /* Reset per-track ADPCM filter state. */
    memset(g_XaPlayer.lastSamples, 0, sizeof(g_XaPlayer.lastSamples));
    /* Mono replication test confirmed not the issue — leave off. */
    g_XaPlayer.debugForceMono = 0;

    // Create OpenAL source/buffers once
    if (!g_XaPlayer.alSource) {
        alGenSources(1, &g_XaPlayer.alSource);
        alGenBuffers(XA_NUM_BUFFERS, g_XaPlayer.alBuffers);
        g_XaPlayer.pcmBuffer = malloc(XA_SECTORS_PER_BUFFER * XA_SAMPLES_PER_SECTOR * 2 * sizeof(int16_t));
    }

    /* Always reset gain to full at the start of a new track. The game's
     * audio task pool emits a Sd_SetVolXa(0,0) "mute-before-seek" early
     * in gameplay (sd_call.c:1073, inside Sd_XaPreLoadAudio case 0).
     * On PSX the matching restore happens after the seek completes; on
     * PC the seek path is a no-op via PsyCross's CdControl, so the
     * restore never fires and the OpenAL source gain stays stuck at
     * 0.0 for the rest of the session. Without this reset, every voice
     * line after the first ~20 plays silently (cafe cutscene voices
     * still work because they precede the mute event). */
    s_XaGameGain = 1.0f;
    alSourcef(g_XaPlayer.alSource, AL_GAIN, s_XaGameGain * g_PcXaVolume);

}

void XaPlayer_Stop(void) {

    if (g_XaPlayer.isPlaying) {
        SH_DBG("[XA] Stop xaIdx=%u (remaining=%u/%u sectors)",
               (unsigned)g_XaPlayer.xaIdx, g_XaPlayer.remainingSectors, g_XaPlayer.totalSectors);
    }

    if (g_XaPlayer.alSource) {
        alSourceStop(g_XaPlayer.alSource);
        /* Detach all buffers from the source so the next Play starts with a
         * clean queue. Without this, leftover buffers from the previous track
         * play before the new audio (audible as "tail of previous line"). */
        alSourcei(g_XaPlayer.alSource, AL_BUFFER, 0);
    }

    g_XaPlayer.isPlaying = 0;
    /* Clear all the streaming-state flags that Sd_AudioStreamingCheck consults.
     * Skip when this Stop is the queued-Stop-before-Play in Sd_XaAudioPlayTaskAdd:
     * in that case Sd_TaskPoolExecute case 2 already preserves xaAudioIdx_4
     * for the upcoming Play, and the about-to-fire Play will re-set the flags. */
    Xa_SignalPlaybackFinished();
}

// Fill a single OpenAL buffer with decoded XA data
static void FillBuffer(ALuint buffer) {
    if (!g_XaPlayer.remainingSectors) {
        return;
    }

    // Decode sectors into PCM
    int16_t* pcmPtr = g_XaPlayer.pcmBuffer;
    int sectorsThisBuffer = (g_XaPlayer.remainingSectors > XA_SECTORS_PER_BUFFER) ?
                            XA_SECTORS_PER_BUFFER : g_XaPlayer.remainingSectors;

    for (int s = 0; s < sectorsThisBuffer; s++) {
        uint8_t sectorData[XA_SECTOR_SIZE];
        if (!ReadXaSectorFromBin(g_XaPlayer.baseSector,
                                 g_XaPlayer.currentSector + s,
                                 sectorData)) {
            g_XaPlayer.isPlaying = 0;
            return;
        }

        // Decode
        DecodeXaSector(sectorData, pcmPtr);
        pcmPtr += XA_SAMPLES_PER_SECTOR * sizeof(int16_t);
    }

    // Queue buffer to OpenAL
    int sampleBytes = sectorsThisBuffer * XA_SAMPLES_PER_SECTOR * sizeof(int16_t);
    alBufferData(buffer, AL_FORMAT_STEREO16, g_XaPlayer.pcmBuffer,
                 sampleBytes, g_XaPlayer.sampleRate);
    alSourceQueueBuffers(g_XaPlayer.alSource, 1, &buffer);

    g_XaPlayer.currentSector += sectorsThisBuffer;
    g_XaPlayer.remainingSectors -= sectorsThisBuffer;
}

void XaPlayer_PlayWithParams(uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors) {
    // Alternative entry point (not used yet)
    XaPlayer_Play(xaIdx);
}

/* Decode up to XA_SECTORS_PER_BUFFER MATCHING sectors (skipping interleaved
 * sectors of other channels) into the PCM scratch buffer and upload to a
 * given AL buffer. Returns total int16 samples written. */
static int FillAndUploadOne(ALuint alBuffer) {
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
         * Declare the stream drained so XaPlayer_Update can finish cleanly —
         * leaving remainingSectors nonzero strands isPlaying=1 with an empty
         * queue and the finished signal never fires. */
        g_XaPlayer.remainingSectors = 0;
        return 0;
    }
    g_XaPlayer.remainingSectors -= matchedCount;

    int byteCount = totalSamples * (int)sizeof(int16_t);
    ALenum format = g_XaPlayer.isStereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    alBufferData(alBuffer, format, g_XaPlayer.pcmBuffer, byteCount, g_XaPlayer.sampleRate);
    alSourceQueueBuffers(g_XaPlayer.alSource, 1, &alBuffer);

    return totalSamples;
}

void XaPlayer_Update(void) {
    if (s_xaPauseHold) return;
    if (!g_XaPlayer.isPlaying) return;

    /* On first Update after Play: queue all buffers fresh (none are
     * processed because nothing has been queued or played yet). */
    if (g_XaPlayer.needsInitialFill) {
        int queued = 0;
        for (int i = 0; i < XA_NUM_BUFFERS && g_XaPlayer.remainingSectors > 0; i++) {
            if (FillAndUploadOne(g_XaPlayer.alBuffers[i]) > 0) queued++;
        }
        g_XaPlayer.needsInitialFill = 0;

        if (queued > 0) {
            alSourcePlay(g_XaPlayer.alSource);
        }
        return;
    }

    /* Refill any buffers that have finished playing back. */
    ALint processed = 0;
    alGetSourcei(g_XaPlayer.alSource, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0 && g_XaPlayer.remainingSectors > 0) {
        ALuint buf;
        alSourceUnqueueBuffers(g_XaPlayer.alSource, 1, &buf);
        FillAndUploadOne(buf);
        processed--;
    }

    /* If the source underran (e.g. paused mid-stream), re-kick. */
    ALint sourceState = 0;
    alGetSourcei(g_XaPlayer.alSource, AL_SOURCE_STATE, &sourceState);
    if (sourceState != AL_PLAYING && g_XaPlayer.remainingSectors > 0) {
        alSourcePlay(g_XaPlayer.alSource);
    }

    /* Detect end-of-playback: no more sectors AND source not playing.
     * AL_INITIAL counts too — if the initial fill found zero matching
     * sectors the source never started and would never reach AL_STOPPED. */
    if (g_XaPlayer.remainingSectors == 0 && sourceState != AL_PLAYING) {
        /* PSX pacing: hold the finished signal until the vblank-watchdog
         * moment (see s_xaPadEndMs). An explicit XaPlayer_Stop (skip / next
         * line preempting) still signals immediately. Wrap-safe compare. */
        if ((Sint32)(SDL_GetTicks() - s_xaPadEndMs) < 0) {
            return;
        }
        SH_DBG("[XA] finished xaIdx=%u (drained) playedMs=%u", (unsigned)g_XaPlayer.xaIdx,
               (unsigned)(SDL_GetTicks() - s_xaPlayStartMs));
        g_XaPlayer.isPlaying = 0;
        /* g_XaPlayer.file aliases the shared s_BinFile — never fclose it
         * here. The BIN handle is held for the lifetime of the process. */
        g_XaPlayer.file = NULL;
        Xa_SignalPlaybackFinished();
    }
}

void XaPlayer_SetPauseHold(int hold) {
    hold = hold ? 1 : 0;
    if (hold == s_xaPauseHold) {
        return;
    }
    s_xaPauseHold = hold;

    if (hold) {
        s_xaPauseStartMs = SDL_GetTicks();
        if (g_XaPlayer.isPlaying && g_XaPlayer.alSource) {
            alSourcePause(g_XaPlayer.alSource);
        }
    } else {
        Uint32 heldMs = SDL_GetTicks() - s_xaPauseStartMs;

        /* Shift the pacing/diagnostic clocks so the held time doesn't count
         * as playback: the pad watchdog and [XATIME] resume where the freeze
         * began. */
        s_xaPadEndMs    += heldMs;
        s_xaPlayStartMs += heldMs;
        s_xaPrevFireMs  += heldMs;
        if (g_XaPlayer.isPlaying && g_XaPlayer.alSource) {
            ALint st = 0;
            alGetSourcei(g_XaPlayer.alSource, AL_SOURCE_STATE, &st);
            if (st == AL_PAUSED) {
                alSourcePlay(g_XaPlayer.alSource);
            }
        }
    }
}

void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight) {
    if (!g_XaPlayer.alSource) {
        return;
    }

    /* Match PSX scaling: (vol * globalVolumeXa_E) >> 7 then map 0..127 to 0..1 OpenAL gain.
     * Per Sd_SetVolXa in sd_call.c. Without globalVolumeXa_E here we approximate
     * by treating the input vol as the already-scaled value. Divide by 127 to
     * normalize, NOT by 84 which overdrove voices well past max gain. */
    int vol = (volLeft + volRight) / 2;
    if (vol < 0) vol = 0;
    float gain = (float)vol / 127.0f;
    if (gain > 1.0f) gain = 1.0f;
    s_XaGameGain = gain;
    alSourcef(g_XaPlayer.alSource, AL_GAIN, gain * g_PcXaVolume);

}

/* Set the master XA volume [0,1] and re-apply it to the live source so an
 * in-game options-menu / console change is audible immediately. */
void XaPlayer_SetMasterVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_PcXaVolume = v;
    if (g_XaPlayer.alSource) {
        alSourcef(g_XaPlayer.alSource, AL_GAIN, s_XaGameGain * g_PcXaVolume);
    }
}
