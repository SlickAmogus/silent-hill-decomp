/*
 * fmv_player.cpp - PC FMV playback for Silent Hill
 *
 * Plays pre-converted AVI (MJPG) files using libjpeg + OpenGL.
 * Adapted from REDRIVER2's VideoPlayer (BSD licensed).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "fmv_player.h"
#include "ReadAVI.h"

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

/* File ID to filename mapping.
 * The numbers after the prefix are PSX sector counts from the file table.
 * Users convert STR files from disc to AVI using jPSXdec. */
typedef struct {
    const char* name;   /* base filename without extension */
} FmvFileEntry;

/* Map of XA file names extracted from the file enum.
 * Index 0 = first XA file in the enum. The caller passes
 * a file_idx from fileenum.h; we subtract the base to index here. */
static const FmvFileEntry s_fmvFiles[] = {
    { "05_02152" },  /* 0  - in-game cutscene */
    { "10_04432" },  /* 1 */
    { "15_07496" },  /* 2 */
    { "20_06552" },  /* 3 */
    { "25_03904" },  /* 4 */
    { "30_04056" },  /* 5 */
    { "35_26008" },  /* 6 */
    { "40_10384" },  /* 7 */
    { "45_28784" },  /* 8 */
    { "C1_20670" },  /* 9  - intro (US) */
    { "C2_20670" },  /* 10 - intro (JP) */
    { "M1_03500" },  /* 11 - opening */
    { "M2_01190" },  /* 12 */
    { "M3_02570" },  /* 13 */
    { "M4_02490" },  /* 14 */
    { "M5_03140" },  /* 15 */
    { "M6_02112" },  /* 16 */
    { "M7_01536" },  /* 17 */
    { "M8_03039" },  /* 18 */
    { "M9_01730" },  /* 19 */
    { "MA_03590" },  /* 20 */
    { "MB_04850" },  /* 21 */
    { "MC_01930" },  /* 22 */
    { "MD_03780" },  /* 23 */
    { "ME_03300" },  /* 24 */
    { "Z1_16180" },  /* 25 */
    { "Z3_02340" },  /* 26 */
    { "Z4_01590" },  /* 27 */
    { "ZC_14392" },  /* 28 */
    { "ZZ_14239" },  /* 29 */
};

#define FMV_FILE_COUNT (sizeof(s_fmvFiles) / sizeof(s_fmvFiles[0]))

/* First XA file index in the file enum (FILE_XA_05_02152 = 2044 across versions) */
#define FIRST_XA_FILE_IDX 2044

/* GL resources */
static GLuint s_fmvTexture = 0;
static GLuint s_fmvShader = 0;

/* Decode buffer - large enough for 1080p RGB */
#define DECODE_BUFFER_SIZE (1920 * 1080 * 3)
static unsigned char* s_decodeBuffer = NULL;

/* Simple fullscreen blit shader */
static const char* s_fmvShaderSrc =
    "varying vec4 v_texcoord;\n"
    "#ifdef VERTEX\n"
    "   attribute vec4 a_position;\n"
    "   attribute vec4 a_texcoord;\n"
    "   void main() {\n"
    "       v_texcoord = a_texcoord;\n"
    "       gl_Position = vec4(a_position.xy, 0.0, 1.0);\n"
    "   }\n"
    "#else\n"
    "   uniform sampler2D s_texture;\n"
    "   void main() {\n"
    "       fragColor = texture2D(s_texture, v_texcoord.xy);\n"
    "   }\n"
    "#endif\n";

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

static void SetupBlitQuad(int image_w, int image_h)
{
    int windowWidth, windowHeight;
    PsyX_GetScreenSize(&windowWidth, &windowHeight);

    float psxScreenW = 320.0f;
    float psxScreenH = 240.0f;

    float video_aspect = (float)image_w / (float)image_h;
    float window_aspect = (float)windowWidth / (float)windowHeight;

    /* Fit video to window while maintaining aspect ratio */
    float scaleX, scaleY;
    if (video_aspect > window_aspect) {
        scaleX = 1.0f;
        scaleY = window_aspect / video_aspect;
    } else {
        scaleX = video_aspect / window_aspect;
        scaleY = 1.0f;
    }

    GR_SetViewPort(0, 0, windowWidth, windowHeight);

    GrVertex blit_vertices[] = {
        {  (short)(scaleX * 32767),  (short)(scaleY * 32767),   0, 0,    1, 0,   0, 0,   0, 0,   0, 0 },
        { (short)(-scaleX * 32767), (short)(-scaleY * 32767),   0, 0,    0, 1,   0, 0,   0, 0,   0, 0 },
        { (short)(-scaleX * 32767),  (short)(scaleY * 32767),   0, 0,    0, 0,   0, 0,   0, 0,   0, 0 },

        {  (short)(scaleX * 32767), (short)(-scaleY * 32767),   0, 0,    1, 1,   0, 0,   0, 0,   0, 0 },
        { (short)(-scaleX * 32767), (short)(-scaleY * 32767),   0, 0,    0, 1,   0, 0,   0, 0,   0, 0 },
        {  (short)(scaleX * 32767),  (short)(scaleY * 32767),   0, 0,    1, 0,   0, 0,   0, 0,   0, 0 },
    };

    GR_UpdateVertexBuffer(blit_vertices, 6);
}

