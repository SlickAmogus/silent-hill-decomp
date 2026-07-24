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
| `--v7` (high-poly) | a whole new mesh, or you need >255 verts/part | rebuilds as a PC-only high-poly file with no vertex cap; the ILM supplies only the rig. **The button's "High-poly replace".** |

## 4. Install

Drop the new model at `gamedata/load/CHARA/NAME.ILM` and set `allow_loose_files = 1`
(the Mod Manager does both). See the loose-file section of the
[Modding & Extraction Guide](Modding_And_Extraction_Guide.md#51-loose-file-override-no-disc-rebuild--the-easy-path).

---

## Replacing Harry with a completely different model

Putting a foreign model (e.g. a GTA character) on the skeleton is a full **rig**, not an
edit. The **geometry** work is yours in Blender; the **texture + format** work is the
button's. What does what:

| Step | Who |
|---|---|
| Cut / name / position / assign the 23 parts | **You** (Blender) |
| Left / right naming | **You** — the button *validates*, doesn't fix |
| Fix winding | **You** — one click (Recalculate Outside) |
| Overlap joints (close seams) | **You** — drag verts |
| Pack the textures into one sheet + fix the UVs | **Button** (auto) |
| Relabel materials to the game's CLUT rows | **Button** (auto) |
| High-poly (v7) conversion + rest-pose meta | **Button** |

### 1. Rig (Blender)

1. **Model → OBJ** on the character you're replacing (e.g. HERO) to get a **reference
   skeleton** — all 23 parts, named, at the rest pose. Import it beside your model.
2. Cut your model into the 23 parts: Edit mode, select a region, **P → Selection**. Name
   each **exactly** to the reference's part name — the two-digit prefix *is* the bone.
3. **Position each part to overlay the reference part** it replaces. Parts animate around
   their bone's pivot, so a part far from its reference swings wrong.
4. **Assign all geometry** — no part left empty. Parts sharing a bone (e.g. the four
   `10R*` hand parts) animate identically, so how you split them doesn't matter; only that
   none is empty.
5. **Left / right**: name each part after the reference part it sits on, so `L` geometry
   lands in an `L` part. Get this wrong and the arms cross the body under animation — the
   button warns, but does not fix it.

### 2. Clean up (still Blender)

- **Winding** (one click): Edit mode → select all (**A**) → **Mesh → Normals → Recalculate
  Outside** (**Shift+N**). The game backface-culls, so inconsistent winding shows as holes.
- **Seams**: rigid parts aren't skinned, so a gap at a joint opens into a hole when it
  bends. Drag the boundary verts of adjacent parts to **overlap** a little at each joint
  (neck↔chest, thigh↔hip, arm segments, wrist↔hand). Do **not** *Merge by Distance* across
  parts — it fuses UVs; just move them.

### 3. Export

File → Export → Wavefront (.obj) **with the .mtl** (Blender writes your textures into it).
The object list must stay = the 23 parts.

### 4. Build (button)

**Mod Manager → OBJ → Model** → **High-poly replace: Yes** → **Auto texturing: Yes**. Pick
your OBJ and the ORIGINAL ILM (e.g. `HERO.ILM`). It packs your textures into one sheet,
fixes the UVs (with the native-TIM V-fix), relabels the materials, mints the rest-pose
data, and writes `<CHARA>_new.ILM` **and** `<CHARA>.TIM.png`.

> Already have a single sheet + game materials? Choose **Auto texturing: No** and it just
> builds the v7. CLI (v7 only, no atlas): `ilm_obj.py import prepped.obj HERO.ILM --v7`.

### 5. Install

Drop **both** files into `gamedata/load/CHARA/` under the ORIGINAL names (`HERO.ILM` and
`HERO.TIM.png`) and set `allow_loose_files = 1`.

### What the button checks

- **Missing** part (fewer objects than the rig) → refused.
- **Empty** part (no faces) → warned; it'll be invisible.
- Part **far from where the donor expects it** → warned (mis-assigned geometry or L/R swap).

### Why the tool does what it does (gotchas)

- **V-fix**: HERO's texture is **256×192** (not square) and the loose-override shader
  samples `u/256, v/192`. The atlas UVs are pre-scaled by native-height/256 so they land
  1:1 — skip it and the lower body samples into empty atlas space (a black band).
- **Materials**: the game addresses one texture slot by CLUT-row material names
  (`mat00_rowNN`); the button relabels your model's materials to the donor's rows.
- **Geometry stays in Blender**: winding, L/R and joint overlap are intent the tool can't
  read as well as you can, so it warns rather than guesses. (An automatic seam-collar /
  winding-fix / L-R un-mirror pass exists as prototype scripts; ask if you want them added
  to the button.)
