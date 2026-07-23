# RetroAchievements Integration — Feasibility & Design

Status: **RESEARCH / DESIGN ONLY — nothing implemented.** Do not start coding without a go-ahead.
Date: 2026-07-22

## Question asked

1. **A)** Can the PC port use the **existing** RetroAchievements PSX "Silent Hill" achievements?
2. **B)** If not, can we implement **PC-specific** SH1 achievements unique to our port?
3. **Follow-up (the decider):** Can users log in with their **existing RA accounts** and have unlocks show on their **real RA profile**?

## TL;DR

- **A — No.** Technically possible only as an unsanctioned local hack that isn't worth building, and it violates RA policy. Never counts online.
- **B — Yes, technically easy** (our game state is plain in-process C globals), but see the follow-up.
- **Follow-up — No.** Real-account + real-profile integration is only deliverable by the **official** RA.org server, which is closed to this project by written policy. A self-hosted RAWeb instance uses **separate accounts** and shows on a **separate, RA-lookalike profile**, not the user's real one, and adds nothing to their real RA points. The most we can do toward their real account is **cosmetic identity linking** (display their RA username/avatar/points), not write achievements to it.
- **Consequence:** Without real-profile integration, the compelling reason for RA-account users largely evaporates. If anything is built, a **fully in-house achievement system** (optionally using `rcheevos` as the local rule engine) is the pragmatic shape; audience is "players of our port who like achievements," not "RA hunters."

---

## How RetroAchievements works (the two load-bearing facts)

RA achievements are not shipped in the game. The client:
1. Computes a **hash** of the game and asks the server which set to load.
2. Every frame, evaluates each achievement's conditions by **reading raw bytes of console memory** through a callback.

Read-memory callback (from the rc_client integration guide):

```c
uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);
```

For PSX, `address` is an offset into the 2 MB main work-RAM, exposed linearly from `0`
(= physical `0x0` = logical `0x80000000`). So a PSX achievement condition is literally
"read N bytes at absolute PSX RAM address X, compare." **Everything hinges on (1) the hash
and (2) absolute memory addresses** — both of which a native decomp port does not share with
an emulator.

### rc_client API surface (confirmed against the integration wiki)

- `rc_client_create(read_memory, server_call)` — server_call dispatches GET/POST to a host.
- `rc_client_begin_login_with_password(...)` / `..._with_token(...)`.
- `rc_client_begin_identify_and_load_game(client, console_id, path, rom, rom_size, cb, ud)` — takes the ROM/hash.
- `rc_client_do_frame(client)` every frame; `rc_client_idle(client)` ≥1×/sec when paused.
- `rc_client_set_event_handler(...)` → `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`, `..._GAME_COMPLETED`, `..._RESET`.
- `rc_client_set_hardcore_enabled(...)`; server may **demote unlocks if the User-Agent is unrecognized**.

The host the client talks to is configurable (default retroachievements.org; a `host.txt`
points it at a self-hosted server). **Login, game-load, and unlock all go to the same host** —
you cannot authenticate against real RA.org but write unlocks somewhere else.

---

## A) Reusing the existing official set — NO

There is a full official set: **"Silent Hill" (PlayStation), RA game ID 11252 — 66 achievements,
666 points** (2017; challenge-heavy, incl. a sub-~90-min "10-star" full run). RA already unifies
USA/EUR/JAP + fan-translations under that entry via 13 disc hashes. Attractive content, but:

**Technical blockers**
- **Hash is of the disc EXE, not our binary.** RA identifies 11252 as
  `MD5( exe_name_string + first (0x800 + text_size@0x1C) bytes of the PSX boot EXE )`, where the
  EXE (`SLUS_007.07`) is found via `SYSTEM.CNF` `BOOT=`. Our port never runs that EXE; to be
  recognized we'd have to read the user's disc EXE purely to regenerate a hash unrelated to our
  build (= spoofing).
- **Conditions read absolute PSX RAM addresses** of the original runtime layout. Our decomp's
  layout is entirely different, so those addresses are meaningless. We'd need a full **PSX-shaped
  2 MB shadow-RAM mirror** with each struct at its exact PSX offset and encoding — and we **can't
  even obtain the address list** (per-achievement conditions/Code Notes require the authenticated
  RA API; not public).

**Policy blocker (decisive on its own)**
- RA written policy: *"Decompilations, recompilations, and other unofficial ports are not eligible
  for standalone sets."* Direct description of this project.
