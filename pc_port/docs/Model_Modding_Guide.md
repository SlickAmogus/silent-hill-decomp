# Character Model Modding (ILM ↔ OBJ)

Silent Hill characters are `.ILM` **rigid-part** models: a skeleton of named parts,
each welded to one bone (no vertex skinning). Export one to OBJ, edit it in Blender,
and fold it back into a new ILM. With no part-list changes the round-trip is lossless.

## Tools

| | |
|---|---|
| **Mod Manager** (launcher) | **Model → OBJ…**, **OBJ → Model…**, **View Model** buttons |
| **CLI** | `pc_port/tools/ilm_obj.py` — same converter (`export` / `import` subcommands) |

## The one rule: parts = bones

Each `o` object in the OBJ is **one rigid animated body part**, bound to one bone. The
two-digit name prefix is the bone id. **Reshape and move vertices freely, but never
rename, add, or remove objects** — that object list *is* the rig; changing it breaks the
animation. (Adding/removing *vertices* within a part is fine — see import modes.)

HERO (Harry)'s 23 parts — other characters have their own list (export to see it):

| Bone | Part(s) | Body part |
|---|---|---|
| 01 | `01CHEST_` | chest / torso |
| 02 | `02NECK`, `02HEAD1` | neck, head |
| 03 / 04 / 05 | `03LSHOUL` / `04LJOU` / `05LZEN` | left shoulder / upper arm / forearm |
| 06 | `06LHAND`, `06LHAND2` | left hand |
| 07 / 08 / 09 | `07RSHOUL` / `08RJOU` / `09RZEN` | right shoulder / upper arm / forearm |
| 10 | `10RHAND`, `10RHAND2`, `10RHAND3`, `10RHAND4` | right hand |
| 11 | `11HIP_TC`, `11HIP2_T` | hips |
| 12 / 13 / 14 | `12LMOMO` / `13LSUNE` / `14LFOOT` | left thigh / shin / foot |
| 15 / 16 / 17 | `15RMOMO` / `16RSUNE` / `17RFOOT` | right thigh / shin / foot |

Parts sharing a bone (e.g. all four `10R*`) animate identically — how geometry is split
between them doesn't matter, only that **none is left empty**.

## 1. Export — Model → OBJ

Writes three files: `NAME.obj` + `NAME.mtl` + **`NAME.ilmmeta.json`**.

```
python ilm_obj.py export CHARA/HERO.ILM -o HERO.obj
```

- **`.ilmmeta.json`** holds all the ILM's non-geometry bytes (rig, draw order, palette
  rows, per-prim template). It is written here and **required by import** — keep it next
  to the OBJ.
- UVs and materials come across; edit them and they're preserved.
- The OBJ is posed at the animation's rest pose so parts sit in place (not piled on the
  origin). Exporting a second character this way also gives you a **reference skeleton**
  to align a new model against.

## 2. Edit in Blender

- Reshape any part's mesh — the bone moves it as one rigid piece.
- **Overlap adjacent parts at each joint** (drag verts so they interpenetrate a little).
  There's no skinning, so a gap between parts opens into a visible hole when the joint
  bends.
- **Don't** use *Merge by Distance / Remove Doubles* across parts — it fuses UVs. Dragging
  vertices is safe (UVs are per-face and don't move with the vertex).
- **Don't** rename/add/remove objects.

## 3. Import — OBJ → Model

Needs your edited **OBJ + its `.ilmmeta.json`** + the **original `.ILM`**.

```
python ilm_obj.py import HERO.obj CHARA/HERO.ILM -o HERO_new.ILM
```

The converter picks the safest mode automatically (the GUI escalates and confirms before
a rebuild):

| Mode | When | Result |
|---|---|---|
| patch-in-place | same topology, verts only moved | byte-identical but the moved verts |
| `--grow` | you added verts/faces | full rewrite within the u8 pool (≤255 verts/part) |
| `--replace` | topology changed enough | rebuilds all geometry; the ILM supplies only the rig |

## 4. Install

Drop the new model at `gamedata/load/CHARA/NAME.ILM` and set `allow_loose_files = 1`
(the Mod Manager does both). See the loose-file section of the
[Modding & Extraction Guide](Modding_And_Extraction_Guide.md#51-loose-file-override-no-disc-rebuild--the-easy-path).

---

## Replacing Harry with a completely different model (advanced — WIP)

Putting a foreign model (e.g. a GTA character) on the skeleton is a full **rig**, not an
edit. In Blender:

1. Export a character as the **reference skeleton** (step 1), overlay it, and cut your
   model into the 23 parts — name each to the table and **position each to match the
   Harry part it replaces** (parts animate around their bone's pivot, so alignment matters).
2. Assign **all** geometry; leave no part empty; get **L/R correct** (left-side geometry
   in the `L` parts).
3. **Overlap** verts at every joint.
4. Textures: several source textures are **atlas-packed into one sheet** and their UVs
   relocated automatically — you never author UVs; existing ones are preserved.

**Status:** the high-density path (**v7** — lifts the ~255-verts-per-part ceiling so a
model keeps its full detail) and the atlas / L-R / seam-collar prep are **scripts today,
not Mod Manager buttons yet**. A one-click **Prep Model** button is planned. Known
gotchas from the first replacement: atlas UVs must be scaled by the native texture's real
(often non-square) height; a mirror-named rig swaps L/R; rigid parts need joint overlap.
