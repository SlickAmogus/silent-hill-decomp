/*
 * xa_xbox.c - CD-XA streaming audio (voice acting + streamed cutscene/ambient
 * audio) for the Silent Hill Xbox port.
 *
 * SH1 plays voice lines as XA-ADPCM sectors interleaved in the disc image —
 * NOT via the SPU/VAB path (audio_xbox.c). The game drives playback through
 * the XaPlayer_* surface in pc_port/src/xa_player.h:
 *   - Sd_TaskPoolExecute case 1/2 (sd_call.c) -> XaPlayer_Play / XaPlayer_Stop
 *   - Sd_SetVolXa (sd_call.c)                 -> XaPlayer_SetVolume
 *   - MainLoop (game_main.c, once per frame)  -> XaPlayer_Update
 * and every finish/stop/reject MUST call Xa_SignalPlaybackFinished() (defined
 * in sd_call.c under SH_PC_PORT) or Sd_AudioStreamingCheck() reports "busy"
 * forever and scene scripts / Bgm_Init stall.
 *
 * Port of the proven pc_port/src/xa_player.c (OpenAL) onto the Xbox HAL:
 *   - RAW 2352-byte sector reads from the BIN via Cd_XboxReadRaw (cd_xbox.c);
 *     the XA logical sector is the 2336-byte slice at +16 (8-byte subheader =
 *     file/channel/submode/codinginfo, then 18 x 128-byte sound groups).
 *   - (file, channel) filter: XA streams are multiplexed — sectors of other
 *     channels interleaved on disc are skipped (typically every 4th sector
 *     belongs to the current voice at 37800 Hz stereo).
 *   - XA-ADPCM decode (4-bit, 5 filters, unclamped IIR history) is the PC
 *     implementation verbatim — already integer-only.
 *   - Output: decoded PCM is resampled (37800/18900 -> 48000 Hz, linear
 *     interp, Q16 phase) into a ring of stereo frames; the software SPU mixer
 *     (audio_xbox.c Audio_RenderInto) adds one frame per output sample via
 *     Xa_XboxMixInto with the XaPlayer volume applied, BEFORE the master
 *     volume stage — so XA obeys the game's master volume exactly like the
 *     24 SPU voices, and there is only ONE DirectSound buffer.
 *   - Decode+refill stays on the main thread: Xa_XboxPump runs from
 *     XaPlayer_Update (MainLoop) and from Audio_XboxPump (VSync), keeping
 *     ~400 ms decoded ahead of the mixer.
 */
#include <string.h>
#include <stdint.h>

#include "xa_player.h"   /* the exact prototypes sd_call.c/game_main.c compile against */
#include "sh_log.h"

/* --- externs ---------------------------------------------------------------*/

/* cd_xbox.c: BIN disc image access. Init is idempotent; ReadRaw reads full
 * 2352-byte sectors at LBA `lbn` (returns 1 on success, 0 on short read). */
extern void Cd_XboxInit(void);
extern int  Cd_XboxReadRaw(unsigned int lbn, unsigned char* buf, int sectors);

/* sd_call.c (SH_PC_PORT): clears xaAudioIdx_4 / isXaStopping_13 / D_800C37DC. */
extern void Xa_SignalPlaybackFinished(void);

/* main/fileinfo.c: disc-LBA of each XA container file (index 1..9), filled by
 * Fs_InitFileTableForRegion (called from main_xbox.c before MainLoop). */
extern unsigned int g_FileXaLoc[];

/* Mirror of the decomp's s_XaItemData (sound_system.h) — same local copy the
 * PC player uses. GNU bitfield layout (-mno-ms-bitfields) => 12 bytes:
 * fileIdx @0, sector @4..6, group idx @7, length @8..10, group @11. */
typedef struct {
    uint8_t  xaFileIdx_0;
    uint8_t  pad_1[3];
    uint32_t sector_4_bits : 24;
    uint8_t  field_4_24;
    uint32_t audioLength_8_bits : 24;
    uint8_t  field_8_24;
} s_XaItemData;

extern s_XaItemData g_XaItemData[727];

