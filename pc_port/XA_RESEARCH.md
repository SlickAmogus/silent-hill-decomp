# XA Audio Implementation Research

> **Status — Historical/superseded research.** The current port plays 4-bit XA voice streams and STR/MDEC movie audio directly from the disc image; pre-extracted XA files are not required. See the [feature catalog](../features.md), [FMV reference](docs/fmv_files.md), and [documentation index](docs/README.md). The research below preserves the original implementation rationale.

## Overview
XA (eXtended Audio) on PSX: CD audio format for voice/dialogue.
- Compressed with ADPCM (4-bit or 8-bit)
- 18.9kHz or 37.8kHz sample rates
- Mono or Stereo
- Delivered as sectors on CD (2352 bytes/sector)

## Historical PC-port status when this research was written
- **PC port at that time**: skipped all XA playback (`sd_call.c` lines 140–142)
- **Game expected**: voice lines and dialogue during cutscenes and gameplay
- **Raw files used by that investigation**: 30 files, about 150 MB total

## PSX Game-Side Architecture
Game calls:
```c
Sd_XaAudioPlayTaskAdd(u16 cmd)  // Queue XA play command
```

Pipeline:
1. `SD_Call()` (sd_call.c line 94) routes by command category
2. Cases 16-22 → `Sd_XaAudioPlayTaskAdd()` (line 861)
3. Queues task in `g_Sd_TaskPool[]` with ID=1 (XA play) or 2 (XA stop)
4. Main loop calls `Sd_TaskPoolExecute()` (line 1723)
5. Executes `Sd_XaAudioPlay()` state machine (line 884)
   - Initialize → SetMode → PrepareFilter → SetFilter → CalculateLba → Seek → StartRead → EnableAudio
6. Uses CD primitives:
   - `CdlSetmode`: Set CD mode (XA audio + speed)
   - `CdlSetfilter`: Filter for channel/file (s_XaItemData[idx].field_4_24, field_8_24)
   - `CdlSeekL`: Seek to LBA
   - `CdlReadN`: Start reading XA sectors
7. SPU serial interface receives XA stream:
   - `SdSetSerialAttr(0, 0, 1)` - Enable audio input
   - `SdSetSerialVol()` - Set volume

## XA Data Format (from DuckStation)
### CD Sector (2352 bytes)
- Sync (12 bytes)
- Header (4 bytes): minute/second/sector/mode
- [For XA sectors] XASubHeader × 2 (8 bytes total)
- XA payload: 18 chunks × 128 bytes

### XASubHeader (4 bytes per track)
```
Byte 0: file number
Byte 1: channel number
Byte 2-3: submode/coding info
  - Bit 0: End-of-Record
  - Bit 1: Video
  - Bit 2: Audio (1=audio sector)
  - Bit 3: Data
  - Bit 4: Trigger
  - Bit 5-6: Form (0=form1, 1=form2)
  - Bit 7: Real-time
Byte 3: Codinginfo
  - Bits 0-3: Emphasis/reserved
  - Bits 4-5: Sample rate (0=37.8kHz, 1=18.9kHz, 2-3=reserved)
  - Bit 6: Bits/sample (0=4-bit, 1=8-bit ADPCM)
  - Bit 7: Channels (0=mono, 1=stereo)
```

### ADPCM Block (1 byte header + 28 nibbles/bytes)
```
Header byte:
  - Bits 0-3: Filter index (0-4 valid, others =9)
  - Bits 4-7: Shift value (left shift amount for 4-bit sample expansion)

Data: 28 words (4-bit) or 28 bytes (8-bit)
```

### Decode Algorithm (from DuckStation cdrom.cpp)
```c
// Filter coefficients (only indices 0-3 used)
s8 filter_pos[] = {0, 60, 115, 98, 0, ...};
s8 filter_neg[] = {0, 0, -52, -55, 0, ...};

// For each sample:
s32 sample = (nibble << 12) >> shift;  // Expand 4-bit to 16-bit with shift
sample += (prev[0] * filter_pos[filter]) >> 6;  // Mix previous filtered samples
sample += (prev[1] * filter_neg[filter]) >> 6;
prev[1] = prev[0];
prev[0] = clamp16(sample);
```

## Data Layout - Decomp Metadata
### s_XaItemData (12 bytes per entry)
```c
struct s_XaItemData {
  u8  xaFileIdx_0;      // Index into g_FileXaLoc[] (raw file)
  u8  pad_1[3];
  u32 sector_4 : 24;    // Starting sector within file
  u8  field_4_24 : 8;   // Channel index (for CdlSetfilter)
  u32 audioLength_8 : 24;// Length in samples (or sectors?)
  u8  field_8_24 : 8;   // File index (for CdlSetfilter)
};
```

### g_FileXaLoc[]
Array of file sector offsets (in CD sectors from disc start).
Each entry = absolute CD sector where that XA file begins.

### XA to PSX mapping
- Cutscenes (C1, C2 = 20670 sectors each): Full uncompressed dialogue
- Maps (05-45 = ~3000-28000 sectors): Ambient voices, NPC dialogue
- Music (M1-M9, MA-ME = 1000-4850 sectors): BGM (not used same way as XA)
- Zones (Z1,Z3,Z4,ZC,ZZ = 1000-16000 sectors): Zone-specific ambient

## PC Port Implementation Plan

### Phase 1: Raw File -> Audio Buffer
1. Read raw XA files into memory (30 files, pre-extracted)
2. Build in-memory "disc image" mapping:
   - g_FileXaLoc[] → file handle + sector offset within that file
   - s_XaItemData[] metadata (already in decomp)

### Phase 2: ADPCM Decoder
1. Implement XA sector parser
2. Extract XASubHeader (sample rate, channels, bits)
3. Implement ADPCM decode (filter tables + sample interpolation)
4. Handle 4-bit and 8-bit

### Phase 3: Hook into PsyCross
1. Decode XA → PCM samples (float32 or int16)
2. Feed to PsyCross OpenAL audio system
3. Manage playback state (volume, stop, fade)

### Phase 4: Game Integration
Replace PC port's current skip (sd_call.c line 140-142) with:
1. Parse XA file/sector from s_XaItemData
2. Start async decode thread or on-demand decode
3. Queue PCM to OpenAL via PsyCross
4. Respect volume/stop commands

## Key Challenges
1. **Raw file format**: Confirm if 2352-byte sectors or stripped data
2. **Sector boundaries**: XA data may span sectors; need block-aware parsing
3. **Audio format**: 44.1kHz output (PsyCross default) requires resampling from 18.9kHz
4. **Sync timing**: XA must sync with cutscene animations (DMS timing)
5. **Memory**: 150MB raw files need streaming or careful buffering

## Reference Sources
- DuckStation cdrom.cpp (3548-3610): ADPCM decode algorithm
- DuckStation spu.cpp (1899-1930): Voice ADPCM decode
- Silent Hill decomp: sd_call.c, sound_system.h, stream.c
- Game data: g_XaItemData[], g_FileXaLoc[], raw XA files
