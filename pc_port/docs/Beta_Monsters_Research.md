# Beta / Unused Monsters — Research & Restoration Feasibility (2026-07-13)

Research pass over the decomp, the retail disc file table, extracted disc data
(`C:/Claude/silenthill/disc_extract/`), TCRF, Silent Hill wiki, silenthillmemories.net
and community restoration work. **No beta monster is implemented yet** — this doc is
the input for deciding what to restore and how. The global chara pool
(`docs/Global_Chara_Pool.md`) is the enabling substrate: anything with a charaId and
disc files can already be made *visible* anywhere; behavior/AI is the open part.

## Tier 0 — misconceptions cleared up first

- **Unknown23 (charaId 23, "MAR") is NOT an unused monster.** It is the fully
  implemented AI for the Incubator→Incubus transformation phase of the final boss
  (994-line `unknown23.c`); retail runs it by hot-swapping
  `charaUpdateFuncs[Chara_Incubator] = Unknown23_Update` (map7_s03_3.c:3886). The
  charaId itself is never instantiated. Nothing to restore.
- **DummyNurse(17)/DummyDoctor(19)** are retail-shipped placeholder charas: real
  256-byte `DUMMY.ILM` (one `01GRID` object), 512-byte `DUMMY.ANM`, **no texture**
  (`textureFileIdx = NO_VALUE`), dispatching the Puppet* AI. They occupy real
  hospital charaGroupIds with empty spawn lists — reserved "second variant" slots
  for cut hospital enemies. Interesting trivia, nothing meaningful to restore.
- **LittleIncubus(41), GhostDoctor(42), Parasite(43), LockerDeadBody(25)** are all
  used by retail (bad-ending birth, Nowhere ghost doctors, Cybil-cure cutscene,
  school locker scare). Not beta content.
- **"Weeping Bat" is a Silent Hill: Downpour enemy** — any SH1 claim is false.

## Tier 1 — the Chicken (charaId 13, files CKN): one step from spawnable

The ONLY unused monster with a full chara-system registration:
`Chara_Chicken = 13 /* @unused */` + a complete `CHARA_FILE_INFOS` row
(CKN.ANM 43.5 KB — a full anim set, larger than Romper's; CKN.ILM 11 KB;
CKN.TIM 8.4 KB; present in all four region file tables). TCRF: a giant contorted
chicken with fledgling-like textures; community lore says scrapped boss.
Masahiro Ito: "I certainly made some of these. However I have no comment."

- **Zero AI exists anywhere** — no update func, no anim-info table, no spawn.
- With the global pool, `SPAWN CHICKEN` now loads model+anim and it renders as a
  **posed statue** (`[no-ai]`) in any map.
