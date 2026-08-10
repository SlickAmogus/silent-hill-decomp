# Modern inventory-item models (glTF / GLB)

The PC port can replace Silent Hill's inventory, examine, and world-pickup item geometry with a modern **glTF 2.0 Binary (`.glb`)** file. No disc rebuild or conversion to the original TMD format is required.

If the GLB is missing or unsupported, the game automatically uses the original model from the disc. Removing the GLB restores stock behavior.

## Quick version

Blender's defaults are fine except for one setting.

**In Blender**

1. One mesh object. No armature, animation, or shape keys.
2. UVs inside the 0–1 square.
3. Material: Principled BSDF, Blend Mode **Opaque**, no emission — and tick
   **Backface Culling** (Material Properties → Settings). This is the one
   default you must change: without it Blender marks the material
   double-sided and the game rejects the file.
4. `File → Export → glTF 2.0`, set **Format: glTF Binary (.glb)**, and leave
   everything else alone. (If the scene holds other objects, tick *Selected
   Objects* too.)

**In the game folder**

5. Rename it to the item you're replacing — `UNQ21.glb` is the Health Drink —
   and drop it in `gamedata/load/ITEM/`, beside `SilentHillPC.exe`.
6. Set `allow_loose_files = 1` in `config.cfg`.
7. Launch and pick up or examine that item.

Size doesn't matter: the game rescales your model to the stock item's
footprint. Stay under 8,192 vertices, 8,192 triangles, and 16 MiB. Textures
are optional — embed a single PNG base colour, or ship no texture at all and
inherit the retail one.

If nothing changes, set `enable_debug_log = 1` and search the newest
`SilentHill_*.log` for `[MODERN_MESH]`; it names the exact reason. A bad file
is never fatal — the original item is drawn instead, and deleting the `.glb`
uninstalls it.

The Khronos **Duck** GLB is a known-good test file:
<https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Duck/glTF-Binary>

The launcher's Mod Manager does not stage GLB item models yet, so install the
file manually.

---

Everything below is reference detail — the full accepted profile, texture
workflows, packaging, and troubleshooting. Most people won't need it.

## File naming

Modern item files mirror the original unique-item TMD basename:

```text
gamedata/load/ITEM/<original-UNQ-name>.glb
```

Common examples:

| Item | GLB filename |
|---|---|
| Health Drink | `UNQ21.glb` |
| Ampoule | `UNQ22.glb` |
| House Key | `UNQ41.glb` |
| Channeling Stone | `UNQ56.glb` |
| Kitchen Knife | `UNQ80.glb` |
| Steel Pipe | `UNQ81.glb` |
| Hammer | `UNQ82.glb` |
| Chainsaw | `UNQ83.glb` |
| Axe | `UNQ84.glb` |
| Rock Drill | `UNQ85.glb` |
| Katana | `UNQ86.glb` |
| Handgun | `UNQA0.glb` |
| Hunting Rifle | `UNQA1.glb` |
| Shotgun | `UNQA2.glb` |
| Hyper Blaster | `UNQA3.glb` |
| Handgun bullets | `UNQC0.glb` |
| Rifle shells | `UNQC1.glb` |
| Shotgun shells | `UNQC2.glb` |
| Flashlight | `UNQE0.glb` |
| Pocket Radio | `UNQE1.glb` |
| Gasoline Tank | `UNQE2.glb` |

For the full authoritative mapping, see `pc_port/src/pc_item_unq.c` or search `include/main/fileenum.h.*.inc` for `FILE_ITEM_UNQ`.

Names are case-insensitive on normal Windows installations, but using the exact uppercase basename is recommended for portable mod packages.

## Supported GLB profile

The loader intentionally accepts a small, predictable subset of glTF. Use these export constraints.

### Required

