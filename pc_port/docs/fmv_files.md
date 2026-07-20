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
movie for that slot — unless ffmpeg is available (see below).

## Native H.264/H.265/VP9/AV1 + mp4/mkv/webm (bring your own ffmpeg)

The built-in decoder above is intra-frame only (each frame stands alone),
which is why it can't do H.264/H.265 — those are inter-frame and need a
real decoder. Every build of the port has ffmpeg support **compiled in**,
but ships **no ffmpeg DLLs**. It looks for them at runtime; if they aren't
there, nothing breaks — FMVs just play from the disc as always.

So there are two tiers:

**Tier 1 — works out of the box, no extra files.** Everything in
*Supported AVI formats* above: MJPEG, uncompressed RGB, raw
YUY2/UYVY/I420/YV12/NV12, with PCM or float32 audio. This is still the
recommended path for mod authors who want zero friction on the player's
end:

```
ffmpeg -i in.mp4 -c:v mjpeg -q:v 3 -c:a pcm_s16le out.avi
```

**Tier 2 — needs the player to supply ffmpeg DLLs.**

- Video: **H.264, H.265/HEVC, VP9, AV1**
- Audio: **AAC, MP3, Opus, Vorbis, FLAC**
- Containers: `.mp4`, `.mkv`, `.webm`, `.mov`, and `.avi` files carrying
  any of the above

Put the file at `gamedata/fmv/<basename>.<ext>` — same place the MJPEG AVIs
go. The built-in path is always tried **first**, so nothing about the
existing AVI workflow changes; ffmpeg is only the fallback.

### Getting the DLLs (ffmpeg 6.x only)

Download a **shared** ffmpeg 6.x build and drop these five DLLs next to
`SilentHillPC.exe`, or put an ffmpeg 6.x `bin` folder on your `PATH`:

| Library | DLL |
|---------|-----|
| avcodec | `avcodec-60.dll` |
| avformat | `avformat-60.dll` |
| avutil | `avutil-58.dll` |
| swscale | `swscale-7.dll` |
| swresample | `swresample-4.dll` |

Both spellings are accepted — `avcodec-60.dll` and `libavcodec-60.dll`
both load, whichever your build produced.

The version numbers above come from the ffmpeg headers the port was compiled
against, so the authoritative list for *your* build is the one written to
`SilentHill.log` when the libraries are missing — check there if it disagrees
with this table.

> **It must be ffmpeg 6.x. A 7.x build will not work.** The port is
> compiled against the ffmpeg 6 ABI and reads decoder struct fields
> directly, so a different major version is a crash, not a quirk. The port
> checks each library's major version on load and **refuses** anything that
> doesn't match, logging which library was wrong and what it expected. If
> your movie mod isn't playing, check the DLL version numbers first — the
> filenames must end in `-60`, `-58`, `-7`, `-4` as above.

Where to get them:

- **Windows:** [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) or
  [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases). Pick a **shared**
  6.x package — the "static" ones contain no DLLs at all. On gyan.dev the
  file you want is a `ffmpeg-6.*-full_build-shared.7z`; on BtbN, an asset
  with `shared` in the name from a 6.x release.
- **Linux:** the distro packages, e.g.
  `apt install libavcodec60 libavformat60 libavutil58 libswscale7 libswresample4`
  (Debian 13 / Ubuntu 24.04 era). The loader looks for
  `libavcodec.so.60` etc.
- **macOS:** `brew install ffmpeg@6`. The loader looks for
  `libavcodec.60.dylib` etc.

### If the DLLs are missing

Nothing fails. The port logs a line to `SilentHill.log` saying ffmpeg
wasn't available (or which library mismatched), that movie falls back to
the original disc version, and the game continues normally. The launcher
also flags it so you aren't left guessing why a movie mod did nothing.

### Why we don't bundle ffmpeg

Redistributing ffmpeg carries license obligations, and the common prebuilt
Windows binaries are GPL builds. Separately, H.264 and H.265 decoding is
covered by patent pools (MPEG-LA / Access Advance) independent of ffmpeg's
license. Shipping no decoder at all sidesteps both, while users stay free
to install ffmpeg themselves. VP9, AV1, Opus, Vorbis and FLAC are
royalty-free if you'd rather avoid the question entirely when authoring a
mod.

### Build note

ffmpeg **headers** are a build-time dependency (`pacman -S
mingw-w64-x86_64-ffmpeg` on MSYS2, or the `-dev` packages on Linux); the
libraries are never linked. If the headers aren't found, CMake prints a
warning and auto-disables the feature rather than failing the build.
`-DSH_FMV_FFMPEG=OFF` still compiles the path out entirely.
