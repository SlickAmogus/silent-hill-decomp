# BGM System: Technical Analysis

A deep-dive into Silent Hill's background music system for the decomp deobfuscation work. Notes on architecture, the still-unnamed pieces, and suggested renames.

> **Note:** Everything described here is original PSX game code. The PC port adds nothing to the BGM logic itself — it just routes the libsd MIDI vol calls through OpenAL via the PsyCross SPU stub. The renames below are decomp-team work, not PC-port-specific.

## Architecture: it's a sequencer, not a player

SH1 doesn't just play .wav-style tracks. Each "BGM track" is a **Konami libsd SEQ file** — a lightweight MIDI-style format running on the PSX SPU. A SEQ has up to 16 MIDI channels, each driving one or more SPU voices via a VAB (instrument bank). The "8-layer" abstraction sits *above* the MIDI channel layer:

```
       Game (Bgm_Update flags)               <-- flag bits 0..7 = "want layer N"
              │
              ▼
    g_SysWork.bgmLayerVolumes[8]              <-- Q3.12, target per-layer
              │  (per-frame fade math)
              ▼
        layerLimits[8]   <-- u8[8] caps per layer per ROOM
              │
              ▼
     bgmLayerVolumes[8]  (static, file-scope) <-- final s8 push values
              │
              ▼
    Sd_BgmLayerVolumeSet(layer, vol)
              │
              ▼
    D_800AA604[seq_id][midi_ch] == layer ?    <-- the key indirection table
              │  yes
              ▼
       SdSetMidiVol(seq_id=0, midi_ch, vol)   <-- libsd MIDI vol set
              │
              ▼
              SPU
```

**`D_800AA604[41][16]`** is the heart of the system. Indexed `[seq_id][midi_ch]`, it returns which abstract layer that channel belongs to *for this particular track*. So track 1 might map ch1→layer1 (drums), ch2→layer2 (bass), etc., while track 2 has a totally different MIDI lineup but the game still says "fade layer 1." The matrix translates per-track.

This is why an 8-layer abstraction over 16 channels works without explosion: not all channels are independent, several can co-fade (look at row 12: `{0, 3, 3, 1, 2, 6, 4, 5, 1, ...}` — two channels both on layer 3, two on layer 1).

## Active SEQ: `func_80045BC8` → `g_Sd_AudioWork.field_E`

This is the `seq_id` index into `D_800AA604`. Suggested name: `Sd_BgmCurrentSeqGet` / `g_Sd_AudioWork.bgmSeqIdx_E`. Returns:
- `0` = no track loaded (early-out path)
- `0xFFFF` = error / track not yet ready
- `1..40` = active SEQ index

`Bgm_LayerOnCheck` and `Bgm_GlobalLayerVariablesUpdate` both gate on this — if no track, do nothing.

## The state machine: `D_800A99A0`

A 4-state lifecycle for the SEQ player. Suggested name: `g_Bgm_PlayerState` (or `e_BgmPlayerState`):

| Value | Meaning | What `Bgm_Update` does |
|---|---|---|
| `0` | playing | Push computed `bgmLayerVolumes[]` to MIDI via `Sd_BgmLayerVolumeSet` |
| `1` | post-mute resume | Re-sync `g_SysWork.bgmLayerVolumes[]` from the SPU's actual MIDI vols (`Sd_BgmLayerVolumeGet`), then fall to 0 |
| `2` | mute step 2 | One-frame transitional: zero all layers, advance to 1 |
| `3` | mute requested | If a track is loaded, mute all + call `SD_Call(18)` (the "stop BGM" task), `Bgm_ChannelSet` to re-arm MIDI channel routing, advance to 2 |

The transitions are all driven by `isBgmLayerActive` (any layer > 0?) and `cond0` (= `field_E != 0 && field_E != 0xFFFF`, i.e. "valid SEQ loaded"). When a state change happens that *invalidates* the loaded SEQ (e.g. switching tracks), state goes from 0 → 3 → 2 → 1 → 0 over four frames, with mid-flight muting. That's the gap you can hear when BGM changes.

## The flag massage block

Lines 189-196 of `Bgm_Update`:

```c
if (flagsCpy & BgmFlag_Layer0) {
    flagsCpy &= BgmFlag_KeepAlive | BgmFlag_MuteAll;   // strip Layer0..7
} else {
    flagsCpy ^= BgmFlag_Layer0;                         // force Layer0 on
}
```

Suggested comment: *"`BgmFlag_Layer0` semantically means "fade out all layers" because layer 0 is always dominant; passing it tells the mixer to keep only `KeepAlive`/`MuteAll` modifiers and zero every layer rail. Passing any other bit (or zero) implicitly turns layer 0 *back on* via the XOR — layer 0 is the always-audible base, the rest are additive overlays."*

