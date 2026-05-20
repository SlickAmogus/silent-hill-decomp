# TriggerZone System

TriggerZones are axis-aligned rectangular boxes in world space that define **elevated or depressed floor regions** — raised platforms, stairs, sunken areas, and similar geometry that must respond differently to character movement than the surrounding floor. Despite the name, they do not fire events or cutscenes; they are purely a movement-height system.

## Index

1. [Data structure](#1-data-structure)
2. [Storage and per-map data](#2-storage-and-per-map-data)
3. [Initialization](#3-initialization)
4. [Per-frame update](#4-per-frame-update)
5. [Active-zone cache (`D_800C4478`)](#5-active-zone-cache-d_800c4478)
6. [Collision functions](#6-collision-functions)
   - [Collision_TriggerZonesUpdate](#collision_triggerzonesupdate)
   - [func_8006F250 — movement response](#func_8006f250--movement-response)
   - [func_8006F338 — movement state setup](#func_8006f338--movement-state-setup)
   - [func_8006F3C4 — single-zone intersection](#func_8006f3c4--single-zone-intersection)
   - [func_8006F620 — vertical proximity (flying entities)](#func_8006f620--vertical-proximity-flying-entities)
   - [func_8006F8FC — XZ offset to zone boundary](#func_8006f8fc--xz-offset-to-zone-boundary)
7. [Height formula](#7-height-formula)
8. [PC port notes](#8-pc-port-notes)

---

## 1. Data structure

**[`include/bodyprog/bodyprog.h:1278`](../../include/bodyprog/bodyprog.h#L1278)**

```c
typedef struct _TriggerZone
{
    /* 0x0+0  */ u8  isEndOfArray : 1;  // Sentinel — marks last entry in the array.
    /* 0x+01  */ s32 positionX    : 10; // World X origin, in whole-meter steps.
    /* 0x+011 */ s32 positionZ    : 10; // World Z origin, in whole-meter steps.
    /* 0x0+21 */ u32 sizeX        : 4;  // Width, in whole-meter steps.
    /* 0x0+25 */ u32 sizeZ        : 4;  // Depth, in whole-meter steps.
    /* 0x0+29 */ u32 height       : 3;  // Floor elevation, in half-meter steps (see §7).
} s_TriggerZone;
STATIC_ASSERT_SIZEOF(s_TriggerZone, 4);
```

The entire record is packed into **4 bytes** — a single PSX word. The bitfield layout is as the compiler sees it; MIPS packs from LSB upward.

**Coordinate ranges** given the bit widths:
| Field | Bits | Signed? | Raw range | World range |
|-------|------|---------|-----------|-------------|
| positionX | 10 | yes | −512 … 511 | −512 m … 511 m |
| positionZ | 10 | yes | −512 … 511 | −512 m … 511 m |
| sizeX | 4 | no | 0 … 15 | 0 … 15 m |
| sizeZ | 4 | no | 0 … 15 | 0 … 15 m |
| height | 3 | no | 0 … 7 | 0.0 … 3.5 m elevation |

The zone occupies world X in `[positionX, positionX + sizeX]` and Z in `[positionZ, positionZ + sizeZ]`.

---

## 2. Storage and per-map data

Each map overlay embeds up to **200** zones directly inside `s_MapOverlayHeader` at offset `0xD2C`:

**[`include/bodyprog/bodyprog.h:1757`](../../include/bodyprog/bodyprog.h#L1757)**
```c
typedef struct _MapOverlayHeader {
    // ...
    /* 0xD2C */ s_TriggerZone triggerZones_D2C[200];
} s_MapOverlayHeader;
```

The array is **null-terminated** via `isEndOfArray` — the actual count varies per map and may be far fewer than 200. Code always stops at the sentinel rather than walking all 200 slots.

Per-map zone data is stored in:
```
src/maps/<map>/header_field_D2C.h
```
These files are `#include`d into each map's overlay source and contain one C initializer per zone. Every map section has one (all 43 map sections have a `header_field_D2C.h`). Example entries from `map0_s00`:

```c
// src/maps/map0_s00/header_field_D2C.h
{
  .positionX = -204,
  .positionZ = 141,
  .sizeX = 2,
  .sizeZ = 2,
  .height = 4,
},
{
  .positionX = -60,
  .positionZ = 131,
  .sizeX = 1,
  .sizeZ = 1,
  .height = 1,
},
```

---

## 3. Initialization

When a map loads, `func_80040004()` stores a pointer to the map's zone array into the global `g_WorldGfxWork`:

**[`src/bodyprog/events/bodyprog_80040004.c:21`](../../src/bodyprog/events/bodyprog_80040004.c#L21)**
```c
void func_80040004(s_MapOverlayHeader* overlayHeader) // 0x80040004
{
    g_WorldGfxWork.triggerZone = &overlayHeader->triggerZones_D2C[0];
}
```

This is called from two places during map load:
- `src/bodyprog/game_boot/load_screen.c:74` — during the loading screen
- `src/bodyprog/game_boot/chara_init.c:69` — during character initialization

`g_WorldGfxWork.triggerZone` is declared at `include/bodyprog/bodyprog.h:1317` as `s_TriggerZone*`.

---

## 4. Per-frame update

Every gameplay frame, `func_80040014()` refreshes the active-zone cache against the player's current position:

**[`src/bodyprog/events/bodyprog_80040004.c:26`](../../src/bodyprog/events/bodyprog_80040004.c#L26)**
```c
void func_80040014(void) // 0x80040014
{
    Collision_TriggerZonesUpdate(
        g_SysWork.playerWork.player.position.vx,
        g_SysWork.playerWork.player.position.vz,
        g_WorldGfxWork.triggerZone);
}
```

Called from the main gameplay state in `game_sys_states.c:216` immediately before `vcMoveAndSetCamera`.

---

## 5. Active-zone cache (`D_800C4478`)

**[`include/bodyprog/bodyprog.h:1436`](../../include/bodyprog/bodyprog.h#L1436)**
```c
typedef struct {
    u16            flags_0;
    u8             triggerZoneCount_2;   // Number of zones currently active (0–20).
    u8             unk_3;
    s_TriggerZone* triggerZones_4[20];   // Pointers into the map's zone array.
} s_800C4478;

extern s_800C4478 D_800C4478; // @ 0x800C4478
```

Up to **20** zones are cached here per frame. The collision functions all iterate `D_800C4478.triggerZones_4[0 .. triggerZoneCount_2-1]` rather than scanning the full map array.

---

## 6. Collision functions

### `Collision_TriggerZonesUpdate`

**[`src/bodyprog/collision.c:116`](../../src/bodyprog/collision.c#L116)** — PSX addr `0x80069860`

```c
void Collision_TriggerZonesUpdate(q19_12 posX, q19_12 posZ, s_TriggerZone* zones)
```

Walks the sentinel-terminated zone array and **caches any zone whose extended bounds contain `(posX, posZ)`** into `D_800C4478`.

The extended bounds add a **±16 world-unit buffer** around each zone's exact edges:

```c
minX = FP_TO(zone->positionX,           Q12_SHIFT) - Q12(16.0f);
maxX = FP_TO(zone->positionX + sizeX,   Q12_SHIFT) + Q12(16.0f);
minZ = FP_TO(zone->positionZ,           Q12_SHIFT) - Q12(16.0f);
maxZ = FP_TO(zone->positionZ + sizeZ,   Q12_SHIFT) + Q12(16.0f);
```

The 16-unit buffer is a **sensing radius** — it brings nearby zones into the active set before the player actually steps onto them, giving the intersection functions time to react. Exact zone geometry (no buffer) is used by the intersection tests below.

---

### `func_8006F250` — movement response

**[`src/bodyprog/collision.c:3399`](../../src/bodyprog/collision.c#L3399)** — PSX addr `0x8006F250`

```c
void func_8006F250(s32* arg0, q19_12 posX, q19_12 posZ,
                   q19_12 posDeltaX, q19_12 posDeltaZ)
```

Given a character's current position and intended movement delta, returns:
- `arg0[0]` — distance along the movement vector until the zone boundary (Q12; `Q12(32)` if no intersection)
- `arg0[1]` — zone floor Y offset (Q12, negative; see §7)

Internally sets up a movement state (`func_8006F338`), then iterates `D_800C4478` calling `func_8006F3C4` until an intersection is found (stops at first hit).

---

### `func_8006F338` — movement state setup

**[`src/bodyprog/collision.c:3428`](../../src/bodyprog/collision.c#L3428)** — PSX addr `0x8006F338`

Fills an `s_func_8006F338` scratch structure with:
- Start position, end position (start + delta)
- AABB of the movement segment (for broad-phase rejection)
- Initial `field_28 = Q12(1.0f)` (no penetration yet), `field_2C = Q12(1048560.0f)` (sentinel height)

---

### `func_8006F3C4` — single-zone intersection

**[`src/bodyprog/collision.c:3469`](../../src/bodyprog/collision.c#L3469)** — PSX addr `0x8006F3C4`

```c
bool func_8006F3C4(s_func_8006F338* arg0, const s_TriggerZone* zone)
```

Returns `true` if the movement is **fully inside** the zone (`field_28 == 0`).

Two cases:
1. **Already inside**: if the start position is within zone bounds, `field_28 = 0` and `field_2C` is set to the zone floor height immediately.
2. **Entering**: uses `Vw_LineSegmentIntersectionCheck` to find the fraction `t ∈ [0,1]` of the movement vector at which the segment enters the zone. If `t < field_28` (closer intersection than any previous zone), updates `field_28` and `field_2C`.

The zone floor height stored in `field_2C`:
```c
arg0->field_2C = (-Q12(zone->height) >> 1) - Q12(1.5f);
```
See §7 for the derivation.

---

### `func_8006F620` — vertical proximity (flying entities)

**[`src/bodyprog/collision.c:3551`](../../src/bodyprog/collision.c#L3551)** — PSX addr `0x8006F620`

```c
q19_12 func_8006F620(VECTOR3* pos, s_CollisionQuery* collQuery,
                     q19_12 dist, q19_12 offsetY)
```

Used by NPCs that move in 3D (Air Screamer). For each active zone:
1. Computes zone floor Y (`-Q12(height) >> 1 - Q12(1.5f)`).
2. **Skips** the zone if the entity is **above** it (`posY - zoneHeight >= 0`).
3. Calls `func_8006F8FC` for XZ distance; skips if farther than `dist`.
4. Returns a steering vector pulling the entity away from nearby zones below it.

---

### `func_8006F8FC` — XZ offset to zone boundary

**[`src/bodyprog/collision.c:3666`](../../src/bodyprog/collision.c#L3666)** — PSX addr `0x8006F8FC`

```c
void func_8006F8FC(q19_12* outX, q19_12* outZ,
                   q19_12 posX, q19_12 posZ, const s_TriggerZone* zone)
```

Computes the signed XZ vector from `(posX, posZ)` to the nearest point on the zone's boundary:
- Returns `0` on each axis if the position is already inside the zone on that axis.
- Returns a negative offset if the position is below `minX`/`minZ`, positive if above `maxX`/`maxZ`.

Used by `func_8006F620` (NPC steering) and internally by `func_8006F3C4`.

> **Note:** There is a discrepancy between how the two functions convert zone coordinates to Q12: `func_8006F3C4` uses `Q12(zone->positionX)` while `func_8006F8FC` uses `FP_TO(zone->positionX, Q12_SHIFT)`. For integer inputs these produce the same result; the inconsistency is flagged by a TODO comment in the source.

---

## 7. Height formula

The zone floor Y (stored in `field_2C`, output as `arg0[1]` from `func_8006F250`) is:

```
floor_Y = (-Q12(zone->height) >> 1) - Q12(1.5f)
```

The `>> 1` divides the Q12 value by two, so each unit of `height` contributes **0.5 world units** of elevation — matching the "half-meter steps" comment on the field. The `- Q12(1.5f)` is a fixed offset (likely a character half-height or geometry clearance constant).

Examples:

| height | floor_Y (world units) |
|--------|----------------------|
| 0 | −1.5 |
| 1 | −2.0 |
| 2 | −2.5 |
| 4 | −3.5 |
| 7 | −5.0 |

Higher `height` values represent more deeply **sunken** zones (larger negative Y), not raised ones. A zone at height=0 sits at Y=−1.5, which is approximately ground level.

---

## 8. PC port notes

The TriggerZone system is pure integer math operating on Q12 fixed-point values. It runs correctly on 64-bit without modification — no pointer widths, no PSX address tricks, no GTE involved.

One thing to be aware of: `D_800C4478.triggerZones_4` holds **pointers into the map overlay header** (`g_MapOverlayHeader.triggerZones_D2C`). On the PSX this is a fixed memory address; on PC it points into the statically-allocated `g_MapOverlayHeader` global. As long as the map header is not freed or relocated between `func_80040004` and the collision calls — which it isn't — the pointers remain valid. No special handling is needed.