- glTF **2.0 Binary** (`.glb`), not `.gltf` plus sidecar files.
- File size no larger than **16 MiB** (and a valid, non-empty GLB).
- Exactly one selected scene.
- Exactly one reachable, rigid mesh node.
- One mesh; it may contain multiple triangle primitives.
- `POSITION`: `VEC3`, 32-bit float.
- `TEXCOORD_0`: `VEC2`, 32-bit float.
- UVs must stay inside the closed **0–1** range.
- Triangle primitives only.
- Non-empty, non-degenerate geometry and UV triangles.
- At most **8,192 used vertices** and **8,192 triangles** after flattening.
- Vertex indices must be no greater than 8,191.
- Finite transforms, bounds, positions, UVs, normals, and colors.

### Optional and supported

- Multiple primitives, provided they all satisfy the same profile.
- `NORMAL_0`: `VEC3`, 32-bit float.
- `COLOR_0`: `VEC3` or `VEC4` as float, or normalized unsigned 8/16-bit values.
- `TANGENT_0` is accepted but ignored.
- Unsigned 8, 16, or 32-bit scalar indices.
- Cameras or other non-mesh scene nodes.
- Informational `extensionsUsed` entries.
- One embedded base-color PNG texture, as described below.

### Unsupported (falls back to the stock model)

- External buffers or external/data-URI images.
- Any entry in `extensionsRequired`.
- Animation, skinning, bones, morph targets, or morph weights.
- Multiple reachable mesh nodes or multiple meshes.
- Lines, points, strips, or fans.
- Sparse accessors.
- Alpha-blended/masked, double-sided, unlit, or emissive materials.
- UVs outside 0–1.
- Singular/non-finite transforms or degenerate triangles.
- Files or flattened geometry over the limits above.

An unsupported GLB is not fatal: the game logs the reason and renders the original item.

## Textures and materials

Texture selection uses this priority:

1. the GLB's embedded base-color PNG;
2. an installed loose/high-resolution texture override for the original item;
3. the original retail texture already loaded from the disc.

**A self-textured GLB is never overridden by a texture pack.** The two are
authored against different meshes: your embedded PNG is painted for your model's
UVs, while a pack replaces the retail TIM, whose art is laid out for the *stock*
item's UVs. If the pack won, your model would be drawn with an atlas that has no
relationship to its unwrap. The override lookup is therefore skipped entirely for
a self-textured mesh, not merely ranked below it.

The override still applies to a **geometry-only** GLB, which is the case it is
for: that model deliberately inherits the retail binding and is unwrapped against
it, so a higher-resolution version of that same art is exactly what you want.

This lets you choose among three workflows:

### Self-contained GLB

Embed one PNG as the material's `baseColorTexture`. Every primitive must use that same embedded texture on `TEXCOORD_0`. Extra PBR images may be present but are ignored.

The embedded image must:

- be inside the GLB as a `bufferView`;
- declare MIME type `image/png`;
- not use a URI or data URI.

### Geometry-only replacement

A GLB may have no images or textures. The modern geometry inherits the stock item's retail texture binding. Author UVs to match that texture.

### Separate high-resolution texture override

