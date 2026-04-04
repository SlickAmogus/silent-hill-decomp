# Ordering Table & DrawOTag Pipeline

## Background

This document explains how the PSX ordering table (OT) system works, how Silent Hill uses it, and how the PC port bridges it to OpenGL via PsyCross. This is useful context for anyone working on the decompilation who wants to understand how geometry submitted by the original game code ends up on screen.

---

## What Is the Ordering Table?

The PSX GPU has no hardware depth buffer. Instead, all geometry is sorted and drawn back-to-front on the CPU using a **linked list of primitive packets** called the Ordering Table.

The OT is an array of `u_long` values. Each slot is a node in a singly-linked list — the lower 3 bytes hold a pointer to the next node, and the upper byte holds the length of the primitive packet data that follows it at that address. The GTE transforms geometry, computes an average Z depth, and the game inserts each primitive into the corresponding OT slot. At draw time, `DrawOTag()` walks the list from back (farthest) to front (nearest), sending each primitive to the GPU — implementing the painter's algorithm in software.

```
OT[N]  → (farthest, drawn first)  [POLY_GT3] → [POLY_G4] → ...
OT[N-1]→                          [SPRT]     → ...
...
OT[0]  → (nearest, drawn last)    [POLY_GT3] → ...
    ↑ tag pointer (walk starts here)
```

`GsClearOt()` resets the array each frame with `ClearOTagR()`. `GsSortClear()` inserts a screen-clear primitive at the farthest slot.

---

## How Silent Hill Uses the OT

Silent Hill maintains three ordering tables, double-buffered on `g_ActiveBufferIdx`:

| Table | Purpose |
|---|---|
| `g_OrderingTable0` | Main 3D world geometry (environment, characters, fog) |
| `g_OrderingTable1` | (secondary, less commonly used) |
| `g_OrderingTable2` | 2D overlays: HUD text, screen fades, cutscene borders |

The frame loop in `src/bodyprog/sys/game_main.c`:

```
GsSwapDispBuff()               // flip double-buffer index
GsSortClear(fogColor, OT0)     // insert clear-screen prim at OT0 far end
  ... all game subsystems sort prims into OT0/OT2 ...
GsDrawOt(OT0)                  // walk OT0 → send to GPU
GsDrawOt(OT2)                  // walk OT2 → send to GPU
PsyX_EndScene()                // swap SDL/OpenGL buffers
```

Adding geometry to the OT is done with `addPrim(ot->tag + depthSlot, &myPoly)` — the slot acts as a bucket, and multiple prims in the same bucket draw in LIFO order.

---

## The Layered Architecture

The PC port sits across three layers:

```
Original game code (decompiled C)
         │  calls GsDrawOt(), GsClearOt(), GsSortClear(), etc.
         ▼
PSY-Q libgs API  (declared in include/psyq/libgs.h)
         │  re-implemented by us in pc_port/src/stubs/libgs_stub.c
         ▼
PSY-Q libgpu API  (DrawOTag, DrawPrim, etc.)
         │  provided natively by PsyCross
         ▼
PsyCross ParsePrimitivesLinkedList()  (PsyCross/src/gpu/PsyX_GPU.cpp)
         │
         ▼
OpenGL  (glDrawArrays)
```

The original game source **never changes** — it still calls `GsDrawOt()` exactly as on PSX. The entire PC port strategy is: keep the game source identical, re-implement the PSY-Q surface beneath it.

### Why Two Layers (libgs vs libgpu)?

Sony's PSY-Q SDK was split into a lower-level `libgpu` (raw primitive submission, `DrawOTag`) and a higher-level `libgs` (coordinate systems, lighting, OT management, `GsDrawOt`). PsyCross re-implements `libgpu` and `libgte` natively. It does **not** include `libgs`. So we only needed to write stubs for `libgs` — `libgpu` comes for free from PsyCross.

---

## Our `libgs_stub.c` — The Hook Point

`pc_port/src/stubs/libgs_stub.c` implements every `GsXxx()` function. For `GsDrawOt` specifically:

```c
void GsDrawOt(GsOT *ot)
{
    if (ot && ot->tag)
    {
        glClearDepth(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        DrawOTag((u_long*)ot->tag);   // hand off to PsyCross
    }
}
```

It unpacks the `GsOT` wrapper struct, clears the depth buffer (PsyCross adds a real depth buffer that PSX never had), and delegates to PsyCross's `DrawOTag`. The `glClear` is needed because PsyCross's `PsyX_EndScene` is not called between draw calls, so depth state from the previous OT draw would otherwise bleed through.

---

## OT Sanitizer

Because the original geometry subsystems write PSX-native primitive codes, some types that are valid on PSX crash PsyCross — notably `DR_MODE` (`0xE0`) in OT0, line primitives (`0x40`/`0x50`), and unrecognised codes. A sanitizer in `game_main.c` walks the OT *before* `GsDrawOt()` and zeroes the `len` field of any unsafe primitive, making it a passthrough no-op node that `DrawOTag` skips.

Allowed primitive code ranges:

| Code | Type |
|---|---|
| `0x00` | Terminator / pass-through |
| `0x20` | Flat-shaded polygons (`POLY_F3/F4`) |
| `0x30` | Gouraud-shaded polygons (`POLY_G3/G4`) |
| `0x60` | Textured flat polygons (`POLY_FT3/FT4`) |
| `0x70` | Textured gouraud polygons (`POLY_GT3/GT4`) |
| `0xA0` | Sprites (`SPRT`) |
| `0xE0` | Draw env commands (`DR_TPAGE`) — OT2 only |

---

## Inside PsyCross: `DrawOTag` → OpenGL

PsyCross uses a **two-pass design**: first accumulate all vertices into a flat CPU buffer, then batch-upload and issue one `glDrawArrays` per render-state group.