- Hardcore/leaderboard credit requires an **RA-approved client** (unique `<product>/<version>`
  user-agent, public 6+ months, multi-month compliance review). Unapproved/spoofing clients get
  **Untracked**, tickets closed; deliberate circumvention has caused permanent bans.

**Verdict:** At best an unofficial softcore unlock built on hash-spoofing RA can revoke, after a
large fragile effort against a secret address list, while lying to their live server. Not worth it;
we won't do it.

---

## B) Custom PC-specific achievements — technically easy

Nearly everything worth gating an achievement on is already **first-class, persistent state**,
readable in-process as plain C globals — no emulation, no PSX-address translation. Everything hangs
off two structs:

- `g_SysWork` @ PSX `0x800B9FC0` — live gameplay values
- `g_GameWork` @ `0x800BC728`; active savegame at `+0x30C` (`0x800BCA34`), via `g_SavegamePtr` (`0x80024D48`)

| Hook | Location | Notes |
|---|---|---|
| Story milestones | `s_Savegame.eventFlags[52]` @ `0x800BCB9C` | 1664-bit block; `Savegame_EventFlagGet(EventFlag_X)`; ~700 named flags |
| Endings | `clearGameEndings` @ `0x800BCC7F` (cumulative Good+/Good/Bad+/Bad/UFO); runtime `D_800F481C` | "all endings" = one OR check |
| Ranking / 10-star | rank score `D_800C48B5` (0–100); shot/accuracy stats in savegame | mirrors the official hard achievement |
| Speedrun / no-save | `gameplayTimer` @ `0x800BCC84`, `savegameCount` @ `0x800BCADA`, `continueCount` @ `0x800BCCAF` | |
| No-damage / HP | live `player.health` @ `0x800BA0BC` (Q12; 100.0 = `0x64000`; ≤0 = dead) | |
| Kills | `meleeKillCount`/`rangedKillCount` (nibble-packed) @ `0x800BCC91`/`0x93`; per-map `ovlEnemyStates` alive/dead bits | one death hook: `npc_main.c func_80037E78` |
| Difficulty / NG+ | `gameDifficulty` = top 4 bits of u32 @ `0x800BCC94`; `isNextFearMode`, `clearGameCount` @ `0x800BCC7E` | |

Gating globals: `g_GameWork.gameState` @ `0x800BCCBC` (`GameState_InGame=11`);
`g_SysWork.sysFlags` @ `0x800BC264` (`CutsceneActive=1<<3`, `MenuActive=1<<7`).

**Data gaps to design around**
- **No separate riddle/puzzle difficulty** — only one action `gameDifficulty`. Any "beat Hard
  riddles" achievement must be dropped or backed by new instrumentation.
- **No per-monster-species kill counter** — only melee/ranged totals + per-overlay alive/dead bits.
  Species-specific counts need custom instrumentation at the single death hook.
- Prefer dedicated `EventFlag_*` over `ovlEnemyStates` bits for boss defeats — on Hard the death
  bit is RNG-gated and can miss.
- **PC struct caveat:** the `SH_PC_PORT` build widens `s_SysWork.field_2510` to a 64-bit pointer,
  shifting `g_SysWork` fields **above** `0x2510` (all fields above are below it, but **use symbolic
  access** `g_SavegamePtr->field` / `g_SysWork.field`, never raw addresses, in PC code).

---

## The account / profile question (the decider)

The user's real requirement: **log in with an existing RA account and have unlocks appear on the
real RA profile.** Those two properties are a package that only the official server delivers:

- To appear on a **real profile**, the set must live on **official RA.org**, tied to a recognized
  hash, and unlocks must come from an **RA-approved client**. The port fails both (decomp exclusion;
  unapproved client → Untracked).
