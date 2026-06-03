/*
 * fmv_player.cpp - PC FMV playback for Silent Hill
 *
 * Plays pre-converted AVI (MJPG) files using libjpeg + OpenGL.
 * Audio streamed via SDL_QueueAudio (PCM).
 * Adapted from REDRIVER2's VideoPlayer (BSD licensed).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "fmv_player.h"
#include "ReadAVI.h"
#include "mdec.h"
#include "str_demux.h"
#include "sh_log.h"

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <PsyX/util/timer.h>
#include <PsyX/common/glad.h>

#include <psx/libgpu.h>
#include <psx/libetc.h>

#include <jpeglib.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern "C" const char* PcPort_GetGameDataPath(void);

/* FMV diagnostics need to survive the brief tick=23 MovieIntro state — if
 * the game exits before the next stdout flush, plain printf output never
 * hits the log file. Redirect every [FMV] line through SH_DBG (which
 * writes to the line-buffered, exception-handler-flushed g_ShDebugLog). */
#undef printf
#define printf(...) SH_DBG_PRINTF_TRAILING_NEWLINE(__VA_ARGS__)
static inline void SH_DBG_PRINTF_TRAILING_NEWLINE(const char* fmt, ...) {
    if (!g_ShDebugLog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_ShDebugLog, fmt, ap);
    va_end(ap);
    fflush(g_ShDebugLog);
}

/* PsyCross internal - needed for direct buffer swap */
extern SDL_Window* g_window;

/* File ID to filename + disc-sector mapping.
 *
 * `name`         — base filename (used to locate optional AVI override).
 * `base_sector`  — disc-absolute sector where this file's data begins.
 * `n_sectors`    — total sectors the file spans on disc (== the suffix in `name`).
 *
 * Sector offsets are pulled from filetable.c.inc (entries 2044..2073, the
 * XA/MOVIE block on Silent Hill USA). The PSX SDK packed XA dialog audio
 * and STR FMV video into the same sector layout — entries 0..8 are pure
 * audio (in-game voice lines, played via xa_player), entries 9..29 are
 * video+audio cutscenes. The demuxer transparently skips audio sectors
 * when reading a video stream, so the same table feeds both paths. */
typedef struct {
    const char* name;
    uint32_t    base_sector;
    uint32_t    n_sectors;
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

#define FMV_FILE_COUNT (sizeof(s_fmvFiles) / sizeof(s_fmvFiles[0]))

/* First XA file index in the file enum (FILE_XA_05_02152 = 2044 across versions) */
#define FIRST_XA_FILE_IDX 2044

/* GL resources */
static GLuint s_fmvTexture = 0;

/* Decode buffer - large enough for 1080p RGB */
#define DECODE_BUFFER_SIZE (1920 * 1080 * 3)
static unsigned char* s_decodeBuffer = NULL;

static int UnpackJPEG(unsigned char* src, unsigned src_len, unsigned char* dst, int* out_w, int* out_h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_start_decompress(&cinfo);

    *out_w = cinfo.image_width;
    *out_h = cinfo.image_height;

    for (unsigned char* scanline = dst;
         cinfo.output_scanline < cinfo.output_height;
         scanline += cinfo.output_width * cinfo.num_components)
    {
        jpeg_read_scanlines(&cinfo, &scanline, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* Raw GL fullscreen quad - bypasses PsyCross vertex format */
static GLuint s_fmvVAO = 0;
static GLuint s_fmvVBO = 0;
static GLuint s_fmvProgram = 0;

static const char* s_fmvVertSrc =
    "#version 140\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char* s_fmvFragSrc =
    "#version 140\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D s_texture;\n"
    "void main() {\n"
    "    fragColor = texture(s_texture, v_uv);\n"
    "}\n";

static void InitBlitResources(void)
{
    if (s_fmvProgram)
        return;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &s_fmvVertSrc, NULL);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &s_fmvFragSrc, NULL);
    glCompileShader(fs);

    s_fmvProgram = glCreateProgram();
    glAttachShader(s_fmvProgram, vs);
    glAttachShader(s_fmvProgram, fs);
    glBindAttribLocation(s_fmvProgram, 0, "a_pos");
    glBindAttribLocation(s_fmvProgram, 1, "a_uv");
    glLinkProgram(s_fmvProgram);

    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &s_fmvVAO);
    glGenBuffers(1, &s_fmvVBO);
}

/* Save/restore GL state so PsyCross is undisturbed */
typedef struct {
    GLboolean depth_test, stencil_test, blend, scissor_test;
    GLint viewport[4];
    GLfloat clear_color[4];
    GLint active_texture;
    GLint bound_texture;
    GLint current_program;
    GLint bound_vao;
    GLint bound_vbo;
} FmvGLState;

static void SaveGLState(FmvGLState* s)
{
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->stencil_test = glIsEnabled(GL_STENCIL_TEST);
    s->blend = glIsEnabled(GL_BLEND);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->bound_texture);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->current_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->bound_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->bound_vbo);
}

