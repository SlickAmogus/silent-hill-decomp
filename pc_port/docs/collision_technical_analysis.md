# Collision and `s_Keyframe`: Technical Analysis

A deep-dive into Silent Hill's collision system for the decomp deobfuscation work. Notes on architecture, the still-unnamed pieces, and suggested renames.

> **Note:** Everything described here is original PSX game code from the upstream decomp. The PC port adds nothing to the collision *logic* — it just adds 64-bit-offset reformat shims and a few NULL-guards because the PSX file format embeds 32-bit relative offsets that have to be widened. The structural analysis below applies to the original disc binary as decompiled into [src/bodyprog/collision.c](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c). The naming suggestions are decomp-team work, not PC-port-specific.

## The two collision worlds

SH1 has **two distinct collision systems** that get bolted together by the per-frame query pipeline:

| | World geometry (IPD) | Character hitbox |
|---|---|---|
| **Source** | `*.IPD` files on disc, one per cell | Per-NPC `s_Keyframe` arrays compiled into rodata |
| **Animated?** | No, baked at design time | **Yes** — interpolated every frame from the active anim's two surrounding keyframes |
| **Carried by** | [`s_IpdCollisionData`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L871) (308 bytes) | `chara->field_C8 / field_D4 / field_D8` (24 bytes total) |
| **Used for** | Walls, floors, triggers, cell-edge transitions | Chara-vs-chara hits, attack hitboxes, body cylinders |
| **Indexed by** | `(cellX, cellZ)` cell coordinates | `chara->model.anim.status` (which anim) + `time` (alpha within keyframe pair) |

