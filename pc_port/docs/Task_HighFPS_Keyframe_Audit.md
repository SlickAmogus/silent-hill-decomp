# Task: audit exact-keyframe equality triggers for high-FPS multi-fire

**Status:** not started. Self-contained — a fresh session can start here.
**Owner:** unassigned. **Risk:** low (each fix is a latch, no behaviour change at 30 fps).

## The bug class, and why it is real

Animation time advances **fractionally**, scaled by frame time:

```c
/* src/bodyprog/gfx/bodyprog_anim_800445A4.c, Anim_TimestepGet */
duration = Anim_DurationGet(model, animInfo);
return Q12_MULT_PRECISE(duration, g_DeltaTime);
```

So the integer keyframe — `FP_FROM(model->anim.time, Q12_SHIFT)`, or `model->anim.keyframeIdx`
— stays on the **same value for ~2 consecutive frames at 60 fps and ~4 at 120**.

Any trigger written as an **equality** test therefore fires once per frame it stays true:

```c
if (FP_FROM(chara->model.anim.time, Q12_SHIFT) == 35)   /* fires 1x at 30fps, 2x at 60, 4x at 120 */
```

At 30 fps the keyframe advances a full step per frame, so each equality is true exactly once —
which is why this is invisible on console and on a 30 fps cap.

This is the same family as two already-fixed bugs: the "2 bullets per shot" free-aim/FSM handoff
(`dce9773e0`) and the nurse hurt-SFX "machine-gunning" noted in `puppet_nurse.c`.

## What is already known

- **Range checks are the safe pattern.** `ANIM_TIME_RANGE_CHECK(...)` spans several keyframes and
  is used with a latch (e.g. `split_head.c` footstep SFX latch `splitHeadProps.field_108[]`,
  which resets when the range is exited). 27 uses in `stalker.c` alone, all fine.
- **No unlatched keyframe-range SFX** were found in `src/maps/characters/` — that half is clean.
- **Many equality sites self-latch** because they immediately change `anim.status` or
  `controlState`, so the next frame cannot re-enter. Those need no change.
- **The dangerous ones apply an effect without changing state** — damage, spawn, SFX, decal.

## Sites to audit (19 found)

```
grep -rnE "anim\.time, Q12_SHIFT\) == [0-9]+|keyframeIdx == [0-9]+" src/maps/characters/*.c
```

| file | count |
|---|---|
| monster_cybil.c | 3 |
| hanged_scratcher.c | 3 |
| stalker.c | 2 |
| split_head.c | 2 |
| romper.c | 2 |
| dahlia.c | 2 |
| cybil.c | 2 |
| lisa.c | 1 |
| ghost_child_alessa.c | 1 |
| air_screamer.c | 1 |

Already spot-checked: `split_head.c` ~line 386 (`== 35`) calls `func_8005F6B0`, a collision/decal
routine — double-firing is **cosmetic** (two blood splatters), not double damage. Low priority.

Do **not** widen the search to the whole tree without checking `g_DeltaTime` gating first — most
per-frame code in `src/maps/characters/` is already correctly delta-scaled (see
`project_combat_status_jun2026` memory: motion is FPS-correct; only *decision cadence* and
*frame counters* bite).

## Method

For each site, classify:
1. **Self-latching** — the block changes `anim.status` / `controlState` / a state field before it
   can be re-entered. Leave alone, note it.
2. **Idempotent** — sets a value or plays a looping sound that re-triggering does not compound.
   Leave alone, note it.
3. **Compounding** — applies damage, spawns an entity/particle/decal, or plays a one-shot SFX.
   **Fix.**

For (3) the fix is a latch in the character's own props, matching the existing house pattern
(`split_head.c` `field_108[]`): set on fire, cleared when the keyframe leaves the trigger value.
Do not gate the whole AI, and do not force `g_DeltaTime` — that approach was rejected with reasons
in PR #114.

## Acceptance

- Each of the 19 sites classified in the commit message.
- Only class-(3) sites changed.
- Behaviour at 30 fps byte-identical (a latch cannot fire less often when the equality is already
  true exactly once).
- Verify in game at an uncapped framerate: the affected effect happens once per animation cycle.

## Related

- `project_combat_status_jun2026` memory — the FPS decision-cadence class and the PR #114 rejection.
- `project_freeaim_fsm_handoff_class` memory — the 2-bullets-per-shot precedent.
- Grey Child commit-rate fix `7c6394556` — the *decision cadence* sibling of this class.