- **Full restoration needs**: a `CHICKEN_ANIM_INFOS` table (reverse-engineer
  CKN.ANM's keyframe ranges — the K/,/. keyframe inspector can scrub them), an
  update func (new `chicken.c` following the groaner four-pillar pattern, or a
  donor AI — Groaner_Update is the closest quadruped, but anim status ranges
  won't line up without the anim-info table), collision keyframes (none extracted;
  would need hand-tuning), health/damage numbers (invented — no source data), and
  a combat blood-type case (falls to default 16 otherwise). Community reports the
  model has a glitchy right leg (unfinished mesh) — cosmetic.
- Effort: a focused session (ANM reverse-engineering is the long pole). Result
  would be a *plausible reconstruction*, not a restoration — no behavior data
  exists to be faithful to.

## Tier 2 — the animal menagerie (complete files, NO charaId)

Six-plus complete model+texture+anim sets on every retail disc, referenced by zero
code and absent from `e_CharaId`. All FILE_ enums already exist in the port:

| Files | Creature (community name) | Notes |
|---|---|---|
| BTFY (ILM/TIM + 20.7KB ANM) | Butterfly | eye/teeth markings; resembles purple Floatstinger |
| MKY (+21.8KB ANM) | Monkey | bruised face, bloody mouth; lunge anims visible in the ANM |
| OST (+16.6KB ANM) | "Ostrich" | huge flesh-colored faceless torso |
| SNK (+8.7KB ANM) | Snake | long pointed tongue; a "CAUTION snakes" road sign survives in retail |
| FRG (+12KB ANM) | Frog | featureless |
| EI (ILM + 20KB ANM; texture in **TEST/EI.TIM**) | Manta ray | "ei" = Japanese for ray |
| MAN (ILM + 57KB ANM, **no TIM**) | human figure | large anim set, textureless |
| DOB (+3KB ANM), TDRA, BIG (128×512 TIM!), SPD (ANM only), BOS2 (32-row CLUT, no ANM) | misc | partial sets |

TCRF: "no individual behaviors appear to have been programmed for these enemies…
they all have unique animations, meshes and textures." All community restorations
to date are **file-swaps over existing enemies** (roocker666, covered by Kotaku);
no ID-based spawn exists anywhere — our SPAWN console + pool approach is already
ahead of community tooling.

**Restoration path (PC-only, needs design sign-off):** extend the chara table past
`Chara_Count` under `SH_PC_PORT` — e.g. `PcChara_Butterfly = 45..` — with
PC-side CHARA_FILE_INFOS extension rows. Ripple points that make this a *planned
feature, not a patch*: `charaUpdateFuncs[Chara_Count]` in every map header (PC-only
size bump = full DLL rebuild, same class as the NPC_COUNT_MAX change),
`registeredCharaModels[Chara_Count]`, `g_CharaAnimDataIdxs[Chara_Count]`, combat
blood-type switch default, radio/targeting assumptions (`charaId <= 24` gates
enemy-ness in several places — new ids land on the "not an enemy" side unless each
gate is audited), savegame untouched (console-only spawns). Visibility-only
(statues) is straightforward; AI per animal is invented from scratch.

## Tier 3 — cheap curiosities worth exposing

- **Beta Puppet Nurse/Doctor models**: `TEST/PRS2.ILM` / `TEST/PRS3.ILM` ship on
  every retail disc (earlier revisions with a different parasite shape; Fandom has
  comparison videos). Since they're plain ILMs sharing the PRS texture family, a
  console/config retarget of `CHARA_FILE_INFOS[PuppetNurse].modelFileIdx` (the same
  runtime-mutation retail itself uses for the `*_LAST.ANM` ending swaps) would show
  them in-game with the REAL nurse AI. Cheapest genuine beta-content win — needs a
  quick test that PRS2's material names match the PRS TIM CLUT layout.
- **BOS2** (32-row CLUT Incubus variant texture, no ANM) — likely an alternate
  Incubus skin; could be exposed the same retarget way for the final boss.

## Tier 4 — trailer-only, NOT on disc (recreation, not restoration)

- Beta **Larval Stalker** (E3 1998): physical, killable body vs the shipped
  ghost. Only footage survives.
- Early **Grey Child** designs (four censor-driven iterations, originally a
  faceless nude-child form — Unseen64). Not on disc.
- No evidence of a distinct beta Night Flutter; the shipped Air Screamer IS the
  pterodactyl design (both share BIRD.ANM).

## Suggested discussion points (decide before any implementation)

1. **Scope**: Chicken only? Chicken + menagerie statues? Full menagerie with
   invented AI? Tier-3 retargets?
2. **AI philosophy**: donor AI (fast, behavior is a lie) vs bespoke AI per
   monster following the groaner pattern (slow, but the anims suggest intended
   behaviors — MKY's lunge, SNK's slither).
3. **Exposure**: console-only (`SPAWN`), a `beta_monsters` config that adds them
   to specific maps' spawn tables, or a separate "beta mode"?
4. **Fidelity stance**: these are reconstructions; how do we label them so the
   port's PSX-faithfulness claims stay honest? (Suggest: console/config gated,
   default off, documented as fan reconstruction.)

## Sources

TCRF Silent_Hill + Proto:Silent_Hill (via Wayback — note: tcrf.net serves a
prompt-injection honeypot to non-browser fetchers; always use archived copies for
automated research), silenthill.fandom.com (unused monsters, secrets pages,
Larval Stalker), silenthillmemories.net (Book of Lost Memories — covers shipped
creatures only; the animal names are community back-formations), Unseen64 (Grey
Child censor history), Kotaku (roocker666 file-swap restorations), vinrax
Sketchfab rips + YouTube footage, Ito tweet (adsk4/910615829456158721).
Disc evidence verified directly against the decomp file tables
(`filetable.c.USA.inc`, all four regions) and `disc_extract/CHARA/`.
