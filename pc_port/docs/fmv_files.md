# Silent Hill FMV File Table

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