/* --- XA sector geometry ----------------------------------------------------*/

#define BIN_SECTOR_SIZE      2352
#define XA_SLICE_OFFSET      16     /* raw sector: 12 sync + 4 header, subheader next */
#define XA_SECTOR_SIZE       2336   /* subheader (8) + payload */
#define XA_PAYLOAD_OFFSET    8
#define XA_GROUPS_PER_SECTOR 18
#define XA_GROUP_SIZE        128
#define XA_MAX_SECTOR_SAMPLES 4032  /* int16 count (stereo: 2016 L/R frames) */

/* XA-ADPCM filter tables — XA has 5 filters (0..4), unlike SPU's 4. */
static const int16_t g_FilterPos[8] = {0, 60, 115,  98, 122, 0, 0, 0};
static const int16_t g_FilterNeg[8] = {0,  0, -52, -55, -60, 0, 0, 0};

/* --- output ring (decoded + resampled 48 kHz stereo frames) ----------------*/

#define OUT_HZ                48000
#define XA_RING_FRAMES        32768u          /* power of two; ~682 ms at 48 kHz */
#define XA_RING_MASK          (XA_RING_FRAMES - 1u)
#define XA_TARGET_FRAMES      19200u          /* keep ~400 ms decoded ahead */
#define XA_PUMP_MAX_SECTORS   12              /* per-pump decode cap */
#define XA_SCAN_CAP_PER_MATCH 32              /* raw sectors scanned per matching one */

static short    s_ring[XA_RING_FRAMES * 2];   /* interleaved L/R, 128 KB BSS */
static uint32_t s_ringRead  = 0;              /* monotonic frame counters; */
static uint32_t s_ringWrite = 0;              /* count = write - read       */

static int16_t  s_sectorPcm[XA_MAX_SECTOR_SAMPLES];

/* --- player state ---------------------------------------------------------*/

typedef struct {
    uint16_t xaIdx;
    uint32_t baseSector;        /* disc LBA where the XA container begins */
    uint32_t currentSector;     /* container-relative raw scan cursor */
    uint32_t totalSectors;      /* matching sectors wanted (duration) */
    uint32_t remainingSectors;

    int      sampleRate;        /* 37800 or 18900 */
    int      isStereo;
    uint8_t  filterFile;
    uint8_t  filterChannel;

    int      isPlaying;

    /* ADPCM history per channel — int32 for UNCLAMPED IIR feedback. */
    int32_t  lastSamples[2][2];

    /* 37800/18900 -> 48000 linear-interp resampler (Q16 phase). */
    uint32_t rsStep;            /* srcHz * 65536 / OUT_HZ */
    uint32_t rsPhase;
    int      rsPrevL, rsPrevR;

    /* Per-side gain, Q7 (128 = unity). */
    int      volQ7L, volQ7R;
} XaState;

static XaState s_xa;

/* Underrun bookkeeping (silence emitted while sectors were still expected). */
static int      s_inUnderrun        = 0;
static uint32_t s_underrunFrames    = 0;
static uint32_t s_underrunEpisodes  = 0;
static uint32_t s_underrunLogged    = 0;

/* --- XA-ADPCM decode (ported verbatim from pc_port/src/xa_player.c) --------*/