### Pass 1 — Walk the OT and Decode Primitives

`DrawOTag` (`PsyCross/src/psx/libgpu.c`) calls `ParsePrimitivesLinkedList` (`PsyCross/src/gpu/PsyX_GPU.cpp`), which walks the linked list using `nextPrim()` / `getlen()` / `isendprim()` macros and calls `ParsePrimitive()` for each node.

`ParsePrimitive` dispatches on `P_TAG.code`:

```
code & 0xF0  → major type:
  0x20  → ProcessFlatPoly()
  0x30  → ProcessGouraudPoly()
  0x40  → ProcessFlatLines()
  0x60/0x70 → ProcessTileAndSprt()
  0xE0  → ProcessDrawEnv()   (state-only, no vertices)

code & 0x0C  → sub-variant within major type:
  0x0  → triangle  (G3 / F3)
  0x4  → textured triangle  (GT3 / FT3)
  0x8  → quad  (G4 / F4)
  0xC  → textured quad  (GT4 / FT4)
```

For each primitive, vertices are written directly into a statically-allocated flat array:

```cpp
GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
int g_vertexIndex = 0;
```

Example for `POLY_GT3`:

```cpp
GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, dither);
MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);
g_vertexIndex += 3;
```

**Note on quads:** PSX quads (`POLY_G4`, `POLY_GT4`) are fan-triangulated into 6 vertices (2 triangles) because OpenGL doesn't support quads natively. The winding order is `v0,v1,v3,v0,v3,v2` — with v2/v3 swapped — because PSX quads are specified in a "Z" shape, not an "N" shape.

`MakeVertexTriangle` also applies `DrawEnvOffset`, adding `activeDrawEnv.ofs[x/y]` to every screen coordinate to center the PSX coordinate system (PSX origin = screen center, OpenGL origin = top-left).

`0xE0` draw-env commands (`DR_TPAGE`, `DR_MODE`, `DR_AREA`) hit `ProcessDrawEnv()`, which mutates `activeDrawEnv` state without writing vertices. They always trigger a split boundary (see below).

### Splits — Batching by Render State

Every time the **texture page, blend mode, or clip rect** changes between primitives, PsyCross starts a new `GPUDrawSplit`:

```cpp
struct GPUDrawSplit {
    DRAWENV   drawenv;     // clip rect, dither, offset
    BlendMode blendMode;   // none / additive / subtractive / average
    TexFormat texFormat;   // 4bpp / 8bpp / 16bpp / 32bpp VRAM
    TextureID textureId;   // g_vramTexture or g_whiteTexture (untextured)
    u_short   startVertex; // offset into g_vertexBuffer
    u_short   numVerts;
};

GPUDrawSplit g_splits[MAX_DRAW_SPLITS]; // up to 4096 per frame
int g_splitIndex = 0;
```

`AddSplit()` is called before writing each primitive's vertices. If the incoming tpage/blend matches the current split, it just continues appending. If different, `g_splitIndex++` opens a new split. This translates from the PSX per-primitive state model to OpenGL's stateful model without one draw call per primitive.

### Pass 2 — `DrawAllSplits`: Upload and Draw

After `ParsePrimitivesLinkedList` returns, `DrawOTag` calls `DrawAllSplits()`:

```cpp
GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);
//  └─ glBufferSubData(GL_ARRAY_BUFFER, ...) — uploads entire buffer in one call

for (int i = 1; i <= g_splitIndex; i++)
    DrawSplit(g_splits[i]);

ClearSplits();  // g_vertexIndex = 0, g_splitIndex = 0
```

`DrawSplit` sets OpenGL state for the batch and fires one draw call:

```cpp
GR_SetStencilMode(split.drawPrimMode);
GR_SetTexture(split.textureId, split.texFormat);
GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
GR_SetBlendMode(split.blendMode);
GR_DrawTriangles(split.startVertex, split.numVerts / 3);
//  └─ glDrawArrays(GL_TRIANGLES, startVertex, numVerts)
```

---

## Full Call Chain

```
GsDrawOt(GsOT*)                        pc_port/src/stubs/libgs_stub.c
  └─ DrawOTag(u_long*)                  PsyCross/src/psx/libgpu.c
       ├─ PsyX_BeginScene()             setup FBO, begin scene
       ├─ ParsePrimitivesLinkedList()   PsyCross/src/gpu/PsyX_GPU.cpp
       │    └─ per OT node:
       │         ParsePrimitive()
       │           ├─ AddSplit()        batch boundary check
       │           └─ MakeVertex* / MakeTexcoord* / MakeColour*
       │                └─ write into g_vertexBuffer[g_vertexIndex++]
       ├─ glFinish()                    sync before draw
       └─ DrawAllSplits()
            ├─ glBufferSubData()        upload all vertices at once
            └─ per split: DrawSplit()
                 └─ glDrawArrays(GL_TRIANGLES, ...)
```

---

## Key Files

| File | Role |
|---|---|
| `pc_port/src/stubs/libgs_stub.c` | Our libgs re-implementation; `GsDrawOt` hook point |
| `src/bodyprog/sys/game_main.c` | Frame loop: OT clear, sanitizer, `GsDrawOt` calls |
| `PsyCross/src/psx/libgpu.c` | `DrawOTag` entry point |
| `PsyCross/src/gpu/PsyX_GPU.cpp` | `ParsePrimitivesLinkedList`, `AddSplit`, `DrawAllSplits`, `DrawSplit` |
| `PsyCross/src/render/PsyX_render.cpp` | `GR_UpdateVertexBuffer`, `GR_DrawTriangles` (OpenGL calls) |
| `include/psyq/libgs.h` | PSY-Q libgs API declarations |