static void RestoreGLState(const FmvGLState* s)
{
    if (s->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s->stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s->scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glActiveTexture(s->active_texture);
    glBindTexture(GL_TEXTURE_2D, s->bound_texture);
    glUseProgram(s->current_program);
    glBindVertexArray(s->bound_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s->bound_vbo);
}

static void DrawVideoFrame(int image_w, int image_h)
{
    int windowWidth, windowHeight;
    PsyX_GetScreenSize(&windowWidth, &windowHeight);

    float video_aspect = (float)image_w / (float)image_h;
    float window_aspect = (float)windowWidth / (float)windowHeight;

    float scaleX, scaleY;
    if (video_aspect > window_aspect) {
        scaleX = 1.0f;
        scaleY = window_aspect / video_aspect;
    } else {
        scaleX = video_aspect / window_aspect;
        scaleY = 1.0f;
    }

    /* pos.x, pos.y, uv.x, uv.y */
    float quad[] = {
        -scaleX,  scaleY,   0.0f, 0.0f,
         scaleX,  scaleY,   1.0f, 0.0f,
        -scaleX, -scaleY,   0.0f, 1.0f,
         scaleX, -scaleY,   1.0f, 1.0f,
    };

    FmvGLState saved;
    SaveGLState(&saved);

    glViewport(0, 0, windowWidth, windowHeight);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fmvTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, image_w, image_h, 0, GL_RGB, GL_UNSIGNED_BYTE, s_decodeBuffer);

    glUseProgram(s_fmvProgram);
    glUniform1i(glGetUniformLocation(s_fmvProgram, "s_texture"), 0);

    glBindVertexArray(s_fmvVAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_fmvVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    RestoreGLState(&saved);

    SDL_GL_SwapWindow(g_window);
}

/* Try to find AVI file in several locations */
static int FindAviFile(const char* basename, char* out_path, int out_path_size)
{
    const char* search_dirs[] = {
        "gamedata/fmv/",
        "../gamedata/fmv/",
        "",
    };
    const char* extensions[] = { ".avi", ".AVI" };

    for (int d = 0; search_dirs[d]; d++) {
        for (int e = 0; e < 2; e++) {
            snprintf(out_path, out_path_size, "%s%s%s", search_dirs[d], basename, extensions[e]);
            FILE* f = fopen(out_path, "rb");
            if (f) {
                fclose(f);
                return 0;
            }
        }
        if (search_dirs[d][0] == '\0') break;
    }
    return -1;
}

extern "C" void FMV_Init(void)
{
    if (s_decodeBuffer)
        return;

    s_decodeBuffer = (unsigned char*)malloc(DECODE_BUFFER_SIZE);
    memset(s_decodeBuffer, 0, DECODE_BUFFER_SIZE);

    glGenTextures(1, &s_fmvTexture);
    glBindTexture(GL_TEXTURE_2D, s_fmvTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    InitBlitResources();
}

extern "C" void FMV_Shutdown(void)
{
    if (s_decodeBuffer) {
        ::free(s_decodeBuffer);
        s_decodeBuffer = NULL;
    }
    if (s_fmvTexture) {
        glDeleteTextures(1, &s_fmvTexture);
        s_fmvTexture = 0;
    }
    if (s_fmvVAO) {
        glDeleteVertexArrays(1, &s_fmvVAO);
        s_fmvVAO = 0;
    }
    if (s_fmvVBO) {
        glDeleteBuffers(1, &s_fmvVBO);
        s_fmvVBO = 0;
    }
    if (s_fmvProgram) {
        glDeleteProgram(s_fmvProgram);
        s_fmvProgram = 0;
    }
}

/* ===== XA-ADPCM decoder for FMV audio =====
 *
 * Silent Hill FMVs interleave 4-bit XA-ADPCM stereo audio sectors with the
 * MDEC video sectors. The demuxer (str_demux) forwards every audio sector
 * to FmvAudio_OnSector via the registered callback; we decode the 18 sound
 * groups into 4032 interleaved int16 samples and push them to SDL's audio
 * queue. Format constants and decode logic mirror xa_player.c's standalone
 * implementation — we keep separate ADPCM filter state so the FMV path
 * does not collide with in-game XA voice playback. */
static const int16_t s_fmvXaFilterPos[16] = {0, 60, 115,  98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const int16_t s_fmvXaFilterNeg[16] = {0,  0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

#define FMV_XA_PAYLOAD_OFFSET     8
#define FMV_XA_GROUPS_PER_SECTOR  18
#define FMV_XA_GROUP_SIZE         128
#define FMV_XA_SAMPLES_PER_SECTOR 4032   /* int16 count: 18 groups × 4 stereo pairs × 28 samples × 2ch */

typedef struct {
    SDL_AudioDeviceID dev;
    int               sampleRate;
    int               isStereo;
    int               isOpen;
    int32_t           lastSamples[2][2]; /* [channel][prev0=newer, prev1=older] */
    int               sectorsDecoded;
} FmvAudioState;

static FmvAudioState s_fmvAudio;

static inline int16_t FmvClampS16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Decode 28 ADPCM samples from one 4-bit sub-block of a sound group.
 * Identical to xa_player.c's DecodeSubblock4bit — see that file for full
 * notes on group layout. Briefly: 8 header bytes describe the 8 sub-blocks
 * (shift in low nibble, filter idx in high nibble) and 28 little-endian
 * u32 words pack 8 nibbles each (one per sub-block). prev[] is the IIR
 * history (UNCLAMPED — clamping only happens on the emitted s16). */
static void FmvDecodeSubblock4bit(const uint8_t* group, int sb,
                                  int32_t prev[2], int16_t out[28])
{
    const uint8_t* headers = group + 4;
    const uint8_t* words   = group + 16;

    int     shift     = headers[sb] & 0xF;
    int     filterIdx = (headers[sb] >> 4) & 0x7;
    int16_t fpos      = s_fmvXaFilterPos[filterIdx];
    int16_t fneg      = s_fmvXaFilterNeg[filterIdx];
    int     byteIdx   = sb >> 1;
    int     nibShift  = (sb & 1) ? 4 : 0;

    for (int w = 0; w < 28; w++) {
        uint8_t b      = words[w * 4 + byteIdx];
        int     nibble = (b >> nibShift) & 0xF;
        int32_t s      = ((int16_t)(nibble << 12)) >> shift;
        s += ((int32_t)prev[0] * fpos + (int32_t)prev[1] * fneg + 32) >> 6;
        prev[1] = prev[0];
        prev[0] = s;
        out[w]  = FmvClampS16(s);
    }
}

/* Decode one 2336-byte XA sector. Returns int16 sample count written.
 * Layout: 18 sound groups, each 128 bytes, starting at offset 8 (after
 * the 8-byte XA subheader). For 4-bit stereo, each group emits 8 sub-blocks
 * = 4 stereo pairs × 28 samples × 2 channels = 224 int16 per group →
 * 4032 int16 per sector. */
static int FmvDecodeXaSector(const uint8_t* sector, int16_t* pcmOut)
{
    uint8_t coding   = sector[3];
    int     isStereo = coding & 1;
    int     bitDepth = (coding >> 4) & 1;

    if (bitDepth != 0) {
        /* 8-bit XA is rare and not used by Silent Hill FMVs — bail. */
        return 0;
    }

    int     written = 0;
    int16_t* out    = pcmOut;

    for (int g = 0; g < FMV_XA_GROUPS_PER_SECTOR; g++) {
        const uint8_t* group = sector + FMV_XA_PAYLOAD_OFFSET + g * FMV_XA_GROUP_SIZE;

        if (isStereo) {
            for (int p = 0; p < 4; p++) {
                int16_t l[28], r[28];
                FmvDecodeSubblock4bit(group, 2 * p,     s_fmvAudio.lastSamples[0], l);
                FmvDecodeSubblock4bit(group, 2 * p + 1, s_fmvAudio.lastSamples[1], r);
                for (int s = 0; s < 28; s++) {
                    *out++ = l[s];
                    *out++ = r[s];
                }
                written += 56;
            }
        } else {
            for (int sb = 0; sb < 8; sb++) {
                int16_t s[28];
                FmvDecodeSubblock4bit(group, sb, s_fmvAudio.lastSamples[0], s);
                for (int i = 0; i < 28; i++) *out++ = s[i];
                written += 28;
            }
        }
    }

    return written;
}

/* str_demux audio sector callback. Lazily opens an SDL audio device on
 * the first sector (using its subheader to learn rate/channels), then
 * decodes and queues PCM. Must be C-linkage so it can be assigned to
 * str_audio_cb_t (declared inside extern "C"). */
extern "C" {
static void FmvAudio_OnSector(const uint8_t* sector, void* user)
{
    FmvAudioState* st = (FmvAudioState*)user;

    if (!st->isOpen) {
        uint8_t coding     = sector[3];
        int     isStereo   = coding & 1;
        int     srCode     = (coding >> 2) & 3;
        int     sampleRate = (srCode == 0) ? 37800 : 18900;

        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
                printf("[FMV] SDL audio init failed: %s\n", SDL_GetError());
                return;
            }
        }

        SDL_AudioSpec want, got;
        SDL_memset(&want, 0, sizeof(want));
        want.freq     = sampleRate;
        want.format   = AUDIO_S16LSB;
        want.channels = (Uint8)(isStereo ? 2 : 1);
        want.samples  = 4096;

        st->dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (st->dev == 0) {
            printf("[FMV] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return;
        }
        st->sampleRate = got.freq;
        st->isStereo   = (got.channels == 2);
        st->isOpen     = 1;
        SDL_PauseAudioDevice(st->dev, 0);
        printf("[FMV] XA audio opened: %d Hz %s (coding=0x%02X)\n",
               sampleRate, isStereo ? "stereo" : "mono", coding);
    }

    if (st->dev == 0) return;

    int16_t pcm[FMV_XA_SAMPLES_PER_SECTOR];
    int     n = FmvDecodeXaSector(sector, pcm);
    if (n > 0) {
        SDL_QueueAudio(st->dev, pcm, (Uint32)(n * (int)sizeof(int16_t)));
        st->sectorsDecoded++;
    }
}
} /* extern "C" */

/* Determine SDL audio format from AVI audio stream info */
static SDL_AudioDeviceID OpenFmvAudio(const ReadAVI::stream_format_auds_t* fmt, SDL_AudioSpec* obtained)
{
    if (fmt->samples_per_second == 0 || fmt->channels == 0)
        return 0;

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[FMV] Failed to init SDL audio: %s\n", SDL_GetError());
            return 0;
        }
    }

    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = fmt->samples_per_second;
    want.channels = (Uint8)fmt->channels;
    want.samples = 4096;

    if (fmt->bits_per_sample == 16)
        want.format = AUDIO_S16LSB;
    else if (fmt->bits_per_sample == 8)
        want.format = AUDIO_U8;
    else {
        printf("[FMV] Unsupported audio bits_per_sample: %d\n", fmt->bits_per_sample);
        return 0;
    }

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, obtained, 0);
    if (dev == 0) {
        printf("[FMV] Failed to open audio device: %s\n", SDL_GetError());
        return 0;
    }

    printf("[FMV] Audio: %d Hz, %d ch, %d bit\n",
           fmt->samples_per_second, fmt->channels, fmt->bits_per_sample);

    return dev;
}