So a call like `Bgm_Update(BgmFlag_Layer1, ...)` ends up with `flagsCpy = Layer0|Layer1` → both rails to 1.0. `Bgm_Update(BgmFlag_Layer0|BgmFlag_MuteAll, ...)` → `flagsCpy = MuteAll` → all rails to 0.0 + master mute.

The actual loop interpretation:
```c
for (i = 0..6)
    target_i = ((flagsCpy >> i) & 1) ? Q12(1.0) : Q12(0.0);
```

Slot `i=7` (the last) is special: it's the **master ducking rail**, not a layer. Its target comes from the status flags:
- `BgmStatusFlag_ApplyMute` → 1.0 (full duck = silent)
- `BgmStatusFlag_RadioActive` → 0.75 (radio on; duck music for hum)
- `BgmStatusFlag_Duck` → 0.5 (cutscene/dialog duck)
- else → 0.0 (no duck)

Then in the output stage:
```c
temp_v0 = Q12(1.0f) - layerVols[8];   // master "anti-duck" = 1 - duck
for (i in 0..6) {
    out = layerVols[i];
    if (i == 0) out *= temp_v0;        // only layer 0 gets ducked!
    out *= Q12(0.0312);                // (≈ 1/32) scale into 0..127
    out = (out * layerLimitsCpy[i]) >> 7;   // per-room cap
}
```

So **the duck only attenuates layer 0**. Layers 1-6 ride on top. That's how dialog/radio drops the *base* music while leaving stinger layers (action stems, weather, etc.) at full volume.

## The fade rate machinery

Per-layer:
```c
if (target_reached) skip;
else if (delta > 0)  curVol += min(fade_step, delta);
else                 curVol -= min(fade_step, |delta|);
```

`var_t0` (the step) is computed per-layer per-frame:
- For layers 0..6 with target=1.0: `g_DeltaTimeRaw * fadeSpeed * 2` (the `Q12_SHIFT - 1` instead of `Q12_SHIFT` is a `* 2` — comment in source says "@hack Should be multiplied by 2 but doesn't match"). Faster ramp **up**.
- For layers 0..6 with target=0.0: `g_DeltaTimeRaw * fadeSpeed`. Normal ramp **down**.
- For the duck rail (`i == 7`): `g_DeltaTimeRaw * 0.25` — quarter-rate, so duck transitions are smooth.

`fadeSpeed` (the second param to `Bgm_Update`) is a Q12 value — typical map values are `Q12(240.0f)` for slow/normal, `Q12(0.333f)` for fast snap-mute, `Q12(0.125f)` for very fast.

## Per-room limits: `s_BgmLayerLimits` (the third param)

`u8[8]` from per-room data, applied as a max-volume cap **after** the layer math:

```c
out = (out * layerLimitsCpy[i]) >> 7;   // 0..127 scale
```

`128 = 100%`, `64 = 50%`, `0 = silent`. `Map_RoomBgmInit_*` functions choose this table per room. NULL → use `g_Bgm_LayerLimits` (the global default of all 128s). So a room can permanently cap, say, the action layer at 50% even when the flag bit says "full on."

This is the parameter that bit the PC port early on: a room-specific limit table (e.g. `&D_800DF2F8`) that was zero-stubbed produced silent layer output regardless of flags. Reverting to `NULL` made the global default kick in.

## Suggestions for the next deobfuscation pass

