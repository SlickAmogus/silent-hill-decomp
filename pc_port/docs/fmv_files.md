# Silent Hill FMV File Table

> Part of modding/extraction. For the disc-unpacking pipeline, VAB audio, and the
> loose-file override mechanism, see [`Modding_And_Extraction_Guide.md`](Modding_And_Extraction_Guide.md).

Silent Hill (PSX) stores all 21 of its FMVs on the disc as raw MDEC video
streams interleaved with XA audio. To get them onto the PC port, extract
them with **jPSXdec** (https://github.com/m35/jpsxdec/releases/tag/v2.0)
and drop them into `gamedata/FMV/`.

jPSXdec lists every stream as `HILL[xxx]`. Match them to the table below
in disc order — the first video stream is `C1_20670`, second is
`C2_20670`, etc. Save each AVI with the base filename in the second
column (e.g. `C1_20670.avi`). The PC port will pick those up
automatically as overrides; without them it decodes the original
streams straight from the BIN.

## Stream table

| File enum index | Base filename | Description                          |
|-----------------|---------------|--------------------------------------|
| 2044            | 05_02152      | XA voice stream                      |
| 2045            | 10_04432      | XA voice stream                      |
| 2046            | 15_07496      | XA voice stream                      |
| 2047            | 20_06552      | XA voice stream                      |
| 2048            | 25_03904      | XA voice stream                      |
| 2049            | 30_04056      | XA voice stream                      |
| 2050            | 35_26008      | XA voice stream                      |
| 2051            | 40_10384      | XA voice stream                      |
| 2052            | 45_28784      | XA voice stream                      |
| 2053            | C1_20670      | Intro cinematic (US / part 1)        |
| 2054            | C2_20670      | Intro cinematic (JP / part 2)        |
| 2055            | M1_03500      | Opening movie (after main menu)      |
| 2056            | M2_01190      | In-game movie 2                      |
| 2057            | M3_02570      | In-game movie 3                      |
| 2058            | M4_02490      | In-game movie 4                      |
| 2059            | M5_03140      | In-game movie 5                      |
| 2060            | M6_02112      | In-game movie 6                      |
| 2061            | M7_01536      | In-game movie 7                      |
| 2062            | M8_03039      | In-game movie 8                      |
| 2063            | M9_01730      | In-game movie 9                      |
| 2064            | MA_03590      | In-game movie A                      |
| 2065            | MB_04850      | In-game movie B                      |
| 2066            | MC_01930      | In-game movie C                      |
| 2067            | MD_03780      | In-game movie D                      |
| 2068            | ME_03300      | In-game movie E                      |
| 2069            | Z1_16180      | Ending movie 1                       |
| 2070            | Z3_02340      | Ending movie 3                       |
| 2071            | Z4_01590      | Ending movie 4                       |
| 2072            | ZC_14392      | Ending cinematic                     |
| 2073            | ZZ_14239      | Final ending                         |

## Filename convention

The 5-digit suffix after the underscore is the number of CD sectors the
stream spans on disc (e.g. `M1_03500` = 3,500 sectors). It's part of the
on-disc directory entry and the PC port uses it to validate that the
extracted file matches the expected stream — keep the suffix in the
filename.

## Pre-extracted XA-only streams (2044–2052)

The first nine entries are audio-only XA streams for in-game voice
playback (Cybil's dialogue, hospital diaries, etc.), not video. The PC
port reads these directly from the BIN; there's no need to extract them
unless you want to replace them.

## What happens without the AVIs

The PC port has a built-in MDEC software decoder. If an AVI override
isn't present, it decodes the original PSX stream straight from the BIN
and plays the interleaved XA audio through SDL. The result is identical
content, just at the original 320×240 resolution.

## Supported AVI formats (upscale mods)

There is **no file size or resolution limit**: 64-bit offsets, OpenDML
(`RIFF AVIX`) multi-gigabyte files, and 4K+ frames all play (the decode
buffer sizes itself from the video). Any frame rate in the AVI header is
honored.

Video codecs:

- **MJPEG** (fourcc `MJPG`, `dmb1`, `jpeg`, `AVI1`, any case) — the
  recommended format: `ffmpeg -i in.mp4 -c:v mjpeg -q:v 3 -c:a pcm_s16le out.avi`
- Uncompressed RGB DIB (24/32 bpp)
- Raw YUV: `YUY2`/`YUYV`, `UYVY`, `I420`/`IYUV`, `YV12`, `NV12`

Audio: integer PCM 8/16/24/32-bit and float 32-bit
(`pcm_u8/s16le/s24le/s32le/f32le`), including WAVE_FORMAT_EXTENSIBLE.
Compressed audio (MP3/AAC/AC3) is not decoded — the video plays silent
and `SilentHill.log` prints the re-encode hint.

A file using an unsupported *video* codec (H.264, Xvid, …) is not an
error: the port logs the fourcc and falls back to the original disc
movie for that slot — **unless** the ffmpeg fallback below is built in.

## Native H.264/H.265/VP9/AV1 + mp4/mkv/webm (optional ffmpeg build)

The built-in decoder above is intra-frame only (each frame stands alone),
which is why it can't do H.264/H.265 — those are inter-frame and need a
real decoder. An **optional** ffmpeg fallback adds that, plus modern
containers, so modders can drop in the file their editor exported instead
of re-encoding to a giant MJPEG AVI:

- Put `gamedata/fmv/<basename>.mp4` (or `.mkv`, `.webm`, `.mov`, or an
  `.avi` carrying H.264/H.265) next to where the MJPEG AVIs would go.
- Video codecs: **H.264, H.265/HEVC, VP9, AV1** (and everything the
  built-in path already does — those still use the fast built-in decoder).
- Audio: AAC, MP3, Opus, Vorbis, FLAC, PCM.
- The built-in MJPEG/raw path is always tried **first**; ffmpeg is only the
  fallback, so nothing about the existing AVI workflow changes.

It is **off by default** (the shipped build stays dependency-free and
byte-identical). Enable it at configure time:

```
cmake .. -DSH_FMV_FFMPEG=ON
```

### Building the ffmpeg you ship

Two ffmpeg builds matter, and they are different:

- **For local development/linking:** the MSYS2 package is easiest —
  `pacman -S mingw-w64-x86_64-ffmpeg`. CMake finds it via pkg-config
  automatically. **Do not redistribute it** — that prebuilt is GPL-3
  (it bundles x264/x265) and would GPL-encumber the whole release.
- **For release:** build a trimmed **decode-only LGPL** ffmpeg (a few MB,
  ~5–6 DLLs). No `--enable-gpl`, no `--enable-nonfree`:

```
./configure --prefix=/mingw64/sh-ffmpeg \
  --disable-everything --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --disable-postproc \
  --disable-network --disable-encoders --disable-muxers --disable-static \
  --enable-shared --enable-swscale --enable-swresample --enable-libdav1d \
  --enable-decoder=h264,hevc,vp9,libdav1d,mjpeg,rawvideo \
  --enable-decoder=aac,aac_latm,mp3,mp3float,opus,vorbis,flac \
  --enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s24le,pcm_f32le \
  --enable-demuxer=mov,matroska,webm,avi,mp3,flac,ogg,wav,aac,m4v,h264,hevc \
  --enable-parser=h264,hevc,vp9,av1,aac,aac_latm,mpegaudio,opus,vorbis,flac,mjpeg \
  --enable-protocol=file \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe,av1_frame_split,extract_extradata,aac_adtstoasc
make -j && make install
```

Keep the bitstream filters (`--enable-bsf=…`): the mp4 demuxer needs
`h264_mp4toannexb`/`hevc_mp4toannexb` + `extract_extradata` or H.264/H.265
in mp4 silently won't decode. AV1 needs `libdav1d` (FFmpeg's native `av1`
decoder is a slow placeholder); drop both if you don't want AV1.

Then point the build at it and it bundles the DLLs into the build folder
(which the nightly zip picks up automatically):

```
cmake .. -DSH_FMV_FFMPEG=ON -DSH_FMV_FFMPEG_BINDIR=/mingw64/sh-ffmpeg/bin \
  -DCMAKE_PREFIX_PATH=/mingw64/sh-ffmpeg
```

On **Linux/macOS** the system ffmpeg is used as a normal runtime
dependency (`apt install libavcodec-dev libavformat-dev libavutil-dev
libswscale-dev libswresample-dev`, or `brew install ffmpeg`) — nothing is
bundled; note it in the platform README.

### Licensing / patent note

A decode-only build with no `--enable-gpl`/`--enable-nonfree` is LGPL, and
dynamically linking + shipping the DLLs is fine. Separately, **H.264 and
H.265 decoding is covered by patent pools** (MPEG-LA / Access Advance) that
are independent of FFmpeg's license — a redistributor shipping an H.264/HEVC
decoder may owe royalties. VP9, AV1, Opus, Vorbis and FLAC are royalty-free.
Decide deliberately before enabling this for a public release; one option is
to ship with the flag off and let users supply their own ffmpeg DLLs.
