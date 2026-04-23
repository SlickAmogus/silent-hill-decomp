#include "xa_player.h"
#include "sh_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>

// XA file mapping: xaFileIdx_0 → filename
static const char* const g_XaFileNames[] = {
    NULL,        // 0: invalid
    "05_02152",  // 1
    "10_04432",  // 2
    "15_07496",  // 3
    "20_06552",  // 4
    "25_03904",  // 5
    "30_04056",  // 6
    "35_26008",  // 7
    "40_10384",  // 8
    "45_28784",  // 9
};

// ADPCM filter tables (from DuckStation SPU)
static const int8_t g_FilterPos[] = {0, 60, 115, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const int8_t g_FilterNeg[] = {0, 0, -52, -55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
    uint32_t totalSectors;
    uint32_t remainingSectors;
    uint32_t currentSector;

    int sampleRate;
    int isStereo;
    int bitDepth;

    int isPlaying;
    int isPreparedForPlayback;

    // ADPCM state per channel (L/R)
    int16_t lastSamples[2][2];  // [channel][prev0, prev1]
} XaPlayerState;

static XaPlayerState g_XaPlayer = {0};

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

// Decode ADPCM from a 4-bit sub-block
// nibbles: 28 nibbles (4-bit each, packed 2 per byte)
// subblockIdx: which of 8 sub-blocks (0-7)
// shift: left shift amount from header
// filterIdx: filter index (0-3)
// prev: previous samples [0]=prev0, [1]=prev1
// isRight: 0=left channel, 1=right channel (for stereo interleaving)
// outSamples: pointer to write 28 decoded samples
static void DecodeAdpcmSubblock4bit(const uint8_t* words, int shift, int filterIdx,
                                     int16_t prev[2], int isRight, int16_t* outSamples) {
    int8_t filterPos = g_FilterPos[filterIdx];
    int8_t filterNeg = g_FilterNeg[filterIdx];

    for (int w = 0; w < 28; w++) {
        // Extract 4-bit nibble: each word holds 2 nibbles (8 sub-blocks interleaved)
        uint32_t wordVal;
        memcpy(&wordVal, &words[w * 4], 4);  // LE
        int nibble = (wordVal >> (isRight * 4)) & 0xF;

        // Expand 4-bit to 16-bit with shift
        int32_t sample = ((int16_t)(nibble << 12)) >> shift;

        // Mix in filtered previous samples
        sample += ((int32_t)prev[0] * filterPos) >> 6;
        sample += ((int32_t)prev[1] * filterNeg) >> 6;

        // Clamp and store
        prev[1] = prev[0];
        prev[0] = ClampS16(sample);

        outSamples[w] = prev[0];
    }
}

// Decode one XA sector to PCM
// Returns number of samples decoded (stereo = 2 per sample pair)
static int DecodeXaSector(const uint8_t* sector, int16_t* pcmOut) {
    // Parse coding info from subheader
    uint8_t coding = sector[3];
    int isStereo = coding & 1;
    int sampleRate = (coding >> 2) & 3;
    int bitDepth = (coding >> 4) & 1;

    // For now, only handle 4-bit stereo (what 05_02152 uses)
    if (bitDepth != 0 || !isStereo) {
        SH_DBG("[XA] Warning: 8-bit or mono not yet implemented. coding=0x%02X", coding);
        return 0;
    }

    int16_t* outPtr = pcmOut;

    // 18 groups per sector
    for (int g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const uint8_t* group = sector + XA_PAYLOAD_OFFSET + g * XA_GROUP_SIZE;
        const uint8_t* headers = group + 4;   // skip first 4-byte duplicate
        const uint8_t* words = group + 16;

        // 8 sub-blocks (for 4-bit stereo: 4 left + 4 right, interleaved)
        for (int sb = 0; sb < 8; sb++) {
            int shift = headers[sb] & 0xF;
            int filterIdx = (headers[sb] >> 4) & 0xF;
            int channelIdx = sb & 1;  // 0=left, 1=right

            // Decode 28 samples for this sub-block
            int16_t samples[28];
            DecodeAdpcmSubblock4bit(words, shift, filterIdx,
                                     g_XaPlayer.lastSamples[channelIdx],
                                     sb >> 1,  // isRight for word extraction
                                     samples);

            // Interleave stereo: samples for left (sb=0,2,4,6) and right (sb=1,3,5,7)
            for (int s = 0; s < 28; s++) {
                if (channelIdx == 0) {
                    // Left channel (even sub-blocks)
                    outPtr[s * 2] = samples[s];
                } else {
                    // Right channel (odd sub-blocks)
                    outPtr[s * 2 + 1] = samples[s];
                }
            }

            outPtr += 28 * 2;  // advance by 28 stereo pairs
        }
    }

    return XA_SAMPLES_PER_SECTOR;  // total samples (mono count; stereo needs x2 interleaved)
}

// Find XA file by index, trying multiple search paths
static FILE* OpenXaFile(int fileIdx) {
    if (fileIdx < 1 || fileIdx > 9 || !g_XaFileNames[fileIdx]) {
        SH_DBG("[XA] Invalid file index: %d", fileIdx);
        return NULL;
    }

    const char* filename = g_XaFileNames[fileIdx];
    const char* searchDirs[] = {
        "disc_extract/XA/",
        "../disc_extract/XA/",
        "../../disc_extract/XA/",
    };

    for (int d = 0; d < 3; d++) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s", searchDirs[d], filename);
        FILE* f = fopen(path, "rb");
        if (f) {
            SH_DBG("[XA] Opened file: %s", path);
            return f;
        }
    }

    SH_DBG("[XA] Failed to open file index %d (%s)", fileIdx, filename);
    return NULL;
}

// Calculate number of sectors to read based on VSync frames
static uint32_t CalculateSectorsFromDuration(uint32_t vyncFrames) {
    // At 60 FPS: 1 frame = 37800/60 = 630 samples (per channel)
    // 37.8kHz stereo: 2016 samples per channel per sector
    // 630 * vyncFrames samples / 2016 samples/sector
    uint32_t totalSamples = (630 * vyncFrames);
    uint32_t sectors = (totalSamples + 2015) / 2016;  // round up
    return sectors;
}

// Initialize playback for a specific XA index
void XaPlayer_Play(uint16_t xaIdx) {
    SH_DBG("[XA] Play request: xaIdx=%u", xaIdx);

    // For now, just set playing state
    // Full implementation would:
    // 1. Look up g_XaItemData[xaIdx]
    // 2. Open the file
    // 3. Prepare OpenAL buffers
    // 4. Start playback

    g_XaPlayer.isPlaying = 1;
}

void XaPlayer_Stop(void) {
    SH_DBG("[XA] Stop request");

    if (g_XaPlayer.alSource) {
        alSourceStop(g_XaPlayer.alSource);
    }

    g_XaPlayer.isPlaying = 0;
}

void XaPlayer_Update(void) {
    // Check if playback is ongoing
    if (!g_XaPlayer.isPlaying) {
        return;
    }

    // TODO: Check for processed buffers and refill
}

void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight) {
    if (!g_XaPlayer.alSource) {
        return;
    }

    // Average left and right, normalize to 0.0-1.0
    int vol = (volLeft + volRight) / 2;
    float gain = vol / 84.0f;  // 84 is max per game code
    alSourcef(g_XaPlayer.alSource, AL_GAIN, gain);

    SH_DBG("[XA] Volume set: %d (gain %.2f)", vol, gain);
}
