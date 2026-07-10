# Missing Ambient SFX (Rain / Water) — Task Spec

Status: **AUDIT COMPLETE 2026-07-09 — fixes landed, awaiting in-game test.**
23-map multi-agent sweep + byte-level verification done; see "Findings" below.
Prepared 2026-07-09.
Read alongside memories `[[project_water_ambient_sfx]]` (the two mechanisms, corrected
school map IDs), `[[project_audio_audit_jul06]]` (audit tooling + exonerations),
`[[project_bgm_audit]]` (volume-parameter semantics).

User reports: no rain/water sounds in the sewers, otherworld school, and other
wet/rainy areas. Ongoing complaints from multiple users.

---

## 0. Ground rules (hard-won; do not relearn)

- **Ambient water/rain is NOT one system.** Two mechanisms confirmed so far; assume a
  map could use either (or both) until proven:
  1. **Distance-modulated BGM layers** — per-room code sets layer volumes from the
     player's distance to a water-source position (the map6_s03 "drip"). The sound is
     a BGM layer, not an SFX. Grep pattern for source positions:
     `Math_Distance2dGet(&g_SysWork.playerWork.player.position, &D_...)`.
  2. **Sd_AmbientSfx positional loop** — `SD_Call(Sfx_...)` started once, then
     per-frame `Sd_SfxAttributesUpdate(sfx, balance, vol, 0)` (the map1_s06
     school-exterior rain). `Sd_AmbientSfxInit` is real on PC, not stubbed.
- **The recurring root is zero-stubbed data**, not missing code: source positions /
  room-flag tables / layer-limit tables that came from `INCLUDE_RODATA` and were
  zero-stubbed on PC. Fix = extract the REAL values (from source comments or the PSX
  binary/overlays) and hand-append them to the map's
  `pc_port/build_gen/extracted_data/<map>_extracted_data.c`. **Never bulk-regen
  extract_map_data.py** (drops hand-added symbols — `[[feedback_no_bulk_extract_regen]]`);
  never ship guessed values.
- **Volume semantics trap:** `Sd_PlaySfx`-family vol params are ATTENUATION in some
  call paths (0 = full, 255 = silent) — verify direction per site before concluding
  "volume is zero, so it's muted" (`[[project_bgm_audit]]`).
- Tempo/FPS/pitch are EXONERATED (don't chase): the sequencer clock is a dedicated
  577.8Hz thread, verified 2026-07-06.
- Audit tooling: `pc_port/tools/audit_zero_stubs.py` (ranks zero-stubs),
  `pc_port/tools/audit_xa_items.py`. `[SH_BGM]`/`[SD]` log lines + SilentHill.log.

## 1. School map IDs (CORRECTED — the objdiff config-retail.yaml list is authoritative)

- map1_s00 = 1F + courtyard + basement (normal world)
- map1_s01 = 2F (normal)
- **map1_s02 = 1F + courtyard OTHERWORLD** ← "otherworld school" rain reports
- map1_s03 = 2F + roof OTHERWORLD
- map1_s04 = unused; map1_s05 = boss; map1_s06 = 1F + basement after boss
  (has the WORKING Sd_AmbientSfx rain — use as the reference implementation)
- map6_s03 = sewer connecting to Lakeside Amusement Park (drip FIXED, 414172e09)
- **map5_s00 = the MAIN sewers** ← "sewers" reports mean this one, NOT the fixed
  map6_s03.
- ~~map2_s01 = Old Silent Hill sewers~~ **CORRECTION (2026-07-09 audit): map2_s01
  is the Balkan CHURCH interior** (MAP_INFOS[9], MapType_ER — Flauros/Dahlia
  events, one exit to MAP2_S00). It has no water mechanism at all; church silence
  before the Dahlia cutscene is original PSX behavior. There is no separate "Old
  Silent Hill sewers" map — sewer complaints resolve to map5_s00.

## Findings (2026-07-09 sweep — 23 maps audited, every claim byte-verified)

1. **Otherworld-school rain ROOT CAUSE (map1_s02 courtyard + map1_s03 roof,
   also map0_s00/map4_s02 — the Particle_SoundUpdate MAP0_S01 case is @unused,
   that map compiles no rain code): severed PSX symbol alias.** On PSX
   `sharedData_800DD58C_0_s00` IS `g_ParticlesAddedCount[1]` (one object, two
   names — every rain overlay's sym table shows the 4-byte overlap inside the
   s32[2]). On PC they were split into separate objects, so the weather
   machine's rain-particle count never reached `Particle_SoundUpdate`'s ramp
   target and `SD_Call(Sfx_Unk1360)` was structurally unreachable: rain visuals
   with no rain sound. FIXED with an SH_PC_PORT alias in src/maps/particle.c.
   This is a new bug CLASS (object identity, not zeroed content) invisible to
   zero-stub audits — see also finding 3.
2. **map5_s00 (main sewers): all water-ambience data is REAL and byte-exact**
   (layer caps D_800DA570, room flags D_800DA578, source D_800CB0CC, engine
   tables — verified against MAP5_S00.BIN). The water bed is BGM-28 layers
   gated per room by func_800D6790. If testers still hear no water on a
   post-2026-06-10 build, the fault is downstream (KDT track-28 layer playback)
   — next step is runtime `[SH_BGM]` probes during a sewer walk, per §2.3.
3. **map6_s04/map6_s05 water-fade alias (same class as finding 1):**
   `sharedData_800EB74A_6_s04` is byte [2] of the limits table
   `sharedData_800EB748_6_s04` on PSX; split on PC, so the room-3 water layer
   played at CONSTANT FULL volume (no distance fade, hard cut at the room
   edge). FIXED in Map_RoomBgmInit_6_s04_CondFalse.h (SH_PC_PORT write-through).
4. **Hospital elevator arrival chime (map3_s01/s03/s04/s05):** the exe
   cross-map stub for `sharedData_800CB094_3_s01` hardcodes map7_s01's position
   ~169 units away -> attenuated to exactly 0. FIXED: per-overlay VECTOR3
   extracted into each map's extracted_data (+ map3_s01's elevator-stop clunk
   `sharedData_800CB0A0_3_s01`, which was a plain zero-stub).