static int16_t ClampS16(int32_t val)
{
    if (val > 32767)  return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

/* Decode 28 samples from one sub-block of a 128-byte sound group.
 * header bytes [4..11] = 8 sub-block descriptors; words [16..127] = 28 LE u32,
 * each packing one nibble per sub-block. */
static void DecodeSubblock4bit(const uint8_t* group, int sb, int32_t prev[2],
                               int16_t outSamples[28])
{
    const uint8_t* headers = group + 4;
    const uint8_t* words   = group + 16;

    int     shift     = headers[sb] & 0xF;
    int     filterIdx = (headers[sb] >> 4) & 0x7;   /* XA: 5 filters (0..4) */
    int16_t fpos      = g_FilterPos[filterIdx];
    int16_t fneg      = g_FilterNeg[filterIdx];
    int     byteIdx   = sb >> 1;
    int     nibShift  = (sb & 1) ? 4 : 0;
    int     w;

    for (w = 0; w < 28; w++) {
        int     nibble = (words[w * 4 + byteIdx] >> nibShift) & 0xF;
        int32_t sample = ((int16_t)(nibble << 12)) >> shift;

        sample += ((int32_t)prev[0] * fpos + (int32_t)prev[1] * fneg + 32) >> 6;

        /* History feeds back UNCLAMPED for IIR precision; clamp only on output. */
        prev[1] = prev[0];
        prev[0] = sample;
        outSamples[w] = ClampS16(sample);
    }
}

/* Decode one 2336-byte XA sector into pcmOut. Returns int16 samples written
 * (stereo interleaved L/R counted as 2). 8-bit XA (unused by SH) yields 0. */
static int DecodeXaSector(const uint8_t* sector, int16_t* pcmOut)
{
    uint8_t coding   = sector[3];
    int     isStereo = coding & 1;
    int     bitDepth = (coding >> 4) & 1;
    int     written  = 0;
    int16_t* out     = pcmOut;
    int     g;

    if (bitDepth != 0)
        return 0;

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const uint8_t* group = sector + XA_PAYLOAD_OFFSET + g * XA_GROUP_SIZE;

        if (isStereo) {
            /* 8 sub-blocks = 4 stereo pairs of 28 samples (sb even=L, odd=R). */
            int p, s;
            for (p = 0; p < 4; p++) {
                int16_t leftSamples[28], rightSamples[28];
                DecodeSubblock4bit(group, 2 * p,     s_xa.lastSamples[0], leftSamples);
                DecodeSubblock4bit(group, 2 * p + 1, s_xa.lastSamples[1], rightSamples);
                for (s = 0; s < 28; s++) {
                    *out++ = leftSamples[s];
                    *out++ = rightSamples[s];
                }
                written += 56;
            }
        } else {
            /* Mono: all 8 sub-blocks feed channel 0 sequentially. */
            int sb, s;
            for (sb = 0; sb < 8; sb++) {
                int16_t samples[28];
                DecodeSubblock4bit(group, sb, s_xa.lastSamples[0], samples);
                for (s = 0; s < 28; s++)
                    *out++ = samples[s];
                written += 28;
            }
        }
    }
    return written;
}

/* --- resample + ring push --------------------------------------------------*/

/* Resample one decoded sector's PCM (srcHz, mono or stereo) to 48 kHz stereo
 * frames and append to the ring. Q16 phase; (diff * phase>>1) >> 15 keeps the
 * interp product inside int32 (65535 * 32767 < 2^31). The pump guarantees
 * ring space before decoding, so the in-loop space check is only a guard. */
static void PushResampled(const int16_t* pcm, int sampleCount, int isStereo)
{
    int frames = isStereo ? (sampleCount / 2) : sampleCount;
    int i;

    for (i = 0; i < frames; i++) {
        int curL = isStereo ? pcm[i * 2]     : pcm[i];
        int curR = isStereo ? pcm[i * 2 + 1] : curL;

        while (s_xa.rsPhase < 0x10000u) {
            if ((s_ringWrite - s_ringRead) < XA_RING_FRAMES) {
                uint32_t idx = (s_ringWrite & XA_RING_MASK) * 2;
                int      ph  = (int)(s_xa.rsPhase >> 1);        /* 0..0x7FFF */
                s_ring[idx]     = (short)(s_xa.rsPrevL + (((curL - s_xa.rsPrevL) * ph) >> 15));
                s_ring[idx + 1] = (short)(s_xa.rsPrevR + (((curR - s_xa.rsPrevR) * ph) >> 15));
                s_ringWrite++;
            }
            s_xa.rsPhase += s_xa.rsStep;
        }
        s_xa.rsPhase -= 0x10000u;
        s_xa.rsPrevL = curL;
        s_xa.rsPrevR = curR;
    }
}

/* --- sector streaming pump -------------------------------------------------*/