static void DrawVideoFrame(int image_w, int image_h)
{
    int windowWidth, windowHeight;
    PsyX_GetScreenSize(&windowWidth, &windowHeight);

    PsyX_BeginScene();
    GR_Clear(0, 0, windowWidth, windowHeight, 0, 0, 0);

    glBindTexture(GL_TEXTURE_2D, s_fmvTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, image_w, image_h, 0, GL_RGB, GL_UNSIGNED_BYTE, s_decodeBuffer);
    glBindTexture(GL_TEXTURE_2D, 0);

    GR_SetShader(s_fmvShader);
    GR_SetTexture(s_fmvTexture, (TexFormat)-1);
    GR_SetScissorState(0);
    GR_EnableDepth(0);
    GR_SetStencilMode(0);
    GR_SetBlendMode(BM_NONE);

    SetupBlitQuad(image_w, image_h);
    GR_DrawTriangles(0, 2);

    PsyX_EndScene();
}

/* Try to find AVI file in several locations */
static int FindAviFile(const char* basename, char* out_path, int out_path_size)
{
    const char* search_dirs[] = {
        "data/FMV/",
        "DATA/FMV/",
        "../data/FMV/",
        "../DATA/FMV/",
        "FMV/",
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

    if (!s_fmvShader)
        s_fmvShader = GR_Shader_Compile(s_fmvShaderSrc);
}

extern "C" void FMV_Shutdown(void)
{
    if (s_decodeBuffer) {
        ::free(s_decodeBuffer);
        s_decodeBuffer = NULL;
    }
    if (s_fmvTexture) {
        GR_DestroyTexture(s_fmvTexture);
        s_fmvTexture = 0;
    }
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

    if (FindAviFile(basename, filepath, sizeof(filepath)) != 0) {
        printf("[FMV] AVI file not found for '%s' (file_idx=%d)\n", basename, file_idx);
        return -1;
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

    /* Set up display environment for movie playback */
    DISPENV movie_disp;
    DRAWENV movie_draw;

    SetDefDispEnv(&movie_disp, 0, 0, 320, 240);
    SetDefDrawEnv(&movie_draw, 0, 0, 320, 240);
    movie_draw.dfe = 1;

    PutDispEnv(&movie_disp);
    PutDrawEnv(&movie_draw);

    ReadAVI::frame_entry_t frame_entry;
    frame_entry.type = (ReadAVI::chunk_type_t)(ReadAVI::ctype_video_data);
    frame_entry.pointer = 0;

    timerCtx_t fmvTimer;
    Util_InitHPCTimer(&fmvTimer);

    double nextFrameDelay = 0.0;
    int done_frames = 0;

    Util_GetHPCTime(&fmvTimer, 1);

    /* Main playback loop */
    while (1)
    {
        double delta = Util_GetHPCTime(&fmvTimer, 1);
        if (delta > 1.0)
            delta = 0.0;

        nextFrameDelay -= delta;

        if (nextFrameDelay > 0) {
            SDL_Delay(1);
            continue;
        }

        frame_entry.type = (ReadAVI::chunk_type_t)(ReadAVI::ctype_video_data);
        int frame_size = readAVI.GetFrameFromIndex(&frame_entry);

        if (frame_size < 0)
            break;

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

        /* Check for skip input */
        PsyX_UpdateInput();
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT)
                return 0;
            if (evt.type == SDL_KEYDOWN) {
                /* Skip on Enter, Escape, or Space */
                if (evt.key.keysym.sym == SDLK_RETURN ||
                    evt.key.keysym.sym == SDLK_ESCAPE ||
                    evt.key.keysym.sym == SDLK_SPACE)
                {
                    printf("[FMV] Skipped at frame %d/%d\n", done_frames, avi_header.TotalNumberOfFrames);
                    goto done;
                }
            }
        }
    }

done:
    printf("[FMV] Playback complete (%d frames)\n", done_frames);
    return 0;
}