5. **map1_s06 rain + map6_s03 drip references verified intact**; shared Sd_*
   layer verified healthy end-to-end (SD_Call -> Sd_PlaySfx -> Konami libsd ->
   PsyCross SPUAL, no stubs). Volume semantics confirmed: Sd_PlaySfx /
   Sd_SfxAttributesUpdate vol = ATTENUATION (0=full, 255=silent; sd_call.c:425,
   565); BGM layer path = plain LEVEL with limits cap 0x80=unity (bgm.c:256).
6. Maps with NO water/rain mechanism in original PSX code (silence there is
   authentic): map2_s01, map2_s02, map3_s00/s02/s06, map4_s03 (pre-event),
   map5_s02, map6_s01, map7_s00/s01/s02. map5_s01/map6_s00/map6_s02 water
   ambience verified data-complete and working.
7. Side fixes landed with this sweep: six map5_s00 pickup poses
   (D_800DAAF8..D_800DAB5C, items rendered at world origin), map5_s03
   `g_ParticlesAddedCount` scalar-vs-s32[2] OOB write.
8. Still open (small, documented): map3_s05 vine-fire loop plays at constant
   volume (D_800DD190 is `sharedData_800D8568_1_s05+0x10` on PSX — another
   severed alias, engine-written); worth folding into any future alias sweep of
   Bgm_Update users (map7_s00..s03 limit tables are candidates to check).

## 2. Work plan

1. **Reproduce + scope.** For each complaint area (map5_s00, map2_s01, map1_s02,
   map1_s03 roof, plus any rainy street maps), check the map code for either
   mechanism: grep the map's `.c` files for `Math_Distance2dGet(... &D_...)`
   (mechanism 1) and `SD_Call`/`Sd_SfxAttributesUpdate`/`Sd_AmbientSfx` (mechanism 2).
   A map with NEITHER may drive rain via BGM layer-limit tables alone
   (`[[project_bgm_audit]]` zero-stub class) — check its extracted_data for
   zero-stubbed `D_*` audio tables.
2. **For each hit, identify the data dependencies** (source-position vectors,
   room-flag tables, layer tables, sfx IDs) and verify against
   `pc_port/build_gen/extracted_data/<map>_extracted_data.c`: zero-stubbed → extract
   real bytes (PSX overlay for that map; the EUR decrypter
   `pc_port/tools/decrypt_eur_overlay.py` pattern works for US overlays too if
   needed) → surgical append.
3. **Runtime verify with logs**: add temporary `[AMBSFX]` SH_DBG probes (volume,
   balance, room gate, distance) if data looks correct but silence persists — the
   map6_s03 lesson is that correct-looking code + zeroed data is indistinguishable
   from broken code without probes.
4. **Cross-check the working reference** (map1_s06 rain) on the same build first: if
   THAT is silent too, the bug is in the shared Sd_* layer (attributes update,
   ambient init, SPU voice limits), not per-map data.
5. Wet-map inventory worth sweeping while in here: map3_* (alleys rain?), map4_s03
   (sewer worm areas), map6_s02 (lighthouse water), resort/lakeside maps.

## 3. Testing

- User runs the game; verify via SilentHill.log + their report. For each map: enter
  the complaint room, confirm the ambient starts, walk toward/away from the water
  source (mechanism 1 should swell/fade), room-transition to confirm gating.
- PAL note: audio files remap by sector at region init (g_AudioData remap) — test on
  US first so region remapping isn't a variable.

## 4. Parallel-session boundaries

Texture/renderer work owns: hires_override.*, tex_pack.*, fsqueue_3.c PostLoadTim,
terrain.h pool, PsyX_GPU/PsyX_render. PAL session owns: font_region, lang_text,
fileinfo region init, launcher. This task should not need any of those files —
it lives in src/maps/*, extracted_data, and src/bodyprog/sound/*.