/* Decode matching sectors (skipping other channels' interleaved sectors) until
 * the ring holds ~400 ms, the duration countdown hits 0, or the per-call cap
 * is reached. Main thread only — called from XaPlayer_Update (MainLoop) and
 * Audio_XboxPump (VSync). */
void Xa_XboxPump(void)
{
    int decoded = 0;

    if (!s_xa.isPlaying)
        return;

    while (s_xa.remainingSectors > 0 && decoded < XA_PUMP_MAX_SECTORS) {
        uint8_t raw[BIN_SECTOR_SIZE];
        int     scan    = XA_SCAN_CAP_PER_MATCH;
        int     matched = 0;

        if ((s_ringWrite - s_ringRead) >= XA_TARGET_FRAMES)
            break;   /* enough decoded ahead; ring free space (>13k frames)
                      * always exceeds one sector's worst case (~10.3k) */

        while (scan-- > 0) {
            const uint8_t* xs;

            if (!Cd_XboxReadRaw(s_xa.baseSector + s_xa.currentSector, raw, 1)) {
                SH_DBG("[XA] read FAIL at sector=%u (base=%u) -> drain",
                       s_xa.currentSector, s_xa.baseSector);
                s_xa.remainingSectors = 0;
                break;
            }
            s_xa.currentSector++;

            xs = raw + XA_SLICE_OFFSET;
            if (xs[0] == s_xa.filterFile && xs[1] == s_xa.filterChannel) {
                int n = DecodeXaSector(xs, s_sectorPcm);
                if (n > 0)
                    PushResampled(s_sectorPcm, n, s_xa.isStereo);
                matched = 1;
                break;
            }
        }

        if (s_xa.remainingSectors == 0)
            break;

        if (!matched) {
            /* Channel ended early (scan cap without a match). Declare drained
             * so Update can finish cleanly — same as the PC player. */
            SH_DBG("[XA] channel ended early xaIdx=%u (remaining=%u/%u)",
                   (unsigned)s_xa.xaIdx, s_xa.remainingSectors, s_xa.totalSectors);
            s_xa.remainingSectors = 0;
            break;
        }

        s_xa.remainingSectors--;
        decoded++;
    }
}

/* --- FMV audio stream mode (fmv_xbox.c) --------------------------------------
 * STR movies interleave XA audio sectors among the MDEC video sectors; the FMV
 * demuxer forwards each one here as a 2336-byte logical sector (subheader at
 * +0). Reuses the same ADPCM decoder + resampler + ring, but BYPASSES the
 * XaPlayer/Sd_* bookkeeping entirely (no Xa_SignalPlaybackFinished) — the PC
 * player's FMV audio goes straight to SDL the same way. Main-thread only, and
 * mutually exclusive with XaPlayer playback: the game is blocked inside
 * FMV_Play for the whole stream, and StreamBegin stops any live voice line
 * (on PSX the movie owns the CD drive, killing XA playback anyway). */
static int          s_fmvStream       = 0;
static int          s_fmvStreamStereo = 1;
static int          s_fmvStreamOpened = 0;   /* first-sector format latch */
static unsigned int s_fmvSectorsFed   = 0;

void Xa_XboxStreamBegin(void)
{
    if (s_xa.isPlaying)
        XaPlayer_Stop();

    memset(&s_xa.lastSamples, 0, sizeof(s_xa.lastSamples));
    s_xa.rsPhase = 0;
    s_xa.rsPrevL = 0;
    s_xa.rsPrevR = 0;
    s_xa.rsStep  = (37800u * 65536u) / OUT_HZ;  /* re-latched from the first sector */
    s_xa.volQ7L  = 128;                          /* FMV audio at unity (PC parity) */
    s_xa.volQ7R  = 128;

    s_ringRead        = 0;
    s_ringWrite       = 0;
    s_fmvStream       = 1;
    s_fmvStreamOpened = 0;
    s_fmvStreamStereo = 1;
    s_fmvSectorsFed   = 0;
}

/* Decode one 2336-byte XA sector into the ring. If the ring is too full the
 * resampler's in-loop guard drops the overflow — the FMV pacing caps
 * consecutive video-frame drops precisely so this never triggers. */