| Current | Suggested |
|---|---|
| `D_800AA604[41][16]` | `g_Bgm_SeqMidiToLayerMap` |
| `D_800A99A0` | `g_Bgm_PlayerState` (with `e_BgmPlayerState` enum: `Playing/MuteResume/MuteStep/MuteRequest`) |
| `D_800BCD5C` | `g_Bgm_TrackUpdateRan` (flag set at end of `Bgm_Update`, cleared at start of `Bgm_TrackUpdate`, used to skip default `Bgm_Update` if a map's `bgmEvent_10` already called it) |
| `func_80045BC8` | `Sd_BgmCurrentSeqGet` |
| `g_Sd_AudioWork.field_E` | `bgmSeqIdx_E` |
| `BgmStatusFlag_RequestMute` / `_ApplyMute` | (already named, good) |
| `D_800A99A0 == 3` SD_Call(18) | the `18` is the "BGM stop" SFX cmd ID — could promote to `SfxCmd_BgmStop` |
| `bgmLayerVolumes[8]` (static in `Bgm_Update`) | rename to `s_BgmOutputLayerVols` to disambiguate from `g_SysWork.bgmLayerVolumes` |
| index 7 of `bgmLayerVolumes` | `bgmDuckRail` or document as such — it's not a layer, it's the master duck envelope |

The fact that there's a `static s8 bgmLayerVolumes[8]` (file-scope-ish) shadowing `g_SysWork.bgmLayerVolumes` (Q3.12) is the main confusing thing. They're not the same thing — one is the per-frame target rail (Q12, internal smoothing), the other is the s8 output written to libsd MIDI volume.

## Summary diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│  Per-frame                                                           │
│  Bgm_Update(flags, fadeSpeed, layerLimits)                          │
│                                                                      │
│   flags ──► massage (layer-0 sticky logic) ──► per-layer target      │
│                                                          │           │
│                                                          ▼           │
│   g_SysWork.bgmLayerVolumes[0..7]   <─ smooth toward target          │
│                                          (rate = fadeSpeed)          │
│                                          │                            │
│             ┌────────────────────────────┘                            │
│             ▼                                                         │
│   apply duck (only on layer 0)                                       │
│   apply scale (Q12 → s8)                                             │
│   apply layerLimits[i] cap                                           │
│             │                                                         │
│             ▼                                                         │
│   bgmLayerVolumes[0..6]  (static s8)                                 │
│             │                                                         │
│             ▼                                                         │
│   state machine (D_800A99A0):                                        │
│     0 → push to SPU via Sd_BgmLayerVolumeSet                         │
│     1 → resync from SPU                                              │
│     2 → silence + advance                                            │
│     3 → request stop track                                           │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
        Sd_BgmLayerVolumeSet(layer, s8 vol)
                                │
            ┌───────────────────┴───────────────────┐
            │ for each MIDI ch i in 0..15:           │
            │   if D_800AA604[seq][i] == layer:      │
            │     SdSetMidiVol(seq=0, ch=i, vol)     │
            └────────────────────────────────────────┘
                                │
                                ▼
                       Konami libsd / SPU
```

That's the whole thing. The deobfuscation work is mostly naming `D_800AA604`, the state machine var, and recognizing that index 7 of the layer rail is a duck envelope — once those are renamed the function is genuinely readable.

## Track loading vs. layer switching

Two different paths:

- **`Bgm_TrackChange(idx)`** — sets `g_MapOverlayHeader.bgmIdx_14 = idx`. Picked up by `Bgm_TrackUpdate` next frame; that calls the map overlay's `bgmEvent_10` callback, which examines the new index and issues `SD_Call(g_BgmTaskLoadCmds[bgmIdx])` to actually load the SEQ + VAB into SPU memory. **This is heavy** — physical memory transfer. Causes the audible 1-2 frame gap on track change.

- **`Bgm_Update(flags, fadeSpeed, NULL)`** — reroutes which layers within the *currently loaded* track are audible. **This is cheap** — just MIDI vol changes. No memory transfer, no audible gap (just the fade).

Designers seem to have used Track Change for narrative beats (room → room with different music) and Layer flags for moment-to-moment dynamics within a track (combat tension stem, ambient mood stem, etc.).

## Why layer 0 is special

Several pieces of code treat layer 0 as the "base" track and layers 1-6 as overlays:
- `Bgm_GlobalLayerVariablesUpdate` only writes to layer 0 explicitly (`= Q12(1.0f)`); 1-6 are read from current SPU state.
- The duck envelope only attenuates layer 0 (lines 263-265).
- `flagsCpy ^= BgmFlag_Layer0` forces it on if not explicitly suppressed.

Conceptually: every track has a layer-0 "always playing" stem (typically the main melody/loop), and 1-6 are the conditional stems that the game enables/disables based on context.

## Open questions / remaining unknowns

- `D_800A999C` and `D_80025234[]` — declared in `bgm_update.c` but never referenced inside the BGM logic. Per-region byte values look like leftover/garbage data from a deleted feature. Probably not BGM-related; could move out when split is fully resolved.
- `func_8003652C` — at the bottom of the file, calls `LoadImage` on a hardcoded VRAM rect with hardcoded values. Looks like a per-frame VRAM patch (debug-text overwrite?). Unrelated to BGM but lives in the same TU.
- The exact interpretation of `g_Sd_AudioWork.field_E` values 1-40 vs the 41 rows of `D_800AA604`. Off-by-one suggests row 0 is a "no track" sentinel (all zeros), rows 1-40 are real tracks, matching the 35 entries in `e_BgmTrackIdx` plus a handful of internal/cutscene-only tracks.
- `Bgm_TrackUpdate(arg0)` — the `arg0` bool. `false` = normal frame update, `true` = forced re-arm (called from `func_8003596C` during map load to kick the BGM if it stalled). Worth naming.