You may pair **geometry-only** modern geometry with the port's normal loose
texture override system. The override applies only when the GLB ships no texture
of its own — a self-contained GLB keeps its embedded PNG regardless of what any
pack replaces. See [Modding & Asset Extraction Guide](Modding_And_Extraction_Guide.md#51-loose-file-override-no-disc-rebuild--the-easy-path).

Materials are flattened to the port's item renderer. Metallic/roughness, normal, occlusion, and other PBR maps are not rendered.

## Blender export recipe

A conservative Blender workflow:

1. Keep the item rigid. Do not add an armature or shape keys.
2. Join the visible item into one mesh object. Multiple material primitives are allowed, but one object is simplest.
3. Apply transforms (`Ctrl+A` → Rotation & Scale). Non-applied finite transforms work, but applying them makes troubleshooting easier.
4. Recalculate normals outside (`Shift+N`). Materials are single-sided.
5. UV unwrap into the 0–1 square; do not tile outside it.
6. Use an opaque, non-emissive Principled BSDF material.
7. If using an embedded texture, use one PNG base-color image shared by every primitive/material.
8. Export **glTF 2.0** with:
   - Format: **glTF Binary (`.glb`)**
   - Include: Selected Objects
   - Data: Mesh
   - Animation: off
   - Skinning: off
   - Shape Keys: off
   - Images: embedded/automatic
9. Give the file the target item's `UNQxx.glb` name and install it under `gamedata/load/ITEM/`.

The game derives a uniform scale from the exported `POSITION` bounds and targets the stock Health Drink extent. Very tiny or huge models are acceptable as long as the resulting scale remains sane; otherwise the loader uses a legacy fallback scale and notes that in the debug log.

## Verifying a mod

For useful diagnostics, temporarily set:

```ini
allow_loose_files = 1
enable_debug_log = 1
```

Start the game, show or pick up the target item, exit normally, then open the newest `SilentHill_*.log` beside the executable.

A successful load contains a line like:

```text
[MODERN_MESH] ACCEPT gamedata/load/ITEM/UNQ21.glb: 2399 vertices, 4212 triangles, embedded texture retained
```

When the inventory links the replacement, the log contains:

```text
[UNIFIEDITEM] carousel link source=modern ...
```

Useful texture diagnostics include:

```text
texture source=installed-override
texture source=embedded-glb
texture source=retail-vram
```

A rejected file produces a specific reason and then uses the native item model, for example:

```text
[MODERN_MESH] reject ...: one rigid, non-animated mesh is required — native item model
```

A missing or rejected replacement remains on the retail carousel path and may be logged as:

```text
[UNIFIEDITEM] carousel link source=stock-pack ...
```

After troubleshooting, turn `enable_debug_log` back off if you do not want verbose logs.

## Fallback and safe removal

The modern-item path is fail-closed:

- `allow_loose_files = 0` completely disables GLB discovery.
- A missing GLB uses the stock model.
- A malformed or unsupported GLB uses the stock model.
- Selecting a stock item after a modern item clears the previous modern handle; stale imported geometry cannot stand in for another item.
- Remove or rename the `.glb` to uninstall the model replacement.

No save data is changed by installing or removing a modern item model.

## Packaging a mod

A minimal distributable package should preserve the game-relative path:

```text
gamedata/
└── load/
    └── ITEM/
        └── UNQ21.glb
```

Include a short README naming the replaced item, tested port version, whether the GLB carries an embedded texture, and the requirement to set `allow_loose_files = 1`.

Do not redistribute the original disc image, original TMD files, or other copyrighted game data.

## Troubleshooting checklist

### Nothing changes

- Confirm `allow_loose_files = 1`.
- Confirm the path is `gamedata/load/ITEM/`, relative to `SilentHillPC.exe`.
- Confirm the exact `UNQxx.glb` filename for the item.
- Enable debug logging and search for `[MODERN_MESH]`.

### The log says the model was rejected

Use the rejection text as the authoritative reason. Common fixes:

- export `.glb`, not `.gltf`;
- embed all data and the PNG;
- remove armatures, animation, and shape keys;
- join down to one mesh object;
- triangulate;
- keep UVs within 0–1;
- make the material opaque, single-sided, and non-emissive;
- reduce the mesh below 8,192 vertices and 8,192 triangles;
- keep the file below 16 MiB.

### The model is white or uses the wrong texture

- Embed a PNG base-color texture, or intentionally UV-map to the retail texture.
- Make sure every primitive uses the same embedded base-color texture.
- Check whether a separate installed texture override is taking precedence.

### Parts are invisible

- Recalculate normals outside.
- Do not use a double-sided material as a workaround; double-sided materials are rejected.
- Remove zero-area triangles.

### The model is present but oddly framed

- Apply Blender transforms and re-export.
- Check for distant stray vertices that enlarge the model's bounds.
- Search the debug log for the `normalize` line, which reports measured source extent and applied scale.

## Scope

This feature replaces **unique inventory/world-pickup item models** (`ITEM/UNQ*.TMD`). It does not replace character ILM files, map geometry, or arbitrary TMD assets. For character models, use [Character Model Modding](Model_Modding_Guide.md).