void Xa_XboxStreamFeedSector(const unsigned char* xs)
{
    int n;

    if (!s_fmvStream || xs == 0)
        return;

    if (!s_fmvStreamOpened) {
        uint8_t coding = xs[3];
        int     srCode = (coding >> 2) & 3;
        int     rate   = (srCode == 0) ? 37800 : 18900;

        s_fmvStreamStereo = coding & 1;
        s_xa.rsStep       = ((uint32_t)rate * 65536u) / OUT_HZ;
        s_fmvStreamOpened = 1;
        SH_DBG("[XA] fmv stream open: %dHz %s coding=0x%02x",
               rate, s_fmvStreamStereo ? "stereo" : "mono", (unsigned)coding);
    }

    n = DecodeXaSector(xs, s_sectorPcm);
    if (n > 0)
        PushResampled(s_sectorPcm, n, s_fmvStreamStereo);
    s_fmvSectorsFed++;
}

/* --- FMV AVI-override PCM feed (fmv_xbox.c) -----------------------------------
 * Same stream mode / ring / resampler as the XA sector feed above, but fed
 * with raw 16-bit PCM from the AVI '##wb' chunks — the format is declared by
 * the AVI header instead of latched from a sector subheader. Ends via the
 * shared Xa_XboxStreamEnd. */
void Xa_XboxStreamBeginPcm(int sampleRate, int stereo)
{
    Xa_XboxStreamBegin();
    s_fmvStreamStereo = stereo ? 1 : 0;
    if (sampleRate < 4000)
        sampleRate = 4000;   /* floor keeps rsStep sane on a garbage header */
    s_xa.rsStep       = ((uint32_t)sampleRate * 65536u) / OUT_HZ;
    s_fmvStreamOpened = 1;   /* no sector-subheader latch in PCM mode */
    SH_DBG("[XA] fmv pcm stream open: %dHz %s", sampleRate, stereo ? "stereo" : "mono");
}

/* sampleCount = int16 sample count (stereo: interleaved L/R counted as 2 —
 * same convention as DecodeXaSector). Overflow beyond the ring is dropped by
 * PushResampled's guard; the FMV player checks Xa_XboxStreamRingFree first. */
void Xa_XboxStreamFeedPcm(const short* pcm, int sampleCount)
{
    if (!s_fmvStream || pcm == 0 || sampleCount <= 0)
        return;
    PushResampled((const int16_t*)pcm, sampleCount, s_fmvStreamStereo);
    s_fmvSectorsFed++;   /* chunk-granular here; same progress counter */
}

unsigned int Xa_XboxStreamRingFree(void)
{
    return s_fmvStream ? (XA_RING_FRAMES - (s_ringWrite - s_ringRead)) : 0;
}

void Xa_XboxStreamEnd(void)
{
    if (s_fmvStream)
        SH_DBG("[XA] fmv stream end: %u sectors fed, %u ring frames discarded",
               s_fmvSectorsFed, s_ringWrite - s_ringRead);
    s_fmvStream = 0;
    s_ringRead  = 0;
    s_ringWrite = 0;
}

unsigned int Xa_XboxStreamRingLevel(void)  { return s_fmvStream ? (s_ringWrite - s_ringRead) : 0; }
unsigned int Xa_XboxStreamSectorsFed(void) { return s_fmvSectorsFed; }

/* --- mixer hook (audio_xbox.c Audio_RenderInto) -----------------------------*/

/* Add one 48 kHz frame of XA into the L/R accumulators, XA volume applied.
 * Runs BEFORE the master-volume stage in Audio_RenderInto, so XA respects the
 * game's master volume like the SPU voices. No-op when idle; while playing an
 * empty ring emits silence and counts as underrun (unless it's the end-drain). */
