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
extern "C" const char* PcPort_GetGameDiscPath(void);
extern "C" unsigned int PcPort_FileTableStartSector(int fileIdx);

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
 * `base_sector`  — US-disc reference sector (documentation only; playback
 *                  reads the live region-remapped g_FileTable instead).
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

/* Console command accessors (pc_console_cmd.c): list + play by table index.
 * Entries 0..8 are XA voice banks (multiple interleaved audio channels, no
 * video) — feeding them to FMV_Play decodes every channel back-to-back into
 * fast garbled audio with a black screen. Only expose the real movies. */
#define FIRST_MOVIE_TABLE_IDX 9
extern "C" int FMV_GetCount(void) { return (int)FMV_FILE_COUNT - FIRST_MOVIE_TABLE_IDX; }
extern "C" const char* FMV_GetName(int movieIdx)
{
    int tableIdx = FIRST_MOVIE_TABLE_IDX + movieIdx;
    if (movieIdx < 0 || tableIdx >= (int)FMV_FILE_COUNT) return "";
    return s_fmvFiles[tableIdx].name;
}
extern "C" int FMV_GetFileIdx(int movieIdx) { return FIRST_XA_FILE_IDX + FIRST_MOVIE_TABLE_IDX + movieIdx; }

/* GL resources */
static GLuint s_fmvTexture = 0;

/* RGB decode buffer, grown on demand to fit the video — no resolution cap.
 * (A fixed 1920x1080 buffer used to overflow the heap on 4K upscale mods.) */
static unsigned char* s_decodeBuffer = NULL;
static size_t s_decodeBufferSize = 0;

/* Growable scratch for repacking 24/32-bit PCM audio chunks to S16. */
static unsigned char* s_audioConvBuf = NULL;
static size_t s_audioConvBufSize = 0;

static int EnsureDecodeBuffer(size_t bytes)
{
    if (bytes <= s_decodeBufferSize)
        return 0;

    unsigned char* p = (unsigned char*)realloc(s_decodeBuffer, bytes);
    if (!p) {
        printf("[FMV] Out of memory growing decode buffer to %u bytes\n", (unsigned)bytes);
        return -1;
    }
    s_decodeBuffer = p;
    s_decodeBufferSize = bytes;
    return 0;
}

/* libjpeg's default error handler exit()s the process on a corrupt frame —
 * longjmp back out and drop the frame instead. */
#include <setjmp.h>
struct FmvJpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void FmvJpegErrorExit(j_common_ptr cinfo)
{
    FmvJpegErr* err = (FmvJpegErr*)cinfo->err;
    longjmp(err->jump, 1);
}

