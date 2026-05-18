# Inventory Rendering & World Item Positioning — PC Port Technical Notes

This document explains how the Silent Hill inventory 3-D model system works, why world items (pickups) were invisible in the PC port, and why inventory items showed wrong models — and how both were fixed.

---

## 1. How World Item Pickups Are Positioned

### PSX Data Layout

Every map overlay (`.BIN` file) contains a `g_CommonWorldObjectPoses[]` array in its data segment. Each element is an `s_WorldObjectPose` struct ([`include/maps/shared.h`](../../include/maps/shared.h)):

```c
typedef struct {
    VECTOR3  position;   // 3 × s32, 12 bytes  (Q19.12 fixed-point world coords)
    SVECTOR3 rotation_C; // 3 × s16, 6 bytes   (Q3.12 fixed-point angles)
    // 2 bytes implicit struct padding
} s_WorldObjectPose;     // total: 20 bytes per entry
```

Each map's header C file (`map*_header.c`) references entries from this array when placing world objects (pickups like ammo boxes, health drinks, keys). The relevant call is `WorldGfx_ObjectAdd()`, which receives a position pointer and a rotation pointer from an element of `g_CommonWorldObjectPoses`.

### The PC Port Bug — Symbol Collisions via EXE Export

On the PSX, `g_CommonWorldObjectPoses` data lives in the overlay binary and is loaded into RAM when the map loads. In the PC port, each map is built as a Windows DLL (`build/maps/map*.dll`).

The symbol is declared `extern` in the map headers but has **no definition in any C source file** for most maps. On PSX this was fine because the linker pulled the data from the binary. On PC it is a problem:

1. The EXE (`SilentHillPC.exe`) was built with a zero-filled stub in [`pc_port/src/stubs/data_stubs.c`](../src/stubs/data_stubs.c) as a fallback.
2. MinGW builds the EXE with `--export-all-symbols`, so every global in the EXE is visible to DLLs.
3. A DLL that references `g_CommonWorldObjectPoses` but doesn't define it locally resolves the symbol through a `.refptr` import thunk pointing back into the EXE's zero-filled stub.
4. **Result:** All world item positions were `(0, 0, 0)` — the world origin. Since the player spawns nearby and the camera faces outward, origin-placed items were always behind the camera and never rendered.

**Log evidence** (from `Gfx_WorldObjectDraw`):
```
[WOBJ-BEHIND] model=BULLET_N Q8pos=(0,0,0) viewZ=-3840
```

### The Fix

1. **Removed the zero stub** from `data_stubs.c` so the EXE no longer exports a competing definition.
2. **Extended [`pc_port/tools/extract_map_data.py`](../tools/extract_map_data.py)** to extract `g_CommonWorldObjectPoses` from each map's PSX binary at the virtual address listed in `configs/USA/maps/sym.map*.txt`.
3. The tool emits a proper `s_WorldObjectPose name[N] = { ... }` initializer into each map's auto-generated `pc_port/build_gen/extracted_data/map*_extracted_data.c`, which is compiled into the DLL. A local DLL definition always wins over an EXE import, so the correct data is used.

Maps that already defined the array in their C source (e.g. `map1_s00_events_data.c`) are skipped via the `SKIP_SYMBOL_FOR_MAP` set in the tool.

---

## 2. How the Inventory 3-D Model System Works

The inventory screen shows a rotating 3-D model for each item. Understanding this requires following four distinct layers.

### 2.1 TMD Data — the Model Pack

When the inventory opens, [`GameFs_MapItemsTextureLoad()`](../../src/bodyprog/items/item_screens_3.c) (`item_screens_3.c:3803`) loads a per-map TMD pack (e.g. `FILE_ITEM_IT_001_TMD`) into a fixed filesystem buffer `FS_BUFFER_8`. This pack contains the 3-D model data for all items loadable on that map.

### 2.2 The Loadable Item List — `LOADABLE_INVENTORY_ITEMS`

Each map has a zero-terminated `u8` array `LOADABLE_INVENTORY_ITEMS[]` that lists the item IDs whose models are in the TMD pack, in the same order. It is declared `extern` in [`include/maps/shared.h`](../../include/maps/shared.h).

`Gfx_Items_DrawInit()` (`item_screens_3.c:4037`) copies this list into `g_Item_MapLoadableItems[]` at inventory-open time.

### 2.3 The TMD Cache — `GsMapModelingData` / `GsGetTMDObject`

`GameFs_TmdDataAlloc(FS_BUFFER_8)` (`item_screens_cam.c:119`) calls `GsMapModelingData()` ([`pc_port/src/stubs/libgs_stub.c`](../src/stubs/libgs_stub.c)). This parses the PSX TMD format out of the buffer, allocates a 64-bit `TMD_STRUCT` with vertex/normal/primitive pointers, and stores it in a static 64-slot cache keyed on the buffer address (`tmd_hdr = &buf[1]`).

`GsGetTMDObject(tmd_hdr, index)` searches newest-first through the cache and returns the model at `index` within the pack.

### 2.4 Linking Models to Display Slots — `Gfx_Items_Display`

`Gfx_Items_Draw()` (`item_screens_3.c:3911`) iterates the player's inventory and for each item whose ID is in `g_Item_MapLoadableItems`, calls:

```c
Gfx_Items_Display(FS_BUFFER_8, displayItemIdx, loadableItemIdx);
// item_screens_3.c:3959
```

`Gfx_Items_Display()` (`item_screens_3.c:4112`) does:

```c
struct TMD_STRUCT* obj = GsGetTMDObject(tmd_hdr, loadableItemIdx); // look up by pack index
GsLinkObject4_PC(obj, &g_Items_ItemsModelData[displayItemIdx]);    // store in display slot
```

`g_Items_ItemsModelData[0..8]` is a static array of 9 `GsDOBJ2` structs, one per display slot. Slots 0–6 are the carousel strip; slot 7 is the equipped-item display (rendered separately, see §3).

### 2.5 Per-Frame Rendering — `Gfx_ItemScreens_DrawInit`

Called every frame by `item_screens_2.c:404`. For each occupied display slot it:

1. Calls `ItemScreen_ItemRotate()` to spin the item (modifies the `GsCOORDINATE2` rotation matrix in place).
2. Calls `func_8004BD74(slotIdx, obj, 0)` (`item_screens_cam.c:141`) which builds the combined world+view matrix, corrects for widescreen aspect ratio, and calls `GsSortObject4J()` to emit draw primitives.

The 3-D position of each slot is determined by `g_Items_Coords[i].coord.t[0/1/2]`, set during `Gfx_Items_Draw()` and updated per-frame by the scroll animation code.

---

## 3. The Wrong-Model Bug — `LOADABLE_INVENTORY_ITEMS` Symbol Collision

### Root Cause

The same EXE-export collision problem that affected world item positions also hit `LOADABLE_INVENTORY_ITEMS`. Map0_s00's definition (in the EXE) listed only 8 items: `[HealthDrink, FirstAidKit, Ampoule, KitchenKnife, SteelPipe, Hammer, Chainsaw, Axe]`. All map DLLs that didn't define the symbol locally resolved it to this 8-item list.

When `Gfx_Items_Draw()` ran for any other map (e.g. map2_s00 — the street area), it searched the player's actual inventory items against this 8-item list. Items like `HandgunBullets` (ID `0xC0`) were not in the list, so **zero `Gfx_Items_Display` calls were made** and no TMD links were established.

`Gfx_ItemScreens_DrawInit` (per-frame render) used the stale `g_Items_ItemsModelData[i].tmd` pointers left over from the last time inventory was opened on a different map. The player saw whatever models were linked on the previous map.

### The Fix

Added `LOADABLE_INVENTORY_ITEMS` to the same extraction tool ([`pc_port/tools/extract_map_data.py`](../tools/extract_map_data.py)), with null-terminator detection to find the actual array length. The tool extracts the correct per-map list from the PSX binary and emits it into each DLL's `extracted_data.c`. Maps that already define it in their `_anim_info.c` source (map0_s01, map1_s04, map2_s01, map2_s03, map2_s04, map5_s03) are skipped.

---

## 4. The Equipped-Item Y Position Bug

### Three Y-Coordinate Sites

The equipped item's 3-D display position (display slot 7) is set by `g_Items_Coords[7].coord.t[1]`. This value must be set consistently at three places:

| Site | Location | When it runs |
|---|---|---|
| A | `Gfx_Items_Draw()`, `item_screens_3.c:3994` | Every inventory open |
| B | `Inventory_PlayerItemScroll()` case 5, `item_screens_3.c:2816` | Each frame of equip animation |
| C | `Inventory_PlayerItemScroll()` case 6, `item_screens_3.c:2862` | Each frame of unequip animation |

### The Bug

Site A had a wrong `#ifdef SH_PC_PORT` override of `Q8(5.0f)` (= +1280 units, pushing the item **below** the carousel). Sites B and C used the original PSX value `Q8(-2.5f)` (= −640 units). After equipping and re-opening inventory, site A's wrong value took effect, placing the pistol at the bottom of the carousel box.

### The Fix

All three sites now use `Q8(-4.0f)` on PC (= −1024 units, placing the item above the carousel in the "Equipment" column area), with the original `Q8(-2.5f)` preserved for the PSX build. The PSX and PC values differ because the PC view projection maps world Y to screen Y at a different scale than the original hardware.

---

## 5. The Extraction Tool

[`pc_port/tools/extract_map_data.py`](../tools/extract_map_data.py) reads each map's sym file (`configs/USA/maps/sym.map*.txt`) for symbol addresses and sizes, then extracts raw bytes from the PSX binary (`disc_extract/VIN/MAP*.BIN`) at `VA - load_base`. It generates `pc_port/build_gen/extracted_data/map*_extracted_data.c` with C initializers.

**Key design points:**
- `NULL_TERM_SYMBOLS` — for `LOADABLE_INVENTORY_ITEMS`, scans for the null terminator instead of trusting the sym-file size.
- `SKIP_SYMBOL_FOR_MAP` — prevents duplicate definitions for maps that already define a symbol in their C source.
- Files marked `MANUALLY MAINTAINED` in their header are never overwritten.
- Run with: `python3 pc_port/tools/extract_map_data.py` from the repo root.