void Xa_XboxMixInto(int* accL, int* accR)
{
    uint32_t idx;

    if (!s_xa.isPlaying && !s_fmvStream)
        return;

    if (s_ringRead == s_ringWrite) {
        /* Underrun bookkeeping is XaPlayer-only; an empty ring in FMV stream
         * mode just means the demuxer hasn't reached the next audio sector
         * yet (or the movie's audio ended) — emit silence, no stats. */
        if (s_xa.isPlaying && s_xa.remainingSectors > 0) {
            if (!s_inUnderrun) {
                s_inUnderrun = 1;
                s_underrunEpisodes++;
            }
            s_underrunFrames++;
        }
        return;
    }
    s_inUnderrun = 0;

    idx = (s_ringRead & XA_RING_MASK) * 2;
    *accL += ((int)s_ring[idx]     * s_xa.volQ7L) >> 7;
    *accR += ((int)s_ring[idx + 1] * s_xa.volQ7R) >> 7;
    s_ringRead++;
}

/* --- game-facing API (matches pc_port/src/xa_player.h) ----------------------*/

/* audioLength is in VSync frames; 60 fps * 37800 Hz stereo => 630 samples per
 * frame, 2016 samples/channel per sector (PC formula verbatim). */
static uint32_t CalculateSectorsFromDuration(uint32_t vsyncFrames)
{
    uint32_t totalSamples = 630u * vsyncFrames;
    return (totalSamples + 2015u) / 2016u;
}

void XaPlayer_Play(uint16_t xaIdx)
{
    s_XaItemData* item;
    uint16_t      fileIdx;
    uint32_t      baseSector;
    uint8_t       raw[BIN_SECTOR_SIZE];
    const uint8_t* xs;
    int           isStereo, srCode, bitDepth, sampleRate;

    /* Every rejection MUST signal finished: Sd_XaAudioPlayTaskAdd sets
     * xaAudioIdx_4/D_800C37DC before the task pool reaches us; bailing without
     * the signal leaves Sd_AudioStreamingCheck() busy forever (scene scripts
     * waiting on the voice stall, Bgm_Init blocks for the rest of the map). */
    if (xaIdx >= 727) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u out of range", (unsigned)xaIdx);
        Xa_SignalPlaybackFinished();
        return;
    }

    item    = &g_XaItemData[xaIdx];
    fileIdx = item->xaFileIdx_0;

    if (fileIdx < 1 || fileIdx > 9) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u out of range",
               (unsigned)xaIdx, (unsigned)fileIdx);
        Xa_SignalPlaybackFinished();
        return;
    }

    if (s_xa.isPlaying)
        XaPlayer_Stop();

    Cd_XboxInit();   /* idempotent; the BIN is normally open long before now */

    baseSector = g_FileXaLoc[fileIdx];
    if (baseSector == 0) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u — g_FileXaLoc empty (no file table?)",
               (unsigned)xaIdx, (unsigned)fileIdx);
        Xa_SignalPlaybackFinished();
        return;
    }

    /* Peek the first sector: subheader gives the audio format and the
     * (file, channel) multiplex filter for this voice line. */
    if (!Cd_XboxReadRaw(baseSector + item->sector_4_bits, raw, 1)) {
        SH_DBG("[XA] Play REJECTED: xaIdx=%u fileIdx=%u sector=%u — first-sector read failed",
               (unsigned)xaIdx, (unsigned)fileIdx, (unsigned)item->sector_4_bits);
        Xa_SignalPlaybackFinished();
        return;
    }
    xs       = raw + XA_SLICE_OFFSET;
    isStereo = xs[3] & 1;
    srCode   = (xs[3] >> 2) & 3;    /* 0 = 37800 Hz, 1 = 18900 Hz */
    bitDepth = (xs[3] >> 4) & 1;    /* 0 = 4-bit (8-bit unused by SH) */
    sampleRate = (srCode == 0) ? 37800 : 18900;

    memset(&s_xa.lastSamples, 0, sizeof(s_xa.lastSamples));
    s_xa.xaIdx            = xaIdx;
    s_xa.baseSector       = baseSector;
    s_xa.currentSector    = item->sector_4_bits;
    s_xa.totalSectors     = CalculateSectorsFromDuration(item->audioLength_8_bits);
    s_xa.remainingSectors = s_xa.totalSectors;
    s_xa.sampleRate       = sampleRate;
    s_xa.isStereo         = isStereo;
    s_xa.filterFile       = xs[0];
    s_xa.filterChannel    = xs[1];
    s_xa.rsStep           = ((uint32_t)sampleRate * 65536u) / (uint32_t)OUT_HZ;
    s_xa.rsPhase          = 0;
    s_xa.rsPrevL          = 0;
    s_xa.rsPrevR          = 0;
    /* Reset gain to unity on every Play: the game's task pool emits a
     * Sd_SetVolXa(0,0) "mute-before-seek" whose PSX-side restore never fires
     * on the native ports (the seek is a no-op) — without this reset every
     * voice after the first ~20 plays silently. (Same fix as the PC player.) */
    s_xa.volQ7L = 128;
    s_xa.volQ7R = 128;

    s_ringRead  = 0;
    s_ringWrite = 0;
    s_inUnderrun = 0;

    s_xa.isPlaying = 1;

    SH_DBG("[XA] Play xaIdx=%u file=%u sector=%u sectors=%u %s %dHz bits=%d filter=(%u,%u)",
           (unsigned)xaIdx, (unsigned)fileIdx, (unsigned)item->sector_4_bits,
           s_xa.totalSectors, isStereo ? "stereo" : "mono", sampleRate,
           bitDepth ? 8 : 4, (unsigned)xs[0], (unsigned)xs[1]);

    /* Initial fill (~400 ms) so the mixer never starts on an empty ring. */
    Xa_XboxPump();
    SH_DBG("[XA] primed ring=%u frames", s_ringWrite - s_ringRead);
}