/* Open <gamedata>/Silent Hill (USA).bin. Caller owns the FILE*. */
static FILE* OpenDiscImage(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/Silent Hill (USA).bin",
             PcPort_GetGameDataPath());
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[FMV] Failed to open disc image: %s\n", path);
    }
    return f;
}

/* Poll skip keys, drain SDL events, and return 1 if the user wants to bail. */
static int PollSkipOrQuit(int* out_quit)
{
    SDL_PumpEvents();

    SDL_Event evt;
    while (SDL_PollEvent(&evt)) {
        if (evt.type == SDL_QUIT) {
            if (out_quit) *out_quit = 1;
            return 1;
        }
    }

    const Uint8* keystate = SDL_GetKeyboardState(NULL);
    return (keystate[SDL_SCANCODE_RETURN] ||
            keystate[SDL_SCANCODE_ESCAPE] ||
            keystate[SDL_SCANCODE_SPACE]);
}

/* Play an FMV directly from the BIN disc image using the MDEC software
 * decoder. Audio is *not* decoded here yet — XA dialogue tracks have their
 * own pipeline (xa_player) and FMV soundtrack sync is a follow-up. Returns
 * 0 on completion / user skip, -1 on open failure. */
static int PlayFromBin(int table_idx, int max_frames)
{
    const FmvFileEntry& e = s_fmvFiles[table_idx];

    FILE* bin = OpenDiscImage();
    if (!bin) return -1;

    str_stream_t stream;
    if (!str_open(&stream, bin, e.base_sector, e.n_sectors)) {
        printf("[FMV] str_open failed for %s\n", e.name);
        fclose(bin);
        return -1;
    }

    /* Reset FMV audio state and register the audio-sector callback. The
     * demuxer calls this for every audio sector encountered while reading
     * the next video frame; the callback lazily opens an SDL audio device
     * on the first sector and queues decoded PCM thereafter. */
    SDL_memset(&s_fmvAudio, 0, sizeof(s_fmvAudio));
    stream.audio_cb   = &FmvAudio_OnSector;
    stream.audio_user = &s_fmvAudio;

    FMV_Init();

    /* Allocate persistent decode state: a 16k-halfword bitstream buffer
     * (matches STR_FRAME_BS_MAX_HALFWORDS), the MDEC context (lazy IDCT/
     * VLC tables), and the RGB output. */
    static uint16_t bs[STR_FRAME_BS_MAX_HALFWORDS];
    mdec_ctx_t ctx;
    int ctx_initialized = 0;
    uint8_t* rgb = NULL;
    int rgb_w = 0, rgb_h = 0;

    str_frame_info_t info;
    size_t bs_halfwords = 0;

    timerCtx_t fmvTimer;
    Util_InitHPCTimer(&fmvTimer);
    double nextFrameDelay = 0.0;
    int done_frames = 0;
    int want_quit = 0;

    Util_GetHPCTime(&fmvTimer, 1);

    /* Flush stale keys */
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_KEYDOWN);
    SDL_FlushEvent(SDL_KEYUP);

    while (1) {
        if (PollSkipOrQuit(&want_quit)) {
            printf("[FMV] Skipped at frame %d\n", done_frames);
            break;
        }

        double delta = Util_GetHPCTime(&fmvTimer, 1);
        if (delta > 1.0) delta = 0.0;
        nextFrameDelay -= delta;

        if (nextFrameDelay > 0) {
            SDL_Delay(1);
            continue;
        }

        int r = str_read_frame(&stream, bs, &info, &bs_halfwords);
        if (r <= 0) {
            /* [FMVEND] why did playback stop? r==0 EOF (cur_sector hit
             * total) vs r==-1 demux error (secCount mismatch / bitstream
             * overflow). Compare done_frames to the expected count to see
             * if the FMV cut off early. */
            SH_DBG("[FMVEND] stop: r=%d done_frames=%d max_frames=%d cur_sector=%u total_sectors=%u (%s)",
                   r, done_frames, max_frames,
                   (unsigned)stream.cur_sector, (unsigned)stream.total_sectors,
                   r == 0 ? "EOF" : "DEMUX-ERROR");
            break; /* EOF or error */
        }

        if (max_frames > 0 && done_frames >= max_frames) {
            SH_DBG("[FMVEND] stop: hit max_frames=%d done_frames=%d cur_sector=%u/%u",
                   max_frames, done_frames, (unsigned)stream.cur_sector, (unsigned)stream.total_sectors);
            break;
        }

        /* (Re)allocate RGB buffer if dimensions changed (shouldn't for a
         * single FMV, but cheap to be defensive). */
        if (info.width != rgb_w || info.height != rgb_h) {
            ::free(rgb);
            rgb = (uint8_t*)calloc((size_t)info.width * info.height * 3u, 1);
            rgb_w = info.width;
            rgb_h = info.height;
            if (!ctx_initialized) {
                mdec_init(&ctx, info.width, info.height);
                ctx_initialized = 1;
            }
        }
        if (!rgb) break;

        int mb = mdec_decode_frame(&ctx,
                                   (const uint8_t*)bs,
                                   bs_halfwords * 2u,
                                   rgb);
        if (mb < 0) {
            /* Decode failure — emit a blank frame so timing still ticks. */
            memset(rgb, 0, (size_t)info.width * info.height * 3u);
        }

        /* Sample some center pixels every 30 frames to verify the decoder is
         * actually producing varied content at runtime (not just in offline
         * tests). If these are all zero we know the on-screen black is the
         * decoder; if non-zero, it's the upload/draw path. */
        if ((done_frames % 30) == 0) {
            const uint8_t* p = rgb + ((size_t)(info.height/2) * info.width + info.width/2) * 3u;
            SH_DBG("[FMV] frame %d mb=%d center pixels: "
                   "(%02x %02x %02x) (%02x %02x %02x) (%02x %02x %02x)",
                   done_frames, mb,
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
        }

        /* Move into the persistent decode buffer for DrawVideoFrame, then
         * blit. We copy because s_decodeBuffer is what DrawVideoFrame
         * uploads to GL — keeping that contract stable. */
        size_t bytes = (size_t)info.width * info.height * 3u;
        if (bytes <= (size_t)DECODE_BUFFER_SIZE) {
            memcpy(s_decodeBuffer, rgb, bytes);
            DrawVideoFrame(info.width, info.height);
        }

        /* PSX STR videos target 15fps (every other NTSC frame). */
        nextFrameDelay += 1.0 / 15.0;
        done_frames++;

        if (want_quit) break;
    }

    ::free(rgb);
    fclose(bin);

    /* Wait for skip keys to release before returning so the still-held key
     * doesn't carry into the next state's first Joy_Update (Enter → phantom
     * Confirm on the title). Same protection as the AVI path. */
    {
        int wait_frames = 0;
        while (wait_frames < 30) {
            SDL_PumpEvents();
            const Uint8* ks = SDL_GetKeyboardState(NULL);
            if (!ks[SDL_SCANCODE_RETURN] && !ks[SDL_SCANCODE_ESCAPE] &&
                !ks[SDL_SCANCODE_SPACE])
                break;
            SDL_Delay(16);
            wait_frames++;
        }
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_KEYDOWN);
        SDL_FlushEvent(SDL_KEYUP);
    }

    printf("[FMV] BIN playback complete (%d frames from %s)\n",
           done_frames, e.name);
    return 0;
}