- **Self-hosted RAWeb** (Option B's online variant) is an **independent deployment with its own user
  database**. There is **no SSO/federation** with retroachievements.org. Users register a **new
  account**; achievements show on **your** RA-lookalike profile, add **zero** to their real RA
  points/mastery.
- Because `rc_client`'s login + load + unlock all target the **same configured host**, there is no
  "real login → custom unlocks" hybrid.

**Ceiling toward a real account = cosmetic identity linking only.** Using RA's public web API, we can
verify a user owns an RA account and display their **username/avatar/total points** beside our port's
own achievements. We can **read** their RA identity; we cannot **write** achievements to their RA
profile.

**The only way SH1 achievements hit a real RA profile is a real emulator + real disc — never the port.**

---

## Options summary

| Option | Real RA login | Shows on real RA profile | Feasible for this port | Notes |
|---|---|---|---|---|
| A — existing PSX set on official server | (spoof) | No (Untracked/ban risk) | No — policy + ToS | Don't build |
| B(a) — new **official** PC set | Yes | Yes | **No** — decomp exclusion | ~0% chance via RA pipeline |
| B(b) — self-hosted RAWeb + `rc_client` | Separate account | No (separate profile) | Yes | Hosting/moderation burden; low user appeal |
| B(c) — **in-house** system (± `rcheevos` engine) | Optional cosmetic RA link | No | **Yes — recommended if anything** | Own toast/UI; fits port; no real-profile payoff |
| Discord Rich Presence | N/A | N/A (status only) | Yes | "Harry is in Midwich School" presence, not achievements |
| Steam/GOG/EOS achievements | N/A | N/A | No | Requires being that store's publishable app; a fan decomp of Konami IP can't |

---

## Recommendation

Given that the headline feature the user wants (real RA account + real profile) is **not achievable**,
the honest recommendation is:

1. **Default: don't build the online piece.** A separate-account RAWeb instance won't attract the RA
   crowd, and it carries hosting/moderation/redistribution baggage.
2. **If achievements are still wanted for the port's own players:** build a **fully in-house system**
   (Option B(c)) — own tracking + in-game toasts + persistence — optionally using `rcheevos`/`rc_client`
   purely as the local rule-evaluation engine with a bundled ruleset, and optionally with cosmetic RA
   identity linking so hunters at least see their RA name/points in-app.
3. **Consider a lighter win:** Discord Rich Presence (map/scene/HP status) is low-friction, needs no
   store publishing, and gives visible social value without the achievement infrastructure.

---

## In-house implementation sketch (only if greenlit)

Because conditions are authored against **offsets we control**, there is **no PSX-address shim** — the
read-memory callback is a bounds-checked `memcpy` from a small per-frame shadow buffer.

1. **Frame pump.** Once per frame from the main loop, `memcpy` `g_SavegamePtr->savegame` + needed
   `g_SysWork` fields into a flat shadow buffer, then run the evaluator. Gate to real gameplay
   (`gameState == GameState_InGame`; skip while paused/menu/cutscene) so nothing fires mid-load.
2. **Read-memory callback:** trivial memcpy from the shadow buffer. Encode Q12 / nibble-packed /
   bitfield semantics in the **rules**, not the callback.
3. **Event handler:** on trigger → in-game toast + persist unlock state.
4. **Cheat-eligibility latch (important):** the port ships a dev console, cheat keys, a randomizer,
   and enemy-cap overrides. Latch an "achievements-eligible" session flag **off** the moment the
   console opens, any cheat key is used, `unlimited_enemies` is on, or a non-default config map was
   applied on New Game.

**Effort (in-house):** author list 2–4d · integrate engine + callback + pump + gating 3–6d · shadow
copy + encode rules + map achievements 4–8d · toast UI + persistence + latch 4–7d · full playthrough /
all-endings / NG+ testing 3–6d → **~16–31 person-days**. Self-hosted RAWeb adds **+5–10d** for little
user payoff.

## Open questions before any go-ahead

- Is an **online/leaderboard** component actually wanted, now that it can't touch real RA profiles?
- Would **cosmetic RA identity linking** (show RA name/points, achievements local) satisfy the intent,
  or is it not worth it without real-profile writes?
- Draft achievement list scope (endings, boss milestones, no-save run, sub-90-min run, ranking).

## Sources / evidence

- RA game 11252 "Silent Hill" (PlayStation): 66 achievements / 666 points.
- PSX hashing: docs.retroachievements.org game-identification + rcheevos `hash_disc.c`.
- Standalone/decomp exclusion + hardcore compliance: docs.retroachievements.org standalone-support,
  hardcore-compliance-requirements.
- rc_client contract: rcheevos wiki `rc_client-integration`.
- Port state addresses: `configs/USA/sym.bodyprog.txt`, `src/bodyprog/sys/syswork_globals.c`,
  `include/bodyprog/savegame.h`, `include/event_flags.h`, `src/bodyprog/ranking.c`.