static int UnpackJPEG(unsigned char* src, unsigned src_len, int* out_w, int* out_h)
{
    struct jpeg_decompress_struct cinfo;
    FmvJpegErr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = FmvJpegErrorExit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    /* Always decode to RGB (grayscale sources included) and size the buffer
     * from the actual frame header, so any resolution fits. */
    cinfo.out_color_space = JCS_RGB;

    if (EnsureDecodeBuffer((size_t)cinfo.image_width * cinfo.image_height * 3u) != 0) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_start_decompress(&cinfo);

    *out_w = cinfo.output_width;
    *out_h = cinfo.output_height;

    for (unsigned char* scanline = s_decodeBuffer;
         cinfo.output_scanline < cinfo.output_height;
         scanline += cinfo.output_width * cinfo.output_components)
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

    /* GPUs cap 2D texture size (16384 on anything modern) — a frame past the
     * cap would silently upload nothing. Warn once so the log names the cause. */
    {
        static int warned = 0;
        static GLint maxTex = -1;
        if (maxTex < 0)
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        if (maxTex > 0 && (image_w > maxTex || image_h > maxTex) && !warned) {
            printf("[FMV] Frame %dx%d exceeds GPU max texture size %d — frame will not display\n",
                   image_w, image_h, (int)maxTex);
            warned = 1;
        }
    }

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

/* ===== Video codec dispatch =====
 *
 * Everything decodes into s_decodeBuffer as tightly-packed RGB24. MJPEG (all
 * fourcc spellings) goes through libjpeg; the raw formats ffmpeg's -c:v
 * rawvideo / BI_RGB writers emit are converted inline. Anything else falls
 * back to the original disc STR so the cutscene still plays. */
typedef enum {
    FMV_CODEC_MJPEG,
    FMV_CODEC_RGB,   /* uncompressed DIB, 24/32 bpp */
    FMV_CODEC_YUY2,
    FMV_CODEC_UYVY,
    FMV_CODEC_I420,  /* also IYUV */
    FMV_CODEC_YV12,
    FMV_CODEC_NV12,
    FMV_CODEC_UNSUPPORTED
} FmvCodec;

static int FourccIs(const char* fourcc, const char* want)
{
    for (int i = 0; i < 4; i++) {
        char a = fourcc[i], b = want[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static FmvCodec IdentifyCodec(const ReadAVI::stream_format_t* fmt)
{
    const char* cc = fmt->compression_type;

    if (FourccIs(cc, "MJPG") || FourccIs(cc, "DMB1") ||
        FourccIs(cc, "JPEG") || FourccIs(cc, "AVI1"))
        return FMV_CODEC_MJPEG;

    /* BI_RGB has a zeroed compression field; some writers tag "DIB ". */
    if ((cc[0] == 0 || FourccIs(cc, "DIB ")) &&
        (fmt->bits_per_pixel == 24 || fmt->bits_per_pixel == 32))
        return FMV_CODEC_RGB;

    if (FourccIs(cc, "YUY2") || FourccIs(cc, "YUYV")) return FMV_CODEC_YUY2;
    if (FourccIs(cc, "UYVY"))                         return FMV_CODEC_UYVY;
    if (FourccIs(cc, "I420") || FourccIs(cc, "IYUV")) return FMV_CODEC_I420;
    if (FourccIs(cc, "YV12"))                         return FMV_CODEC_YV12;
    if (FourccIs(cc, "NV12"))                         return FMV_CODEC_NV12;

    return FMV_CODEC_UNSUPPORTED;
}

/* BT.601 limited-range YUV -> RGB (what raw YUV AVIs carry). */
static inline void YuvToRgb(int y, int u, int v, unsigned char* rgb)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    rgb[0] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
    rgb[1] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
    rgb[2] = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

/* Uncompressed DIB: rows bottom-up unless height was negative, BGR(A) order,
 * rows padded to 4 bytes. */
static int UnpackRGB(const unsigned char* src, unsigned src_len, int w, int h,
                     int bpp, int top_down)
{
    int bytes_pp = bpp / 8;
    size_t stride = ((size_t)w * bytes_pp + 3) & ~(size_t)3;

    if ((size_t)h * stride > src_len)
        return -1;
    if (EnsureDecodeBuffer((size_t)w * h * 3u) != 0)
        return -1;

    for (int y = 0; y < h; y++) {
        const unsigned char* row = src + (size_t)(top_down ? y : (h - 1 - y)) * stride;
        unsigned char* out = s_decodeBuffer + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            out[x * 3 + 0] = row[x * bytes_pp + 2];
            out[x * 3 + 1] = row[x * bytes_pp + 1];
            out[x * 3 + 2] = row[x * bytes_pp + 0];
        }
    }
    return 0;
}

/* Packed 4:2:2 — YUY2 (Y0 U Y1 V) and UYVY (U Y0 V Y1). */
static int UnpackYuv422(const unsigned char* src, unsigned src_len, int w, int h,
                        int y_first)
{
    if ((size_t)w * h * 2u > src_len || (w & 1))
        return -1;
    if (EnsureDecodeBuffer((size_t)w * h * 3u) != 0)
        return -1;

    for (int y = 0; y < h; y++) {
        const unsigned char* row = src + (size_t)y * w * 2;
        unsigned char* out = s_decodeBuffer + (size_t)y * w * 3;
        for (int x = 0; x < w; x += 2) {
            const unsigned char* p = row + x * 2;
            int y0 = y_first ? p[0] : p[1];
            int u  = y_first ? p[1] : p[0];
            int y1 = y_first ? p[2] : p[3];
            int v  = y_first ? p[3] : p[2];
            YuvToRgb(y0, u, v, out + x * 3);
            YuvToRgb(y1, u, v, out + x * 3 + 3);
        }
    }
    return 0;
}

/* Planar/semi-planar 4:2:0 — I420 (Y,U,V), YV12 (Y,V,U), NV12 (Y, UV interleaved). */
static int UnpackYuv420(const unsigned char* src, unsigned src_len, int w, int h,
                        FmvCodec codec)
{
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t ysize = (size_t)w * h, csize = (size_t)cw * ch;

    if (ysize + csize * 2 > src_len)
        return -1;
    if (EnsureDecodeBuffer(ysize * 3u) != 0)
        return -1;

    const unsigned char* yp = src;
    const unsigned char* up;
    const unsigned char* vp;
    int uv_interleaved = (codec == FMV_CODEC_NV12);

    if (codec == FMV_CODEC_YV12) {
        vp = src + ysize;
        up = vp + csize;
    } else { /* I420 + NV12 (up = base of interleaved plane for NV12) */
        up = src + ysize;
        vp = up + (uv_interleaved ? 1 : csize);
    }

    for (int y = 0; y < h; y++) {
        unsigned char* out = s_decodeBuffer + (size_t)y * w * 3;
        size_t crow = (size_t)(y / 2) * (uv_interleaved ? (size_t)cw * 2 : (size_t)cw);
        for (int x = 0; x < w; x++) {
            size_t ci = crow + (size_t)(x / 2) * (uv_interleaved ? 2 : 1);
            YuvToRgb(yp[(size_t)y * w + x], up[ci], vp[ci], out + x * 3);
        }
    }
    return 0;
}

/* Decode one video chunk into s_decodeBuffer. Returns 0 + real dims on success. */
static int DecodeVideoFrame(FmvCodec codec, unsigned char* buf, unsigned len,
                            const ReadAVI::stream_format_t* fmt, int* out_w, int* out_h)
{
    int w = fmt->image_width;
    int h = fmt->image_height < 0 ? -fmt->image_height : fmt->image_height;
    int top_down = fmt->image_height < 0;

    switch (codec) {
        case FMV_CODEC_MJPEG:
            return UnpackJPEG(buf, len, out_w, out_h);
        case FMV_CODEC_RGB:
            if (UnpackRGB(buf, len, w, h, fmt->bits_per_pixel, top_down) != 0) return -1;
            break;
        case FMV_CODEC_YUY2:
            if (UnpackYuv422(buf, len, w, h, 1) != 0) return -1;
            break;
        case FMV_CODEC_UYVY:
            if (UnpackYuv422(buf, len, w, h, 0) != 0) return -1;
            break;
        case FMV_CODEC_I420:
        case FMV_CODEC_YV12:
        case FMV_CODEC_NV12:
            if (UnpackYuv420(buf, len, w, h, codec) != 0) return -1;
            break;
        default:
            return -1;
    }
    *out_w = w;
    *out_h = h;
    return 0;
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

    /* Seed with a 640x480 buffer; EnsureDecodeBuffer grows it to fit
     * whatever resolution actually plays. */
    EnsureDecodeBuffer(640 * 480 * 3);
    if (s_decodeBuffer)
        memset(s_decodeBuffer, 0, s_decodeBufferSize);

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
        s_decodeBufferSize = 0;
    }
    if (s_audioConvBuf) {
        ::free(s_audioConvBuf);
        s_audioConvBuf = NULL;
        s_audioConvBufSize = 0;
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

/* FMV *movie* volume (0..1), independent of the XA cutscene-voice volume
 * (g_PcXaVolume). FMV movie audio streams as raw PCM via SDL_QueueAudio — a
 * different path from the OpenAL XA player — so it must be scaled here, and the
 * two are split into separate options sliders (Voice vs FMV Movie). */
extern "C" { float g_PcFmvVolume = 1.0f; }

static void FmvApplyVolume(void* buf, int bytes, SDL_AudioFormat fmt)
{
    float g = g_PcFmvVolume;
    if (g >= 0.999f) return; /* full volume: leave the samples byte-identical */
    if (g < 0.0f) g = 0.0f;

    if (fmt == AUDIO_S16LSB || fmt == AUDIO_S16MSB)
    {
        int16_t* s = (int16_t*)buf;
        int      n = bytes / (int)sizeof(int16_t);
        for (int i = 0; i < n; i++)
            s[i] = (int16_t)((float)s[i] * g);
    }
    else if (fmt == AUDIO_U8) /* unsigned 8-bit: silence is 128 */
    {
        uint8_t* s = (uint8_t*)buf;
        for (int i = 0; i < bytes; i++)
            s[i] = (uint8_t)(128 + (int)(((float)s[i] - 128.0f) * g));
    }
    else if (fmt == AUDIO_F32LSB)
    {
        float* s = (float*)buf;
        int    n = bytes / (int)sizeof(float);
        for (int i = 0; i < n; i++)
            s[i] *= g;
    }
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
        FmvApplyVolume(pcm, n * (int)sizeof(int16_t), AUDIO_S16LSB);
        SDL_QueueAudio(st->dev, pcm, (Uint32)(n * (int)sizeof(int16_t)));
        st->sectorsDecoded++;
    }
}
} /* extern "C" */

/* AVI audio: PCM integer 8/16 bits map straight to SDL formats, float 32
 * (wFormatTag 3, what `ffmpeg -c:a pcm_f32le` writes) queues as AUDIO_F32,
 * and 24/32-bit integer PCM is repacked to S16 per chunk (`convertFrom`).
 * Compressed audio (MP3/AAC/AC3) has no decoder here — the video plays
 * silent and the log says how to re-encode. */
typedef struct {
    SDL_AudioDeviceID dev;
    SDL_AudioSpec     spec;
    int               convertFrom; /* 0 = none, else source bits (24/32 int PCM) */
} FmvAviAudio;

static int OpenFmvAudio(const ReadAVI::stream_format_auds_t* fmt, FmvAviAudio* out)
{
    memset(out, 0, sizeof(*out));

    if (fmt->samples_per_second == 0 || fmt->channels == 0)
        return -1; /* no audio stream */

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[FMV] Failed to init SDL audio: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = fmt->samples_per_second;
    want.channels = (Uint8)fmt->channels;
    want.samples = 4096;

    if (fmt->format == 3 && fmt->bits_per_sample == 32) {
        want.format = AUDIO_F32LSB;
    } else if (fmt->format == 1 && fmt->bits_per_sample == 16) {
        want.format = AUDIO_S16LSB;
    } else if (fmt->format == 1 && fmt->bits_per_sample == 8) {
        want.format = AUDIO_U8;
    } else if (fmt->format == 1 &&
               (fmt->bits_per_sample == 24 || fmt->bits_per_sample == 32)) {
        want.format = AUDIO_S16LSB;
        out->convertFrom = fmt->bits_per_sample;
    } else {
        printf("[FMV] Unsupported audio (wFormatTag=0x%X, %d bit) — video will play WITHOUT sound.\n"
               "[FMV] Re-encode the audio track as PCM, e.g.: ffmpeg -i in.avi -c:v copy -c:a pcm_s16le out.avi\n",
               fmt->format, fmt->bits_per_sample);
        return -1;
    }

    out->dev = SDL_OpenAudioDevice(NULL, 0, &want, &out->spec, 0);
    if (out->dev == 0) {
        printf("[FMV] Failed to open audio device: %s\n", SDL_GetError());
        return -1;
    }

    printf("[FMV] Audio: %d Hz, %d ch, %d bit %s%s\n",
           fmt->samples_per_second, fmt->channels, fmt->bits_per_sample,
           (fmt->format == 3) ? "float" : "PCM",
           out->convertFrom ? " (repacked to 16-bit)" : "");

    return 0;
}

/* Repack 24/32-bit little-endian integer PCM to S16 (keep the top 16 bits). */
static int ConvertPcmToS16(const unsigned char* src, int src_bytes, int src_bits,
                           unsigned char** out_buf, int* out_bytes)
{
    int bytes_per = src_bits / 8;
    int samples = src_bytes / bytes_per;
    size_t need = (size_t)samples * 2;

    if (need > s_audioConvBufSize) {
        unsigned char* p = (unsigned char*)realloc(s_audioConvBuf, need);
        if (!p) return -1;
        s_audioConvBuf = p;
        s_audioConvBufSize = need;
    }

    int16_t* dst = (int16_t*)s_audioConvBuf;
    for (int i = 0; i < samples; i++) {
        const unsigned char* s = src + (size_t)i * bytes_per;
        /* little-endian: the two most significant bytes are the last two */
        dst[i] = (int16_t)(s[bytes_per - 2] | (s[bytes_per - 1] << 8));
    }

    *out_buf = s_audioConvBuf;
    *out_bytes = samples * 2;
    return 0;
}

/* Open the resolved disc image (any region — PcPort_GetGameDiscPath also
 * region-initializes g_FileTable on first use, exactly like xa_player).
 * Caller owns the FILE*. */
static FILE* OpenDiscImage(void)
{
    const char* path = PcPort_GetGameDiscPath();
    FILE* f = (path && path[0]) ? fopen(path, "rb") : NULL;
    if (!f) {
        printf("[FMV] Failed to open disc image: %s\n",
               (path && path[0]) ? path : "(no disc found)");
    }
    return f;
}

/* Taskbar close / Alt+F4 / window X during an FMV used to only stop the video:
 * all three playback loops treat SDL_QUIT as "bail out of this FMV", then return
 * into the game, swallowing the close request — the window stayed open and the
 * user had to close it again once the movie was gone. Route it to the real close
 * path instead. PsyX_Exit is what PsyCross's own window-close handler calls, so
 * teardown matches Alt+F4 exactly, including the normal-exit flag that stops a
 * benign teardown exception from being logged as FATAL. Does not return. */
extern void PsyX_Exit();

static void FmvQuitNow(const char* where)
{
    printf("[FMV] close requested during %s — quitting\n", where);
    fflush(stdout);
    PsyX_Exit();
}

/* Poll skip keys, drain SDL events, and return 1 if the user wants to bail. */
static int PollSkipOrQuit(int* out_quit)
{
    SDL_PumpEvents();

    SDL_Event evt;
    while (SDL_PollEvent(&evt)) {
        if (evt.type == SDL_QUIT) {
            FmvQuitNow("MDEC playback");
            if (out_quit) *out_quit = 1;
            return 1;
        }
    }

    const Uint8* keystate = SDL_GetKeyboardState(NULL);
    return (keystate[SDL_SCANCODE_RETURN] ||
            keystate[SDL_SCANCODE_ESCAPE] ||
            keystate[SDL_SCANCODE_SPACE] ||
            PsyX_Pad_SkipButtonHeld());
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

    /* Base sector from the live (region-remapped) file table, not the
     * US-baked s_fmvFiles value — PAL places the same byte-identical movies
     * +0x1E88 sectors later. OpenDiscImage() above guarantees the table is
     * region-initialized. n_sectors/name stay table-local (identical across
     * regions; name only feeds the AVI-override lookup). */
    str_stream_t stream;
    if (!str_open(&stream, bin, PcPort_FileTableStartSector(FIRST_XA_FILE_IDX + table_idx), e.n_sectors)) {
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

    /* Skip is armed only after the skip keys have been seen released once:
     * the keystroke that STARTED playback (console Enter, menu confirm) is
     * usually still held on the first loop iterations and would otherwise
     * skip the video at frame 0. */
    int skip_armed = 0;

    while (1) {
        int skipHeld = PollSkipOrQuit(&want_quit);
        if (want_quit) {
            printf("[FMV] Quit at frame %d\n", done_frames);
            break;
        }
        if (!skip_armed) {
            if (!skipHeld)
                skip_armed = 1;
        } else if (skipHeld) {
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
        if (r <= 0) break; /* EOF or demux error */

        if (max_frames > 0 && done_frames >= max_frames) break;

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

        /* Move into the persistent decode buffer for DrawVideoFrame, then
         * blit. We copy because s_decodeBuffer is what DrawVideoFrame
         * uploads to GL — keeping that contract stable. */
        size_t bytes = (size_t)info.width * info.height * 3u;
        if (EnsureDecodeBuffer(bytes) == 0) {
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
     * Confirm on the title; Cross on pad = Confirm too). Same protection as
     * the AVI path. */
    {
        int wait_frames = 0;
        while (wait_frames < 30) {
            SDL_PumpEvents();
            const Uint8* ks = SDL_GetKeyboardState(NULL);
            if (!ks[SDL_SCANCODE_RETURN] && !ks[SDL_SCANCODE_ESCAPE] &&
                !ks[SDL_SCANCODE_SPACE] && !PsyX_Pad_SkipButtonHeld())
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

/* ============================================================================
 * Optional ffmpeg fallback: native h264/h265/vp9/av1 inside mp4/mkv/webm/avi.
 *
 * Compiled in by default (CMake option SH_FMV_FFMPEG), but ffmpeg is loaded at
 * RUNTIME and we ship no ffmpeg binaries: only the headers are a build
 * dependency, nothing is in the exe's import table. Linking normally would make
 * Windows refuse to start the exe for every user who has no ffmpeg DLLs, and we
 * cannot redistribute them (the MSYS2 prebuilt is GPL). So the DLLs are
 * user-supplied — dropped next to the exe or on PATH — and absent DLLs degrade
 * to the disc STR exactly as if the feature were off.
 *
 * This is a FALLBACK BELOW the zero-dependency built-in MJPEG/raw decoder — a
 * modder can drop e.g. gamedata/fmv/<name>.mp4 and it plays natively instead of
 * forcing a huge MJPEG re-encode. Video decodes to RGB24 straight into
 * s_decodeBuffer and presents through DrawVideoFrame; audio decodes to
 * interleaved S16 and streams through the same SDL device + FmvApplyVolume the
 * AVI path uses. Presentation is paced to the container's own PTS.
 * ==========================================================================*/
#if defined(SH_FMV_FFMPEG)
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

extern "C" {
#include "../dll_loader.h"
}

/* We read AVCodecContext/AVFrame fields directly, so a different major version
 * is an ABI break, not a missing-feature problem: refuse rather than crash. */
struct FfmpegApi
{
    int         (*avformat_open_input)(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**);
    int         (*avformat_find_stream_info)(AVFormatContext*, AVDictionary**);
    void        (*avformat_close_input)(AVFormatContext**);
    int         (*av_read_frame)(AVFormatContext*, AVPacket*);
    int         (*av_find_best_stream)(AVFormatContext*, enum AVMediaType, int, int, const AVCodec**, int);
    unsigned    (*avformat_version)(void);

    const AVCodec*   (*avcodec_find_decoder)(enum AVCodecID);
    AVCodecContext*  (*avcodec_alloc_context3)(const AVCodec*);
    void        (*avcodec_free_context)(AVCodecContext**);
    int         (*avcodec_parameters_to_context)(AVCodecContext*, const AVCodecParameters*);
    int         (*avcodec_open2)(AVCodecContext*, const AVCodec*, AVDictionary**);
    int         (*avcodec_send_packet)(AVCodecContext*, const AVPacket*);
    int         (*avcodec_receive_frame)(AVCodecContext*, AVFrame*);
    const char* (*avcodec_get_name)(enum AVCodecID);
    AVPacket*   (*av_packet_alloc)(void);
    void        (*av_packet_free)(AVPacket**);
    void        (*av_packet_unref)(AVPacket*);
    unsigned    (*avcodec_version)(void);

    AVFrame*    (*av_frame_alloc)(void);
    void        (*av_frame_free)(AVFrame**);
    void        (*av_frame_unref)(AVFrame*);
    void        (*av_channel_layout_default)(AVChannelLayout*, int);
    void        (*av_channel_layout_uninit)(AVChannelLayout*);
    unsigned    (*avutil_version)(void);

    struct SwsContext* (*sws_getContext)(int, int, enum AVPixelFormat, int, int, enum AVPixelFormat,
                                         int, SwsFilter*, SwsFilter*, const double*);
    void        (*sws_freeContext)(struct SwsContext*);
    int         (*sws_scale)(struct SwsContext*, const uint8_t* const[], const int[], int, int,
                             uint8_t* const[], const int[]);
    unsigned    (*swscale_version)(void);

    int         (*swr_alloc_set_opts2)(struct SwrContext**,
                                       const AVChannelLayout*, enum AVSampleFormat, int,
                                       const AVChannelLayout*, enum AVSampleFormat, int,
                                       int, void*);
    int         (*swr_init)(struct SwrContext*);
    void        (*swr_free)(struct SwrContext**);
    int         (*swr_convert)(struct SwrContext*, uint8_t**, int, const uint8_t**, int);
    int         (*swr_get_out_samples)(struct SwrContext*, int);
    unsigned    (*swresample_version)(void);
};

static FfmpegApi s_ff;
static int       s_ffState = 0; /* 0 = untried, 1 = usable, -1 = unavailable */

/* Bare names first: the OS then searches the exe's directory, so a user can
 * either drop the DLLs next to the game or rely on a system-wide ffmpeg.
 * On macOS a bare name only reaches the DYLD fallback paths (~/lib, /usr/local/lib,
 * /usr/lib), which miss Homebrew's Apple Silicon prefix — so those are listed
 * explicitly as absolute-path candidates. */
#if defined(_WIN32)
#define FF_LIB_NAMES(base, ver) { base "-" ver ".dll", "lib" base "-" ver ".dll" }
#elif defined(__APPLE__)
#define FF_LIB_NAMES(base, ver)                        \
    { "lib" base "." ver ".dylib",                     \
      "/opt/homebrew/lib/lib" base "." ver ".dylib",   \
      "/usr/local/lib/lib" base "." ver ".dylib" }
#else
#define FF_LIB_NAMES(base, ver) { "lib" base ".so." ver }
#endif

/* The soname we search for and the ABI major we validate must both come from the
 * headers we compiled against, or the two can disagree and no library the user
 * supplies could ever satisfy both at once. */
#define FF_STR2(x) #x
#define FF_STR(x)  FF_STR2(x)

#define FF_V_AVFORMAT   FF_STR(LIBAVFORMAT_VERSION_MAJOR)
#define FF_V_AVCODEC    FF_STR(LIBAVCODEC_VERSION_MAJOR)
#define FF_V_AVUTIL     FF_STR(LIBAVUTIL_VERSION_MAJOR)
#define FF_V_SWSCALE    FF_STR(LIBSWSCALE_VERSION_MAJOR)
#define FF_V_SWRESAMPLE FF_STR(LIBSWRESAMPLE_VERSION_MAJOR)

#define FF_ID_AVFORMAT   "avformat-"   FF_V_AVFORMAT
#define FF_ID_AVCODEC    "avcodec-"    FF_V_AVCODEC
#define FF_ID_AVUTIL     "avutil-"     FF_V_AVUTIL
#define FF_ID_SWSCALE    "swscale-"    FF_V_SWSCALE
#define FF_ID_SWRESAMPLE "swresample-" FF_V_SWRESAMPLE

static DllHandle FfmpegOpenLib(const char* const* names, int count)
{
    for (int i = 0; i < count; i++) {
        DllHandle h = DllLoader_Open(names[i]);
        if (h)
            return h;
    }
    return NULL;
}

static int FfmpegCheckMajor(const char* lib, unsigned got, int want)
{
    if ((int)(got >> 16) == want)
        return 0;
    printf("[FMV] ffmpeg: %s major version %u, need %d — refusing to use ffmpeg (ABI mismatch)\n",
           lib, got >> 16, want);
    return -1;
}

static int FfmpegLoad(void)
{
    static const char* const avformatNames[]   = FF_LIB_NAMES("avformat",   FF_V_AVFORMAT);
    static const char* const avcodecNames[]    = FF_LIB_NAMES("avcodec",    FF_V_AVCODEC);
    static const char* const avutilNames[]     = FF_LIB_NAMES("avutil",     FF_V_AVUTIL);
    static const char* const swscaleNames[]    = FF_LIB_NAMES("swscale",    FF_V_SWSCALE);
    static const char* const swresampleNames[] = FF_LIB_NAMES("swresample", FF_V_SWRESAMPLE);
#define FF_NAMES(a) a, (int)(sizeof(a) / sizeof((a)[0]))

    DllHandle   hFormat = NULL, hCodec = NULL, hUtil = NULL, hScale = NULL, hResample = NULL;
    const char* missing = NULL;

    memset(&s_ff, 0, sizeof(s_ff));

    hFormat   = FfmpegOpenLib(FF_NAMES(avformatNames));
    hCodec    = FfmpegOpenLib(FF_NAMES(avcodecNames));
    hUtil     = FfmpegOpenLib(FF_NAMES(avutilNames));
    hScale    = FfmpegOpenLib(FF_NAMES(swscaleNames));
    hResample = FfmpegOpenLib(FF_NAMES(swresampleNames));
#undef FF_NAMES

    if (!hFormat || !hCodec || !hUtil || !hScale || !hResample) {
        printf("[FMV] ffmpeg runtime not found (missing:%s%s%s%s%s) — container overrides disabled\n",
               hFormat   ? "" : " " FF_ID_AVFORMAT,
               hCodec    ? "" : " " FF_ID_AVCODEC,
               hUtil     ? "" : " " FF_ID_AVUTIL,
               hScale    ? "" : " " FF_ID_SWSCALE,
               hResample ? "" : " " FF_ID_SWRESAMPLE);
        printf("[FMV] To play mp4/mkv/webm overrides, put the matching ffmpeg shared libraries\n"
               "[FMV] (" FF_ID_AVCODEC ", " FF_ID_AVFORMAT ", " FF_ID_AVUTIL ", "
               FF_ID_SWSCALE ", " FF_ID_SWRESAMPLE ") next to the game exe or on PATH.\n");
        goto fail;
    }

#define FF_SYM(handle, name)                                                     \
    do {                                                                         \
        *(void**)&s_ff.name = DllLoader_GetSymbol(handle, #name);                \
        if (!s_ff.name) { missing = #name; goto missing_symbol; }                \
    } while (0)

    FF_SYM(hFormat, avformat_open_input);
    FF_SYM(hFormat, avformat_find_stream_info);
    FF_SYM(hFormat, avformat_close_input);
    FF_SYM(hFormat, av_read_frame);
    FF_SYM(hFormat, av_find_best_stream);
    FF_SYM(hFormat, avformat_version);

    FF_SYM(hCodec, avcodec_find_decoder);
    FF_SYM(hCodec, avcodec_alloc_context3);
    FF_SYM(hCodec, avcodec_free_context);
    FF_SYM(hCodec, avcodec_parameters_to_context);
    FF_SYM(hCodec, avcodec_open2);
    FF_SYM(hCodec, avcodec_send_packet);
    FF_SYM(hCodec, avcodec_receive_frame);
    FF_SYM(hCodec, avcodec_get_name);
    FF_SYM(hCodec, av_packet_alloc);
    FF_SYM(hCodec, av_packet_free);
    FF_SYM(hCodec, av_packet_unref);
    FF_SYM(hCodec, avcodec_version);

    FF_SYM(hUtil, av_frame_alloc);
    FF_SYM(hUtil, av_frame_free);
    FF_SYM(hUtil, av_frame_unref);
    FF_SYM(hUtil, av_channel_layout_default);
    FF_SYM(hUtil, av_channel_layout_uninit);
    FF_SYM(hUtil, avutil_version);

    FF_SYM(hScale, sws_getContext);
    FF_SYM(hScale, sws_freeContext);
    FF_SYM(hScale, sws_scale);
    FF_SYM(hScale, swscale_version);

    FF_SYM(hResample, swr_alloc_set_opts2);
    FF_SYM(hResample, swr_init);
    FF_SYM(hResample, swr_free);
    FF_SYM(hResample, swr_convert);
    FF_SYM(hResample, swr_get_out_samples);
    FF_SYM(hResample, swresample_version);
#undef FF_SYM

    if (FfmpegCheckMajor("avformat",   s_ff.avformat_version(),   LIBAVFORMAT_VERSION_MAJOR) != 0 ||
        FfmpegCheckMajor("avcodec",    s_ff.avcodec_version(),    LIBAVCODEC_VERSION_MAJOR) != 0 ||
        FfmpegCheckMajor("avutil",     s_ff.avutil_version(),     LIBAVUTIL_VERSION_MAJOR) != 0 ||
        FfmpegCheckMajor("swscale",    s_ff.swscale_version(),    LIBSWSCALE_VERSION_MAJOR) != 0 ||
        FfmpegCheckMajor("swresample", s_ff.swresample_version(), LIBSWRESAMPLE_VERSION_MAJOR) != 0)
        goto version_mismatch;

    printf("[FMV] ffmpeg runtime loaded (avcodec %u, avformat %u)\n",
           s_ff.avcodec_version() >> 16, s_ff.avformat_version() >> 16);
    return 0;

missing_symbol:
    printf("[FMV] ffmpeg: symbol '%s' missing — container overrides disabled\n", missing);
    printf("[FMV] The ffmpeg libraries that loaded are not a complete build of the expected set.\n");
    goto fail;

version_mismatch:
    printf("[FMV] ffmpeg was found but is the wrong major version — container overrides disabled.\n");
    printf("[FMV] This build requires " FF_ID_AVCODEC ", " FF_ID_AVFORMAT ", " FF_ID_AVUTIL ", "
           FF_ID_SWSCALE ", " FF_ID_SWRESAMPLE " (see the per-library lines above).\n");

fail:
    memset(&s_ff, 0, sizeof(s_ff));
    DllLoader_Close(hFormat);
    DllLoader_Close(hCodec);
    DllLoader_Close(hUtil);
    DllLoader_Close(hScale);
    DllLoader_Close(hResample);
    return -1;
}

static int FfmpegAvailable(void)
{
    if (s_ffState == 0)
        s_ffState = (FfmpegLoad() == 0) ? 1 : -1;
    return s_ffState > 0;
}

/* Route the decoder body below through the loaded table instead of the (absent)
 * import stubs. av_q2d stays a real call — it is a static inline in rational.h. */
#define avformat_open_input           s_ff.avformat_open_input
#define avformat_find_stream_info     s_ff.avformat_find_stream_info
#define avformat_close_input          s_ff.avformat_close_input
#define av_read_frame                 s_ff.av_read_frame
#define av_find_best_stream           s_ff.av_find_best_stream
#define avcodec_find_decoder          s_ff.avcodec_find_decoder
#define avcodec_alloc_context3        s_ff.avcodec_alloc_context3
#define avcodec_free_context          s_ff.avcodec_free_context
#define avcodec_parameters_to_context s_ff.avcodec_parameters_to_context
#define avcodec_open2                 s_ff.avcodec_open2
#define avcodec_send_packet           s_ff.avcodec_send_packet
#define avcodec_receive_frame         s_ff.avcodec_receive_frame
#define avcodec_get_name              s_ff.avcodec_get_name
#define av_packet_alloc               s_ff.av_packet_alloc
#define av_packet_free                s_ff.av_packet_free
#define av_packet_unref               s_ff.av_packet_unref
#define av_frame_alloc                s_ff.av_frame_alloc
#define av_frame_free                 s_ff.av_frame_free
#define av_frame_unref                s_ff.av_frame_unref
#define av_channel_layout_default     s_ff.av_channel_layout_default
#define av_channel_layout_uninit      s_ff.av_channel_layout_uninit
#define sws_getContext                s_ff.sws_getContext
#define sws_freeContext               s_ff.sws_freeContext
#define sws_scale                     s_ff.sws_scale
#define swr_alloc_set_opts2           s_ff.swr_alloc_set_opts2
#define swr_init                      s_ff.swr_init
#define swr_free                      s_ff.swr_free
#define swr_convert                   s_ff.swr_convert
#define swr_get_out_samples           s_ff.swr_get_out_samples

static int FindContainerFile(const char* basename, char* out_path, int out_path_size)
{
    const char* dirs[] = { "gamedata/fmv/", "../gamedata/fmv/", "" };
    const char* exts[] = { ".mp4", ".MP4", ".mkv", ".MKV",
                           ".webm", ".WEBM", ".mov", ".MOV" };
    for (int d = 0; d < 3; d++) {
        for (int e = 0; e < (int)(sizeof(exts) / sizeof(exts[0])); e++) {
            snprintf(out_path, out_path_size, "%s%s%s", dirs[d], basename, exts[e]);
            FILE* f = fopen(out_path, "rb");
            if (f) { fclose(f); return 0; }
        }
        if (dirs[d][0] == '\0') break;
    }
    return -1;
}

/* Poll the same skip inputs the AVI/BIN paths use. */
static int FfmpegSkipHeld(void)
{
    SDL_PumpEvents();
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    return ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_ESCAPE] ||
           ks[SDL_SCANCODE_SPACE] || PsyX_Pad_SkipButtonHeld();
}

/* Returns 0 if the file was played (or skipped) — caller must NOT fall back to
 * the disc STR — or -1 if it could not be opened/decoded (caller falls back). */
static int PlayFromFFmpeg(const char* path)
{
    AVFormatContext*   fmt = NULL;
    AVCodecContext*    vctx = NULL;
    AVCodecContext*    actx = NULL;
    struct SwsContext* sws = NULL;
    SwrContext*        swr = NULL;
    AVPacket*          pkt = NULL;
    AVFrame*           frm = NULL;
    SDL_AudioDeviceID  audioDev = 0;
    uint8_t*           audioBuf = NULL;
    int                audioBufBytes = 0;
    int                vs = -1, as = -1, ach = 0;
    int                played = -1;

    if (!FfmpegAvailable())
        return -1;

    if (avformat_open_input(&fmt, path, NULL, NULL) != 0) {
        printf("[FMV] ffmpeg: cannot open '%s'\n", path);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        printf("[FMV] ffmpeg: no stream info in '%s'\n", path);
        goto cleanup;
    }

    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) {
        printf("[FMV] ffmpeg: no video stream in '%s'\n", path);
        goto cleanup;
    }
    as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    {
        const AVCodec* vc = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
        if (!vc) { printf("[FMV] ffmpeg: no video decoder built in\n"); goto cleanup; }
        vctx = avcodec_alloc_context3(vc);
        if (!vctx) goto cleanup;
        avcodec_parameters_to_context(vctx, fmt->streams[vs]->codecpar);
        if (avcodec_open2(vctx, vc, NULL) < 0) {
            printf("[FMV] ffmpeg: cannot open video decoder\n"); goto cleanup;
        }
    }

    if (as >= 0) {
        const AVCodec* ac = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
        if (ac) {
            actx = avcodec_alloc_context3(ac);
            if (actx) {
                avcodec_parameters_to_context(actx, fmt->streams[as]->codecpar);
                if (avcodec_open2(actx, ac, NULL) < 0) {
                    avcodec_free_context(&actx);
                    actx = NULL;
                }
            }
        }
        if (!actx) {
            printf("[FMV] ffmpeg: audio decoder unavailable — playing silent\n");
            as = -1;
        }
    }

    printf("[FMV] ffmpeg: %dx%d %s%s\n", vctx->width, vctx->height,
           avcodec_get_name(vctx->codec_id), (as >= 0) ? " + audio" : "");

    FMV_Init();

    sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt,
                         vctx->width, vctx->height, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { printf("[FMV] ffmpeg: sws_getContext failed\n"); goto cleanup; }

    if (as >= 0) {
        ach = actx->ch_layout.nb_channels;
        /* The game runs its own audio through OpenAL, so SDL's audio subsystem is
         * never brought up at startup — open it here exactly as the AVI/STR paths
         * do, or SDL_OpenAudioDevice fails and every container plays silent. */
        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
            SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want, got;
        SDL_memset(&want, 0, sizeof(want));
        want.freq = actx->sample_rate;
        want.channels = (Uint8)ach;
        want.format = AUDIO_S16LSB;
        want.samples = 4096;
        audioDev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (audioDev) {
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, ach);
            if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, actx->sample_rate,
                                    &actx->ch_layout, actx->sample_fmt, actx->sample_rate,
                                    0, NULL) < 0 ||
                swr_init(swr) < 0) {
                printf("[FMV] ffmpeg: swresample init failed — silent\n");
                if (swr) swr_free(&swr);
                SDL_CloseAudioDevice(audioDev);
                audioDev = 0;
            } else {
                SDL_PauseAudioDevice(audioDev, 0);
            }
            av_channel_layout_uninit(&outLayout);
        } else {
            printf("[FMV] ffmpeg: audio device open failed — silent\n");
        }
    }

    pkt = av_packet_alloc();
    frm = av_frame_alloc();
    if (!pkt || !frm) goto cleanup;

    /* From here the file is ours: even a skip counts as "played", so the caller
     * must not also run the disc STR. */
    played = 0;

    {
        timerCtx_t tmr;
        Util_InitHPCTimer(&tmr);
        double   elapsed = 0.0;
        double   vtb = av_q2d(fmt->streams[vs]->time_base);
        int64_t  firstPts = AV_NOPTS_VALUE;
        int      skip_armed = 0;
        int      stop = 0;   /* skip or SDL_QUIT */
        int      reading = 1;

        SDL_PumpEvents();
        SDL_FlushEvent(SDL_KEYDOWN);
        SDL_FlushEvent(SDL_KEYUP);
        Util_GetHPCTime(&tmr, 1); /* zero the clock */

        while (!stop) {
            int haveVideo = 0, haveAudio = 0;
            if (reading) {
                int rr = av_read_frame(fmt, pkt);
                if (rr < 0) {
                    /* Drain: flush both decoders, then finish. */
                    avcodec_send_packet(vctx, NULL);
                    if (actx) avcodec_send_packet(actx, NULL);
                    reading = 0;
                    haveVideo = 1;
                    haveAudio = (actx != NULL);
                } else if (pkt->stream_index == vs) {
                    if (avcodec_send_packet(vctx, pkt) >= 0) haveVideo = 1;
                    av_packet_unref(pkt);
                } else if (as >= 0 && pkt->stream_index == as) {
                    if (avcodec_send_packet(actx, pkt) >= 0) haveAudio = 1;
                    av_packet_unref(pkt);
                } else {
                    av_packet_unref(pkt);
                    continue;
                }
            } else {
                break; /* decoders already flushed below on the EOF pass */
            }

            /* Audio: convert everything available and queue it ahead. */
            if (haveAudio && audioDev) {
                while (avcodec_receive_frame(actx, frm) == 0) {
                    int outMax = (int)swr_get_out_samples(swr, frm->nb_samples);
                    int need = outMax * ach * (int)sizeof(int16_t);
                    if (need > audioBufBytes) {
                        uint8_t* nb = (uint8_t*)realloc(audioBuf, (size_t)need);
                        if (nb) { audioBuf = nb; audioBufBytes = need; }
                    }
                    /* A failed realloc leaves audioBuf at its old, smaller size —
                     * converting into it with the larger outMax would overflow the
                     * heap, so drop the frame instead. */
                    if (audioBuf && audioBufBytes >= need) {
                        uint8_t* out[1] = { audioBuf };
                        int n = swr_convert(swr, out, outMax,
                                            (const uint8_t**)frm->extended_data, frm->nb_samples);
                        if (n > 0) {
                            int bytes = n * ach * (int)sizeof(int16_t);
                            FmvApplyVolume(audioBuf, bytes, AUDIO_S16LSB);
                            SDL_QueueAudio(audioDev, audioBuf, (Uint32)bytes);
                        }
                    }
                    av_frame_unref(frm);
                }
            }

            /* Video: pace each frame to its PTS, then present. */
            if (haveVideo) {
                while (avcodec_receive_frame(vctx, frm) == 0) {
                    int64_t pts = (frm->pts != AV_NOPTS_VALUE) ? frm->pts : frm->best_effort_timestamp;
                    if (firstPts == AV_NOPTS_VALUE)
                        firstPts = (pts != AV_NOPTS_VALUE) ? pts : 0;
                    double target = (pts != AV_NOPTS_VALUE) ? (double)(pts - firstPts) * vtb : elapsed;

                    /* Poll skip + pump events at least ONCE per frame, even when the
                     * frame is already late (elapsed >= target). The old `while (elapsed
                     * < target)` skipped this entirely for a slow-decoding movie, so it
                     * hammered full-speed with no event pump — the window stopped
                     * responding and the movie couldn't be skipped ("locks the system"). */
                    for (;;) {
                        int held = FfmpegSkipHeld();
                        if (!skip_armed) { if (!held) skip_armed = 1; }
                        else if (held) { printf("[FMV] ffmpeg: skipped\n"); stop = 1; }
                        SDL_Event ev;
                        while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) FmvQuitNow("ffmpeg playback");
                        if (stop || elapsed >= target) break;
                        SDL_Delay(1);
                        elapsed += Util_GetHPCTime(&tmr, 1);
                    }
                    if (stop) { av_frame_unref(frm); break; }

                    /* A frame that has fallen well behind realtime is dropped so a slow
                     * decode catches up (plays near real speed, just choppier) instead of
                     * crawling through every frame at 100% CPU. */
                    if (reading && target < elapsed - 0.10) { av_frame_unref(frm); continue; }

                    if (EnsureDecodeBuffer((size_t)vctx->width * vctx->height * 3u) == 0) {
                        uint8_t* dst[4] = { s_decodeBuffer, NULL, NULL, NULL };
                        int      dstStride[4] = { vctx->width * 3, 0, 0, 0 };
                        sws_scale(sws, frm->data, frm->linesize, 0, vctx->height, dst, dstStride);
                        DrawVideoFrame(vctx->width, vctx->height);
                    }
                    elapsed += Util_GetHPCTime(&tmr, 1);
                    av_frame_unref(frm);
                }
            }

            if (!reading) break; /* EOF pass done */
        }

        /* Match the AVI/BIN paths: don't let the key that skipped/ended the movie
         * bleed a phantom Confirm into the next game state. */
        if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
        for (int w = 0; w < 30 && FfmpegSkipHeld(); w++)
            SDL_Delay(16);
        SDL_PumpEvents();
        SDL_FlushEvent(SDL_KEYDOWN);
        SDL_FlushEvent(SDL_KEYUP);
    }

    printf("[FMV] ffmpeg playback complete: %s\n", path);

cleanup:
    if (audioBuf) ::free(audioBuf);
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    if (frm) av_frame_free(&frm);
    if (pkt) av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    if (swr) swr_free(&swr);
    if (vctx) avcodec_free_context(&vctx);
    if (actx) avcodec_free_context(&actx);
    if (fmt) avformat_close_input(&fmt);
    return played;
}

/* Confine the dispatch-table redirection to the decoder body above — anything
 * below (or any later #include) must see the real names again. */
#undef avformat_open_input
#undef avformat_find_stream_info
#undef avformat_close_input
#undef av_read_frame
#undef av_find_best_stream
#undef avcodec_find_decoder
#undef avcodec_alloc_context3
#undef avcodec_free_context
#undef avcodec_parameters_to_context
#undef avcodec_open2
#undef avcodec_send_packet
#undef avcodec_receive_frame
#undef avcodec_get_name
#undef av_packet_alloc
#undef av_packet_free
#undef av_packet_unref
#undef av_frame_alloc
#undef av_frame_free
#undef av_frame_unref
#undef av_channel_layout_default
#undef av_channel_layout_uninit
#undef sws_getContext
#undef sws_freeContext
#undef sws_scale
#undef swr_alloc_set_opts2
#undef swr_init
#undef swr_free
#undef swr_convert
#undef swr_get_out_samples
#endif /* SH_FMV_FFMPEG */

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
#if defined(SH_FMV_FFMPEG)
        /* No .avi override — look for a modern container (mp4/mkv/webm/mov) and
         * decode it with ffmpeg before falling back to the disc movie. */
        char cpath[512];
        if (FindContainerFile(basename, cpath, sizeof(cpath)) == 0) {
            printf("[FMV] Container override: %s\n", cpath);
            if (PlayFromFFmpeg(cpath) == 0)
                return 0;
            printf("[FMV] ffmpeg failed for '%s' — decoding from BIN\n", cpath);
            return PlayFromBin(table_idx, max_frames);
        }
#endif
        printf("[FMV] No AVI override for '%s' — decoding from BIN\n", basename);
        return PlayFromBin(table_idx, max_frames);
    }

    printf("[FMV] Playing: %s\n", filepath);

    FMV_Init();

    ReadAVI readAVI(filepath);
    if (!readAVI.IsOpen()) {
        printf("[FMV] Failed to parse AVI '%s' — falling back to the disc movie\n", filepath);
#if defined(SH_FMV_FFMPEG)
        /* A container misnamed .avi (or an AVI the built-in parser rejects) can
         * still open through ffmpeg. */
        if (PlayFromFFmpeg(filepath) == 0)
            return 0;
#endif
        return PlayFromBin(table_idx, max_frames);
    }

    ReadAVI::avi_header_t avi_header = readAVI.GetAviHeader();
    ReadAVI::stream_format_t stream_format = readAVI.GetVideoFormat();

    FmvCodec codec = IdentifyCodec(&stream_format);
    if (codec == FMV_CODEC_UNSUPPORTED) {
        printf("[FMV] Unsupported video codec '%.4s' (%d bpp) in %s — falling back to the disc movie.\n"
               "[FMV] Supported: MJPEG (MJPG/dmb1/jpeg), uncompressed RGB, YUY2/UYVY/I420/YV12/NV12.\n"
               "[FMV] Re-encode with: ffmpeg -i in.mp4 -c:v mjpeg -q:v 3 -c:a pcm_s16le out.avi\n",
               stream_format.compression_type, stream_format.bits_per_pixel, filepath);
#if defined(SH_FMV_FFMPEG)
        /* h264/h265/etc. inside an AVI wrapper: hand the same file to ffmpeg. */
        if (PlayFromFFmpeg(filepath) == 0)
            return 0;
#endif
        return PlayFromBin(table_idx, max_frames);
    }

    printf("[FMV] Video: %dx%d '%.4s', %d frames indexed, %.1f fps\n",
           stream_format.image_width, stream_format.image_height,
           stream_format.compression_type[0] ? stream_format.compression_type : "RGB",
           avi_header.TotalNumberOfFrames,
           avi_header.TimeBetweenFrames > 0 ? 1000000.0 / avi_header.TimeBetweenFrames : 0);

    /* Set up audio */
    ReadAVI::stream_format_auds_t audio_fmt = readAVI.GetAudioFormat();
    FmvAviAudio aviAudio;
    SDL_AudioDeviceID audioDev = (OpenFmvAudio(&audio_fmt, &aviAudio) == 0) ? aviAudio.dev : 0;

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

    /* Skip is armed only after the skip keys have been seen released once —
     * see PlayFromBin for why (keystroke that started playback still held). */
    int skip_armed = 0;

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
        int skipHeld = keystate[SDL_SCANCODE_RETURN] || keystate[SDL_SCANCODE_ESCAPE] ||
                       keystate[SDL_SCANCODE_SPACE] || PsyX_Pad_SkipButtonHeld();
        if (!skip_armed) {
            if (!skipHeld)
                skip_armed = 1;
        } else if (skipHeld) {
            printf("[FMV] Skipped at frame %d/%d\n", done_frames, avi_header.TotalNumberOfFrames);
            break;
        }

        /* Drain event queue to keep window responsive */
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) {
                FmvQuitNow("AVI playback");
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
                unsigned char* abuf = frame_entry.buf;
                int abytes = frame_size;

                if (aviAudio.convertFrom != 0 &&
                    ConvertPcmToS16(frame_entry.buf, frame_size, aviAudio.convertFrom,
                                    &abuf, &abytes) != 0)
                    continue;

                FmvApplyVolume(abuf, abytes, aviAudio.spec.format);
                SDL_QueueAudio(audioDev, abuf, abytes);
            }
            /* Don't apply video timing for audio frames - immediately read next */
            continue;
        }

        /* Video frame. Unlike the BIN/STR path, an AVI override is a standalone
         * file the user encoded to replace this cutscene — play it to its own
         * EOF (GetFrameFromIndex returns < 0 when the movi index is exhausted).
         * max_frames here is the ORIGINAL STR's frame count (PSX ~15 fps); a
         * re-encode at a higher fps or a different length has more video frames,
         * so applying that cap cut the custom AVI off early. */

        if (frame_size > 0 &&
            (frame_entry.type == ReadAVI::ctype_compressed_video_frame ||
             frame_entry.type == ReadAVI::ctype_uncompressed_video_frame))
        {
            int real_w, real_h;
            if (DecodeVideoFrame(codec, frame_entry.buf, (unsigned)frame_size,
                                 &stream_format, &real_w, &real_h) == 0)
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
     * 0→held edge as a fresh clickedBtnFlags and bleeds into the main menu (e.g.
     * Enter to skip intro = phantom Confirm on the title). Capped so a stuck
     * key can't hang the boot. */
    {
        int wait_frames = 0;
        while (wait_frames < 30) /* ~500ms at 60fps */
        {
            SDL_PumpEvents();
            const Uint8* ks = SDL_GetKeyboardState(NULL);
            if (!ks[SDL_SCANCODE_RETURN] && !ks[SDL_SCANCODE_ESCAPE] &&
                !ks[SDL_SCANCODE_SPACE] && !PsyX_Pad_SkipButtonHeld())
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