extern "C" int FMV_Play(int file_idx, int max_frames)
{
    int table_idx = file_idx - FIRST_XA_FILE_IDX;
    if (table_idx < 0 || table_idx >= (int)FMV_FILE_COUNT) {
        printf("[FMV] File index %d out of range (table_idx=%d)\n", file_idx, table_idx);
        return -1;
    }

    const char* basename = s_fmvFiles[table_idx].name;
    char filepath[512];

    /* Try AVI override first — users can drop upscaled MJPG AVIs under
     * gamedata/fmv/ to replace any cutscene. If no override exists we
     * fall back to decoding the original STR straight from the BIN. */
    if (FindAviFile(basename, filepath, sizeof(filepath)) != 0) {
        printf("[FMV] No AVI override for '%s' — decoding from BIN\n", basename);
        return PlayFromBin(table_idx, max_frames);
    }

    printf("[FMV] Playing: %s\n", filepath);

    FMV_Init();

    ReadAVI readAVI(filepath);
    if (!readAVI.IsOpen()) {
        printf("[FMV] Failed to open AVI: %s\n", filepath);
        return -1;
    }

    ReadAVI::avi_header_t avi_header = readAVI.GetAviHeader();
    ReadAVI::stream_format_t stream_format = readAVI.GetVideoFormat();

    if (strcmp(stream_format.compression_type, "MJPG") != 0) {
        printf("[FMV] Unsupported codec: '%s' (only MJPG supported)\n",
               stream_format.compression_type);
        return -1;
    }

    printf("[FMV] Video: %dx%d, %d frames, %.1f fps\n",
           stream_format.image_width, stream_format.image_height,
           avi_header.TotalNumberOfFrames,
           avi_header.TimeBetweenFrames > 0 ? 1000000.0 / avi_header.TimeBetweenFrames : 0);

    /* Set up audio */
    ReadAVI::stream_format_auds_t audio_fmt = readAVI.GetAudioFormat();
    SDL_AudioSpec audioObtained;
    SDL_AudioDeviceID audioDev = OpenFmvAudio(&audio_fmt, &audioObtained);

    if (audioDev)
        SDL_PauseAudioDevice(audioDev, 0); /* Start playback */

    /* Use combined type mask to read both video and audio in one pass */
    const int FRAME_TYPE_ALL = ReadAVI::ctype_video_data | ReadAVI::ctype_audio_data;

    ReadAVI::frame_entry_t frame_entry;
    frame_entry.type = (ReadAVI::chunk_type_t)FRAME_TYPE_ALL;
    frame_entry.pointer = 0;

    timerCtx_t fmvTimer;
    Util_InitHPCTimer(&fmvTimer);

    double nextFrameDelay = 0.0;
    int done_frames = 0;

    Util_GetHPCTime(&fmvTimer, 1);

    /* Flush any pending key events before playback */
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_KEYDOWN);
    SDL_FlushEvent(SDL_KEYUP);

    /* Main playback loop */
    while (1)
    {
        double delta = Util_GetHPCTime(&fmvTimer, 1);
        if (delta > 1.0)
            delta = 0.0;

        nextFrameDelay -= delta;

        /* Check for skip using keyboard state (event-independent) */
        SDL_PumpEvents();
        const Uint8* keystate = SDL_GetKeyboardState(NULL);
        if (keystate[SDL_SCANCODE_RETURN] || keystate[SDL_SCANCODE_ESCAPE] ||
            keystate[SDL_SCANCODE_SPACE])
        {
            printf("[FMV] Skipped at frame %d/%d\n", done_frames, avi_header.TotalNumberOfFrames);
            break;
        }

        /* Drain event queue to keep window responsive */
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) {
                goto done;
            }
        }

        if (nextFrameDelay > 0) {
            SDL_Delay(1);
            continue;
        }

        /* Read next frame (video or audio) */
        frame_entry.type = (ReadAVI::chunk_type_t)FRAME_TYPE_ALL;
        int frame_size = readAVI.GetFrameFromIndex(&frame_entry);

        if (frame_size < 0)
            break;

        if (frame_entry.type == ReadAVI::ctype_audio_data) {
            /* Queue audio data to SDL */
            if (audioDev && frame_size > 0) {
                SDL_QueueAudio(audioDev, frame_entry.buf, frame_size);
            }
            /* Don't apply video timing for audio frames - immediately read next */
            continue;
        }

        /* Video frame */
        if (max_frames > 0 && done_frames >= max_frames)
            break;

        if (frame_size > 0 &&
            (frame_entry.type == ReadAVI::ctype_compressed_video_frame ||
             frame_entry.type == ReadAVI::ctype_uncompressed_video_frame))
        {
            int real_w, real_h;
            if (UnpackJPEG(frame_entry.buf, frame_size, s_decodeBuffer, &real_w, &real_h) == 0)
            {
                DrawVideoFrame(real_w, real_h);
            }

            if (avi_header.TimeBetweenFrames > 0)
                nextFrameDelay += (double)avi_header.TimeBetweenFrames / 1000000.0;
            else
                nextFrameDelay += 1.0 / 15.0; /* fallback: 15fps (PSX STR default) */

            done_frames++;
        }
    }

done:
    /* Clean up audio */
    if (audioDev) {
        SDL_PauseAudioDevice(audioDev, 1);
        SDL_CloseAudioDevice(audioDev);
    }

    /* Wait for skip keys to release before returning. Otherwise the still-held
     * key would carry into the next state's first Joy_Update, which sees the
     * 0→held edge as a fresh btnsClicked and bleeds into the main menu (e.g.
     * Enter to skip intro = phantom Confirm on the title). Capped so a stuck
     * key can't hang the boot. */
    {
        int wait_frames = 0;
        while (wait_frames < 30) /* ~500ms at 60fps */
        {
            SDL_PumpEvents();
            const Uint8* ks = SDL_GetKeyboardState(NULL);
            if (!ks[SDL_SCANCODE_RETURN] && !ks[SDL_SCANCODE_ESCAPE] &&
                !ks[SDL_SCANCODE_SPACE])
                break;
            SDL_Delay(16);
            wait_frames++;
        }
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_KEYDOWN);
        SDL_FlushEvent(SDL_KEYUP);
    }

    printf("[FMV] Playback complete (%d frames)\n", done_frames);
    return 0;
}