void XaPlayer_PlayWithParams(uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors)
{
    (void)fileIdx; (void)sectorOffset; (void)numSectors;
    XaPlayer_Play(xaIdx);
}

void XaPlayer_Stop(void)
{
    if (s_xa.isPlaying) {
        SH_DBG("[XA] Stop xaIdx=%u (remaining=%u/%u sectors, ring=%u frames)",
               (unsigned)s_xa.xaIdx, s_xa.remainingSectors, s_xa.totalSectors,
               s_ringWrite - s_ringRead);
    }

    s_xa.isPlaying = 0;
    s_ringRead     = 0;
    s_ringWrite    = 0;

    /* Clear the streaming-state flags Sd_AudioStreamingCheck consults. Safe
     * for the queued Stop-before-Play in Sd_XaAudioPlayTaskAdd: the upcoming
     * Play uses xaAudioIdxCheck_2, which survives this. */
    Xa_SignalPlaybackFinished();
}

void XaPlayer_Update(void)
{
    if (!s_xa.isPlaying)
        return;

    Xa_XboxPump();

    /* Rate-limited underrun reporting: first 5 episodes, then every 50th. */
    if (s_underrunEpisodes != s_underrunLogged &&
        (s_underrunEpisodes <= 5 || (s_underrunEpisodes % 50) == 0)) {
        s_underrunLogged = s_underrunEpisodes;
        SH_DBG("[XA] underrun #%u (%u silent frames total)",
               s_underrunEpisodes, s_underrunFrames);
    }

    /* End of playback: duration countdown done AND the ring fully drained. */
    if (s_xa.remainingSectors == 0 && s_ringRead == s_ringWrite) {
        SH_DBG("[XA] finished xaIdx=%u (drained)", (unsigned)s_xa.xaIdx);
        s_xa.isPlaying = 0;
        Xa_SignalPlaybackFinished();
    }
}

void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight)
{
    static uint32_t s_volLogCount = 0;
    int l = volLeft, r = volRight;

    if (l < 0) l = 0;
    if (l > 127) l = 127;
    if (r < 0) r = 0;
    if (r > 127) r = 127;

    /* 0..127 -> Q7 gain with 127 mapping to exact unity (128). */
    s_xa.volQ7L = (l * 128) / 127;
    s_xa.volQ7R = (r * 128) / 127;

    if (s_volLogCount < 8 || (s_volLogCount % 64) == 0)
        SH_DBG("[XA] SetVolume L=%d R=%d", l, r);
    s_volLogCount++;
}