The character hitbox is "live" — it changes shape mid-attack, mid-walk, mid-death. Hit detection (e.g. [`func_8008A3E0`](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/bodyprog_combat_8008A058.c#L336)) reads the *current* `field_D4.radius_0` and `field_D8.offsetX_0/Z_2` to know where the body cylinder is *this frame*. World geometry is static — the IPD file says "wall here, floor at this height" once and doesn't change.

## Architecture (per-frame collision query)

```
Game (player/NPC update)
        │
        │  posX, posZ
        ▼
Collision_Get(coll, posX, posZ)              <-- bodyprog/collision.c:183
        │
        ▼
  func_800426E4(posX, posZ)                  <-- bodyprog/gfx/bodyprog_80040B74.c:934
        │  CHUNK_CELL_SIZE = Q12(40.0f)
        │  cellX = floor(geomX / 40.0)
        │  cellZ = floor(geomZ / 40.0)
        │
        │  walks g_Map.ipdActive_15C[] (active chunk slots)
        │  finds the s_IpdHeader whose (cellX, cellZ) matches
        │  returns &ipdHdr->collisionData    (or NULL / fallback)
        │
        ▼
  s_IpdCollisionData*  ipdCollData
        │
        ▼
Collision_QueryInit(state, &pos, &collQuery, …)
        │  memsets state, populates query.position/rotation
        ▼
func_8006AD44(state, ipdCollData)            <-- bodyprog/collision.c:950
        │
        ├── func_8006B004  (compute grid bbox in cells)
        │   sets state.field_A0.s_0.field_0..3 = (startX, startZ, spanX, spanZ)
        │   in cell-grid units  (each cell = field_1C wide)
        │
        ├── for (i=startZ..startZ+spanZ)
        │     for (j=startX..startX+spanX)
        │       func_8006B1C8(state, collData, &collData->ptr_20[i*field_1E + j])
        │           │
        │           │  walks the cell's element-index list (ptr_28)
        │           │  for each idx in [arg2[0].field_0 .. arg2[1].field_0)
        │           │
        │           ├── if idx < field_8_16:
        │           │     func_8006B318(state, collData, idx)
        │           │     // wall/edge element via ptr_14 + ptr_C
        │           │
        │           └── else:
        │                 func_8006C3D4(state, collData, idx)
        │                 // ceiling/floor element via ptr_18
        │
        └── if (state.field_0_10) func_8006C838(state, collData)
            // post-pass: ground height + secondary collisions
                │
                ▼
        Collision_Get fills out `coll`:
          coll->groundHeight_0
          coll->field_4 / field_6  (slope angles?)
          coll->field_8            (probe quality count)
```

That's the read-only ground/wall query. The character-vs-world query (`Collision_WallDetect`, `Collision_CharaCollisionSetup`) runs the same `func_8006A4A8` machinery but returns an **offset** — how far the character has to move to resolve penetration, not just "what's there."

## `s_IpdCollisionData`: the file format

Defined at [include/bodyprog/bodyprog.h:871](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L871). Embedded inside each `s_IpdHeader` at offset `0x54`:

```c
struct s_IpdCollisionData {            //  308 bytes
    s32 positionX_0;                   // World position of cell origin (Q19.12)
    s32 positionZ_4;                   //   (added to local coords inside)
    u32 field_8_0  : 8;                // \  Counts/sizes for the four sub-tables.
    u32 field_8_8  : 8;                //  | field_8_16 is the wall-vs-floor split point
    u32 field_8_16 : 8;                //  | (used by func_8006B1C8 to dispatch).
    u32 field_8_24 : 8;                // /
    SVECTOR3*               ptr_C;     // Vertex pool (SVECTOR3 = 6 bytes each)
    s_IpdCollisionData_10*  ptr_10;    // Material/flag table (12 bytes each)
    s_IpdCollisionData_14*  ptr_14;    // Wall/edge elements (10 bytes each)
    s_IpdCollisionData_18*  ptr_18;    // Floor/ceiling elements (10 bytes each)
    s16  field_1C;                     // Cell sub-grid step (geometry units)
    u8   field_1E;                     // Sub-grid columns (X)
    u8   field_1F;                     // Sub-grid rows    (Z)
    s_IpdCollisionData_20*  ptr_20;    // Per-cell start/end-index pairs (4 bytes)
    u16  field_24, field_26;
    u8*  ptr_28;                       // Element-index pool (referenced by ptr_20[]
                                       //   ranges into here; values index ptr_14/ptr_18)
    void* ptr_2C;
    u8    field_30;                    // "Visit counter" — bumped per query frame
    u8    field_34[256];               // Per-element "last visited" stamps
                                       //   (visited if field_34[idx] > field_30)
};
```

All seven `ptr_*` fields are stored on disc as **byte offsets relative to the start of the `s_IpdCollisionData`**. They get fixed up in place by [`IpdCollData_FixOffsets`](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c#L150) (called from `IpdHeader_FixOffsets`) the first time a chunk loads:

```c
collData->ptr_C  = (u8*)collData->ptr_C  + (uintptr_t)collData;
collData->ptr_10 = (u8*)collData->ptr_10 + (uintptr_t)collData;
// …etc.
```

Because of this, **you cannot simply memcpy a chunk into a different memory location** without re-running `IpdCollData_FixOffsets` (or having stored the original base address). `isLoaded` in the parent `s_IpdHeader` gates this so it only happens once per chunk lifetime.

### The four sub-tables

`s_IpdCollisionData_10` — element materials/flags:

```c
struct s_IpdCollisionData_10 {     // 12 bytes
    s16 field_0, field_2, field_4;
    u16 field_6_0  : 5;            // Material/surface code
                                   //   12 = "low wall" / step (special-cased in
                                   //   func_8006B318 — character bumps over it)
    u16 field_6_5  : 3;            // Surface category (1 = "stairs"?)
    u16 field_6_8  : 3;            // Height/displacement bits
    u16 field_6_11 : 4;
    u16 field_6_15 : 1;
    s16 field_8, field_A;
};
```

The bit fields are interpreted by [`func_8006B1C8`](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c#L1124) and [`func_8006B318`](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c#L1171). Material code `12` is recognized in two places (also `1` is "step"), suggesting they're surface-type IDs the original toolchain emitted from the level editor.

`s_IpdCollisionData_14` — wall/edge elements (10 bytes):

```c
struct s_IpdCollisionData_14 {
    s16 field_0_0  : 14;           // X1 vertex index reference, signed
    u16 field_0_14 : 2;            // High bits → row in collision-flag matrix
    s16 field_2_0  : 14;           // X2 (paired with field_0)
    u16 field_2_14 : 2;            //   → column in collision-flag matrix
    s16 field_4;
    u8  field_6;                   // First vertex index into ptr_C
    u8  field_7;                   // Second vertex index
    u8  field_8;                   // Material/flag idx into ptr_10 (0xFF = none)
    u8  field_9;                   // Second material idx (front/back? walls have two faces)
};
```

The `(field_0_14 << 2) | field_2_14` 4-bit packed value is used as a bit index into [`s_CollisionState::field_2`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L1027) (the per-query 16-bit "collision flags" mask):

```c
// func_8006B318:1181
if (!((collState->field_2 >> (temp_a3->field_0_14 * 4 | temp_a3->field_2_14)) & 1))
    return false;
```

This is **how rooms can selectively enable/disable certain walls per query** — `Collision_FlagsSet(0xFFFF)` enables everything, `Collision_FlagsSet(0x0001)` enables only walls flagged in bit 0. Trigger zones, cutscene blockers, and dynamic doors all use this.

`s_IpdCollisionData_18` — floor/ceiling triangles (10 bytes):

```c
struct s_IpdCollisionData_18 {
    u16      field_0_0  : 5;       // Material code (5 bits = 32 values)
    u16      field_0_5  : 3;       // Surface flags
    u16      field_0_8  : 4;       // Height bits
    u16      field_0_12 : 3;       // ? (slope?)
    u16      field_0_15 : 1;       // 1-bit flag
    SVECTOR3 vec_2;                // Plane normal or vertex offset (Q3.12-ish)
    s16      field_8;              // Y/height value
};
```

`s_IpdCollisionData_20` — per-cell index range (4 bytes):

```c
struct s_IpdCollisionData_20 {
    s16 field_0;       // Start index in ptr_28[]
    s8  unk_2[2];
};
```

`ptr_20` is a `(field_1E × field_1F)` grid — one `s_IpdCollisionData_20` per sub-cell. To get the elements for sub-cell `(j, i)`:

```c
s_IpdCollisionData_20* cell = &ptr_20[i * field_1E + j];
for (s32 k = cell[0].field_0; k < cell[1].field_0; k++) {
    u8 idx = ptr_28[k];
    if (idx < field_8_16)  /* ptr_14[idx] is a wall */
    else                   /* ptr_18[idx - field_8_16] is a floor/ceiling */
}
```

The `cell[1]` access (= the *next* sub-cell's start index) is why the array has one row of "padding" beyond the last real cell. The `field_8_16` value is the split point between wall element indices and floor/ceiling indices in `ptr_28[]` — a single contiguous index pool partitioned by element type.

### `field_30` / `field_34`: the visited-stamp trick

```c
collData->field_30++;                            // bump every query
if (field_30 > 252) { field_30 = 0; memset(field_34, 0, 256); }

if (field_30 >= field_34[idx]) {                 // first visit this query?
    field_34[idx] = field_30 + 1;                //   mark as visited
    /* … process element idx … */
}
```

The 256-byte `field_34` is a **per-element "last visited at" timestamp**, used to dedupe element processing within a single query. Because cells overlap, the same wall can appear in multiple `ptr_20` entries; without this dedupe each wall would get tested 4× when the character straddles a cell boundary. `field_30` wraps every 252 queries so the stamp space stays small. Suggested name: `s_IpdCollisionData::queryGen_30` and `visitedStamps_34`.

## `g_Map`: how chunks become a map

[`s_Map`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L1363) holds the active streaming state:

```c
s_IpdChunk         ipdActive_15C[4];    // 4 simultaneously-resident chunks
s_IpdColumn        ipdGrid_1CC[18];     // 18×16 grid of cell→file-index lookup
s_IpdColumn*       ipdGridCenter_42C;   // Pointer into the grid for current center
s32                cellX_580, cellZ_584;// Current player cell
bool               isExterior_588;
```

`ipdActive_15C[4]` is the LRU set — only 4 chunks fit in PSX RAM at once. As the player walks, [`Map_PlaceIpdAtCell`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L2798) queues file reads into the chunk slots. `func_800426E4` walks this 4-element array linearly to find the chunk whose `cellX/cellZ` matches the query position.

The exterior/interior split (`isExterior_588`) at the bottom of `func_800426E4` matters: if the query position is *outside* the loaded grid in an exterior level (street, alley), it falls back to `&g_Map.collisionData_0` (a default chunk that holds open-air collision). In an interior, an out-of-range query returns NULL and the caller treats it as "void" — `Collision_Get` substitutes `groundHeight = Q12(8.0f)` (deep below floor), which causes characters to fall.

This is also why the active chunk count went from 4 → bigger when porting to PC: 4 was a tight memory budget, not a structural constraint.

## `s_Keyframe`: animated character collision

[`s_Keyframe`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L647) is the per-frame collision shape blob — 20 bytes of Q3.12 values:

```c
struct _Keyframe {
    q3_12 field_0;    // → chara.field_C8.field_0  (height-related)
    q3_12 field_2;    // → chara.field_C8.field_2  (height-related)
    q3_12 field_4;    // → chara.field_C8.field_4
    q3_12 field_6;    // → chara.field_C8.field_6  (Y offset)
    q3_12 field_8;    // → chara.field_D4.radius_0 (collision cylinder radius)
    q3_12 field_A;    // → chara.field_D4.field_2
    q3_12 field_C;    // → chara.field_D8.offsetX_0  (hitbox center X)
    q3_12 field_E;    // → chara.field_D8.offsetZ_2  (hitbox center Z)
    q3_12 field_10;   // → chara.field_D8.offsetX_4  (collision center X)
    q3_12 field_12;   // → chara.field_D8.offsetZ_6  (collision center Z)
};
```

The mapping is established in [`func_80070400`](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c#L3997):

```c
void func_80070400(s_SubCharacter* chara, s_Keyframe* k0, s_Keyframe* k1) {
    q19_12 alpha    = ANIM_STATUS_IS_ACTIVE(chara->model.anim.status)
                      ? Q12_FRACT(chara->model.anim.time)
                      : chara->model.anim.alpha;
    q19_12 invAlpha = Q12(1.0f) - alpha;

    chara->field_C8.field_0   = lerp(k0->field_0,  k1->field_0,  alpha, invAlpha);
    chara->field_C8.field_2   = lerp(k0->field_2,  k1->field_2,  alpha, invAlpha);
    chara->field_C8.field_4   = lerp(k0->field_4,  k1->field_4,  alpha, invAlpha);
    chara->field_C8.field_6   = lerp(k0->field_6,  k1->field_6,  alpha, invAlpha);
    chara->field_D8.offsetX_4 = lerp(k0->field_10, k1->field_10, alpha, invAlpha);
    chara->field_D8.offsetZ_6 = lerp(k0->field_12, k1->field_12, alpha, invAlpha);
    chara->field_D4.radius_0  = lerp(k0->field_8,  k1->field_8,  alpha, invAlpha);
    chara->field_D8.offsetX_0 = lerp(k0->field_C,  k1->field_C,  alpha, invAlpha);
    chara->field_D8.offsetZ_2 = lerp(k0->field_E,  k1->field_E,  alpha, invAlpha);
    chara->field_D4.field_2   = lerp(k0->field_A,  k1->field_A,  alpha, invAlpha);
}
```

(Notice the *order of the assignments* doesn't match struct order — `field_8` writes to `radius_0`, `field_C` writes to `offsetX_0`, etc. Suggested doc-comment: `// FieldOf KeyframeStruct → FieldOf SubCharacter` mapping table.)

### Where `s_Keyframe` data lives

Each NPC keeps a hand-authored array of `s_Keyframe`s in shared rodata, keyed off **anim status** + **time within anim**. Example from [creeper.c:783](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/maps/characters/creeper.c#L783):

```c
case ANIM_STATUS(CreeperAnim_AttackToWalkForward, false):
    func_80070400(creeper, &sharedData_800E0FC8_1_s02, &sharedData_800E0F78_1_s02[0]);
    break;

case ANIM_STATUS(CreeperAnim_AttackToWalkForward, true):
    keyframeIdx0 = FP_FROM(creeper->model.anim.time, Q12_SHIFT);
    keyframeIdx1 = keyframeIdx0 + 1;
    func_80070400(creeper, &sharedData_800E0F78_1_s02[keyframeIdx0],
                           &sharedData_800E0F78_1_s02[keyframeIdx1]);
    break;
```

So the typical pattern per NPC is:

- One **scalar** `s_Keyframe` per anim *blend* state (`(anim, false)`) — pose at start of anim.
- One **array** of `s_Keyframe`s per anim *playback* state (`(anim, true)`) — sampled by floor(time) and ceil(time), interpolated by fract(time).

Each NPC's `Update` function has a giant `switch (anim.status)` that picks which array to call `func_80070400` with. This is hand-authored data — *not* derived from the bone animation file. Your knife's blade hitbox shrinks back into the body at the swing's end because someone tuned that keyframe data by hand.

The "active" anim path uses `time` (not `keyframeIdx`) as the source — `time` advances continuously while `keyframeIdx = floor(time)`, so the lerp gives sub-frame-smooth collision motion even at low keyframe counts.

## `s_CollisionState` and the wall-flag bitmask

[`s_CollisionState`](https://github.com/Vatuu/silent-hill-decomp/blob/master/include/bodyprog/bodyprog.h#L1020) holds the per-query scratchpad. Two bits in `field_0` are worth naming:

| Bit field | Suggested | Purpose |
|---|---|---|
| `field_0_8` | `bumpedWall` | set if the query produced any displacement against a wall (so the response code knows to apply pushback) |
| `field_0_9` | `bumpedFloor` | set if the query found a floor/ceiling penetration |
| `field_0_10` | `runGroundProbe` | tells `func_8006AD44` to also call the post-pass `func_8006C838` (ground-height computation, used by `Collision_Get` but not by basic wall-detect queries) |

`field_2` is the **16-bit "wall enabled" mask** consulted by `func_8006B318` — see the wall-flag dispatch in `s_IpdCollisionData_14` above. Each `_14` element has 4 bits of "category" (`(field_0_14 << 2) | field_2_14`), used as the index. `Collision_FlagsSet(0xFFFF)` enables every category; runtime systems (trigger zones, cutscene scripts) flip individual bits to disable specific walls.

## The chara-vs-world response: `Collision_WallDetect`

The high-level flow for "character wants to move from A to B, is there a wall in the way":

```
Collision_WallDetect(collResult, offset, chara)              <-- collision.c:256
    │  saves SP, swaps to scratch
    ▼
Collision_CharaCollisionSetup(collResult, offset, chara)     <-- collision.c:459
    │  builds collQuery from chara->position, chara->field_C8 (animated bbox),
    │     chara->field_E1_0 (chara state — 0=ignore, 1=player, 3=alive enemy, …)
    │  calls func_800426E4 to fetch the chunk's IpdCollisionData
    │  calls func_800425D8 to fetch ALL active chunks' coll-data ptrs
    │     (so the query can spill across cell boundaries)
    │  calls Collision_ActiveCharactersGet for chara-vs-chara
    │  → func_8006A4A8: walks every collData, calls func_8006AD44 on each
    │
    ▼
Collision_WallResponse(collResult, offset, chara, response)  <-- collision.c:267
    │  classifies result.field_14 → CollisionType (Wall, Step, None)
    │  for walls, does a 9-direction radial probe (POINT_COUNT=9, ANGLE_STEP)
    │     to find the slide vector
    │
    ▼
returns 0 = blocked, 1 = ok-to-move, 2 = step-up
```

The `9 / 370deg` constants in `Collision_WallResponse` are commented as a possible bug:

```c
#define POINT_COUNT          9
#define ANGLE_STEP           Q12_ANGLE(370.0f / POINT_COUNT) // @bug? Maybe `360.0f` was intended.
```

The `370` not `360` produces overlapping probe directions. Whether this was intentional (overlap to dodge corner snags) or a bug is unclear — the `@bug?` comment is correct to flag it. All shipped collision behavior assumes 370 though, so changing it would risk regressions.

## Naming suggestions for the next deobfuscation pass

| Current | Suggested | Notes |
|---|---|---|
| `s_IpdCollisionData::field_8_16` | `wallElementCount` | The split point between `ptr_14[]` (walls) and `ptr_18[]` (floors). Indices `[0..field_8_16)` in `ptr_28[]` index `ptr_14`; `[field_8_16..]` index `ptr_18`. |
| `s_IpdCollisionData::field_8_8` | `floorElementCount` | (corresponding count for ptr_18) |
| `s_IpdCollisionData::field_30` | `queryGen` | "Query generation counter," bumped per Collision_Get call |
| `s_IpdCollisionData::field_34[256]` | `visitedStamps` | Per-element last-visit `queryGen` |
| `s_IpdCollisionData::field_1C` | `subCellSize` | World-units per sub-cell (geometry units, Q23.8) |
| `s_IpdCollisionData::field_1E / field_1F` | `subCellsX / subCellsZ` | Grid dimensions |
| `s_IpdCollisionData::ptr_C` | `vertexPool` | `SVECTOR3[]` — actual vertex coords |
| `s_IpdCollisionData::ptr_10` | `materials` | Per-material flags table |
| `s_IpdCollisionData::ptr_14` | `wallElements` | The wall/edge primitives |
| `s_IpdCollisionData::ptr_18` | `floorElements` | Floor/ceiling triangles |
| `s_IpdCollisionData::ptr_20` | `subCellRanges` | Per-sub-cell `(start_idx_in_ptr_28)` array |
| `s_IpdCollisionData::ptr_28` | `elementIndexPool` | Big concatenated index list referenced by ptr_20 |
| `s_IpdCollisionData_10::field_6_0` | `materialCode` | 5-bit surface code (`12` = step / low wall) |
| `s_IpdCollisionData_14::field_8 / field_9` | `materialIdx_front / materialIdx_back` | `0xFF` = none |
| `s_IpdCollisionData_14::(field_0_14 << 2) \| field_2_14` | `wallFlagBit` | 4-bit index into `s_CollisionState::field_2` mask |
| `s_CollisionState::field_2` | `enabledWallMask` | The 16-bit "which wall categories are active" mask |
| `s_CollisionState::field_0_8 / _9` | `bumpedWall / bumpedFloor` | Result flags |
| `s_CollisionState::field_0_10` | `runGroundProbe` | Selects which post-pass functions run |
| `s_Keyframe::field_8` | `radius` | Maps to `chara.field_D4.radius_0` (cylinder radius) |
| `s_Keyframe::field_C / field_E` | `hitOffsetX / hitOffsetZ` | Maps to `field_D8.offsetX_0 / offsetZ_2` (combat hitbox) |
| `s_Keyframe::field_10 / field_12` | `collOffsetX / collOffsetZ` | Maps to `field_D8.offsetX_4 / offsetZ_6` (collision body) |
| `s_Keyframe::field_0..6` | `bboxTopY / bboxBotY / bboxHeight / bboxYOffset` | Maps to `field_C8.field_0..6` (Y-axis bbox) |
| `s_SubCharacter::field_C8` | `bboxAnimated` | Animated character AABB (Y-axis) |
| `s_SubCharacter::field_D4` | `cylinderAnimated` | Animated character cylinder (radius + ?) |
| `s_SubCharacter::field_D8` | `hitboxOffsets` | Animated XZ offsets (combat + collision centers) |
| `s_SubCharacter::field_E1_0` | `collisionState` | Values: 0=ignore, 1=player, 3=alive enemy (used by `Collision_ActiveCharactersGet` to filter) |
| `func_800426E4` | `Map_IpdCollisionDataAt` | "Get IPD coll data for world position (X, Z)" |
| `func_8006AD44` | `IpdColl_QueryGrid` | "Run query against IPD collision grid" |
| `func_8006B004` | `IpdColl_QueryBboxToCells` | "Convert query AABB to subcell index range" |
| `func_8006B1C8` | `IpdColl_TestSubcell` | "Test all elements in one subcell" |
| `func_8006B318` | `IpdColl_TestWallElement` | "Test one wall (`ptr_14[idx]`)" |
| `func_8006C3D4` | `IpdColl_TestFloorElement` | "Test one floor (`ptr_18[idx]`)" |
| `func_8006C838` | `IpdColl_GroundProbePost` | The post-pass that fills `coll->groundHeight` |
| `func_8006CC44` | `IpdColl_GroundHeightAt` | Look up exact ground height at (X, Z) for the current sub-cell |
| `func_80070400` | `Chara_KeyframeShapeApply` | "Lerp character collision shape between two keyframes" |
| `func_8006DA08` | `Ray_TraceCharaQuery` | Ray-vs-character intersection (existing-name `Ray_LineCheck` covers ray-vs-world) |

## Per-frame data flow (full picture)

```
┌──────────────────────────────────────────────────────────────────────┐
│  Each game frame                                                      │
│                                                                        │
│  ┌── Animation tick                                                    │
│  │   For each NPC:                                                     │
│  │     run AI control func                                             │
│  │     advance anim time                                               │
│  │     case (anim.status):                                             │
│  │       func_80070400(npc, &keyframe[idx0], &keyframe[idx1])          │
│  │            ⇣  writes npc.field_C8 / D4 / D8                          │
│  │       (the character's hitbox and collision body shapes,            │
│  │        interpolated for THIS frame)                                  │
│  │                                                                       │
│  ┌── Move tick                                                          │
│  │   For each character:                                                │
│  │     compute desired offset (vel * dt)                                │
│  │     Collision_WallDetect(&result, &offset, chara)                    │
│  │       ↓                                                              │
│  │       Collision_CharaCollisionSetup → builds collQuery from           │
│  │         npc.position + npc.field_D8.offsetX_4/Z_6 (collision body)    │
│  │       ↓                                                              │
│  │       func_800426E4(npc.posX, npc.posZ) →                            │
│  │         walks g_Map.ipdActive_15C[]                                   │
│  │         returns &chunk.collisionData                                  │
│  │       ↓                                                              │
│  │       func_8006A4A8(…all 4 chunks' collData…)                        │
│  │         for each: func_8006AD44 → grid traversal → result            │
│  │       ↓                                                              │
│  │     Collision_WallResponse → classifies + 9-radial probe             │
│  │     applies result.offset_0 to npc.position                          │
│  │                                                                       │
│  ┌── Combat / interaction tick                                          │
│  │   For each attack (player or NPC):                                   │
│  │     Ray_LineCheck(&ray, &attackerPos, &targetDir)                    │
│  │       ↓ uses field_D8.offsetX_0 / offsetZ_2 (animated hitbox center)  │
│  │     func_8008A3E0(attacker)                                          │
│  │       ↓ uses field_D4.radius_0 (animated body cylinder)              │
│  │     if hit: target.damage.amount_C += weapon.damage * multipliers    │
│  │                                                                       │
│  ┌── Ground tick                                                        │
│  │   For each character:                                                │
│  │     Collision_Get(&coll, npc.posX, npc.posZ)                         │
│  │       sets coll.groundHeight_0 (lerp Y to it)                        │
│  └──                                                                    │
└──────────────────────────────────────────────────────────────────────┘
```

The big takeaway: the **same `func_800426E4` machinery** services every collision query type. Whether it's a wall test, a ground probe, a footstep splash, or a bullet trace — they all pass through "find the chunk for this position, walk its grid, test elements." The shape of the test changes (offset query vs. height query vs. ray query) but the chunk lookup is identical.

## The "void" pattern

`Collision_Get` ([collision.c:183](https://github.com/Vatuu/silent-hill-decomp/blob/master/src/bodyprog/collision.c#L183)) returns a "void" result when the chunk isn't loaded:

```c
if (ipdCollData == NULL) {
    coll->groundHeight_0 = Q12(8.0f);     // 8 world-units below — well below any floor
    coll->field_6 = 0;
    coll->field_4 = 0;
    coll->field_8 = 0;
    return;
}
```

That `Q12(8.0f)` is a sentinel "infinite drop." Game systems that read `groundHeight_0` and try to clamp the character's Y to it will produce a fall — characters in unloaded cells slide into the void rather than getting stuck. This is **load-order tolerance**: an NPC spawned before its chunk loads behaves like it's in mid-air, not like it's trying to climb a wall, until the chunk finishes streaming in. It's a benign failure mode that the rest of the engine relies on, not a bug.

## Open questions / remaining unknowns

- **`s_IpdCollisionData_14::field_4`** (s16). Read in `func_8006B318` but assigned somewhere between `field_2_0` and the vertex offsets. Likely a height/Y value for the wall — check by extracting a known-tall vs known-short wall from a level.

- **Bit-field 12** in `s_IpdCollisionData_10::field_6_0` is special-cased twice (`temp_a0->field_6_0 == 12` at lines 1204 and 1219 of collision.c) — described as adjusting `field_12.vy / field_18.vy` by `-Q12(1.0f)` if `field_4` is set. Looks like "step / low wall — push character down 1 world-unit on contact" but the exact semantics need confirmation.

- **`s_CollisionState::field_A0` union** has `s_0` and `s_1` variants — only `s_0` is referenced in the ground-collision path. `s_1` (with `q7_8 field_0/2`, `s16 field_4`, `u8 field_6`) is presumably the ray-trace variant — confirmed by checking which functions write to `s_1.*` vs `s_0.*` (likely `Ray_TraceRun` family).

- **`field_E1_0` enum**. Currently named `: 4`, used as a state value (not just a flag). Observed values `0` (inactive/dead/skip), `1` (player), `3` (alive enemy). Where's `2`? If never used in shipping code, the enum has 3 active values; could be `e_CharaCollisionState`. The PSX `: 4` bitfield gives 16 possible values — most are unused.

- **`s_IpdCollisionData::field_24 / field_26`**. Header comment says they exist but haven't been seen used yet — might be sizes of `ptr_28 / ptr_2C` (similar to how `field_1E / field_1F` are sizes for `ptr_20`). Verify by checking the IPD file generator (if one is in the museum tools repo) or by extracting two same-cell IPDs and diffing.

- **The 9-direction probe in `Collision_WallResponse`**: `370` instead of `360` is most likely intentional (the overlap dodges the case where a probe ray skims a corner exactly between two probe directions and reports "no wall"), but it's worth confirming by comparing the disassembly to whichever PSX game `func_8006D90C` and the surrounding ray code might have been ported from. Konami's engine reuse across SH1, Snatcher, etc. suggests there's prior art.

- **`s_IpdCollisionData_18::field_0_12 : 3`** — 3 bits inside the floor-element flags, unread by anything in this TU. Possibly slope category (flat / shallow / medium / steep) but never tested in the decomp'd code; might only matter for some unimplemented gameplay path.

- **Per-chara `field_C8` vs `field_D4` purpose split**. Both are written by `func_80070400`. `field_D4.radius_0` is clearly the body cylinder used for chara-vs-chara checks. `field_C8.field_0..6` are described as "top/bottom abs height" — they're probably the *vertical* part of the AABB while the cylinder handles XZ. If true, walking-under-low-ceilings detection (like crouching under a beam) would consult `field_C8`. No clear evidence in this TU; would need to grep how `field_C8.field_0/2` are read by combat/move code.
