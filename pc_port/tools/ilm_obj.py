#!/usr/bin/env python3
"""ILM <-> OBJ converter for Silent Hill character/object models.

    export  ILM [-o out.obj] [--anm A.ANM] [--keyframe N]
                                    ILM -> OBJ + MTL + .ilmmeta.json
    import  OBJ  ILM [-o new.ILM]   edited OBJ + the ORIGINAL ILM -> new ILM

An SH1 .ILM holds several "models" that are really rigid BODY PARTS (CAT.ILM:
01BODY_T, 02FRLEG1 ... 18TAIL2). Animation transforms each part as a unit, so a
part is exported as one OBJ object (`o NAME`) and import matches parts BY NAME.
Renaming, adding or deleting an object therefore breaks animation, and import
refuses to do it.

Import always needs the original ILM as a template: several fields have no known
meaning (MeshHeader.unkPtr_14/unkCount_3, Material.field_14/16, the ModelHeader
bitfield at 0xB) and are copied through verbatim rather than guessed at.

Geometry facts this relies on, all verified against real files:
  * Primitive vertex/normal indices are GLOBAL; the per-mesh array index is
    `prim_index - ModelHeader.vertexOffset` (normalOffset for normals). Verified
    on all 19 CAT.ILM parts.
  * A vertex is (verticesXy[i].vx, verticesXy[i].vy, verticesZ[i]) - XY and Z
    live in two separate arrays.
  * A TRIANGLE sets the 4th vertex index (byte at prim+0xF) to 0xFF and leaves
    the 4th UV garbage. A QUAD is two triangles in PSX FT4 winding - see
    QUAD_LOOP for why that is NOT an OBJ face order.
  * UVs are u8 per axis packed as u16, U in the low byte, V in the high byte.
  * Pointers in the file are file-relative offsets.

ILM vertices are stored in the part's OWN BONE frame, never in model space: the
draw path loads one matrix per bone (Vw_CoordToWorldAndViewMatrices ->
SetRotMatrix/SetTransMatrix in func_8005A900) and feeds the raw ILM values to
gte_rtpt with no pre-multiply. Exporting them verbatim therefore piles every
part on the origin. The rest pose that places them lives in the .ANM, so export
BAKES it in (world = R*v/4096 + T) and freezes the matrices into the meta.

Baking is also what makes the shared-pool seam references meaningful: the pool
slot a part reads from a neighbour holds that neighbour's ALREADY-TRANSFORMED
vertex, so resolving it to the owner's LOCAL vertex welds two different bone
frames together - the spikes. See resolve_pool() and export().
"""

import argparse
import json
import math
import os
import struct
import sys

TRI_SENTINEL = 0xFF

Q12 = 4096
IDENTITY_R = [Q12, 0, 0, 0, Q12, 0, 0, 0, Q12]

# An edit smaller than this in world units is treated as "the artist did not
# touch it": Blender's measured OBJ round-trip drift is ~3e-05 and the local
# quantisation step is 0.5, so nothing real lives in the gap.
UNMOVED_EPS = 1e-3

# Normals need their own, much looser gate. Blender stores custom split normals
# compressed, so a normal survives its OBJ round-trip only to ~0.66 degrees
# (measured worst case: 1.15e-02 over 5684 normals in 22 CHARA models) - 25x
# the vertex drift and 1.5x the s8 quantisation step itself. 0.05 is ~2.9
# degrees: above the transport noise with 4x margin, still far below any
# deliberate edit.
NORMAL_UNMOVED_EPS = 0.05

# PSX Y grows downward; OBJ/Blender expect Y up, so Y is negated in both
# directions. Values are small, so the s16 range is never at risk.
def _y_out(v): return -v
def _y_in(v):  return -v


# A PSX FT4 quad is a triangle STRIP: func_8005AC50 copies the four indices
# straight into POLY_GT4 x0..x3, which the GPU renders as (v0,v1,v2)+(v1,v2,v3).
# An OBJ `f` is a polygon LOOP, so emitting 0,1,2,3 makes every quad cross
# itself - the bowtie that shatters the model in Blender while leaving every
# edge LENGTH unchanged, so no spike metric can see it. OBJ corner i carries
# prim corner QUAD_LOOP[i]. Swapping 2 and 3 is an involution, so the same
# table maps OBJ corners back to prim corners on import.
QUAD_LOOP = (0, 1, 3, 2)
TRI_LOOP = (0, 1, 2)


def corner_order(prim):
    return TRI_LOOP if prim["tri"] else QUAD_LOOP


# ILM normals point INWARD: dotted against (face centre - part centroid) in the
# part's own local space, 93.5% of faces in the first 20 CHARA models come out
# negative. The Y-negation above preserves that sign (M^T M == I), so emitting
# them the way positions are emitted lights every model from the inside. Normals
# therefore get their own direction convention - the extra flip is deliberate,
# not a copy of the position path.
def _normal_out(v): return (-v[0], v[1], -v[2])
def _normal_in(v):  return (-v[0], v[1], -v[2])


# ---- rest pose (.ANM) -------------------------------------------------------

class Anm:
    """Reader for the skeleton half of a .ANM (include/bodyprog/formats/anm.h).

    Only the bind poses and one keyframe are needed. There is no true bind pose
    in the data: Anim_BoneInit only writes a rotation for bones whose
    rotationDataIdx is negative, and for HERO every non-root bone has one, so a
    keyframe must supply the rotations. Keyframe 0 is a reasonable default but
    is NOT reliably an idle pose - several creature rigs are mid-motion there.
    Pass --keyframe if a model exports in an awkward pose.
    """

    def __init__(self, data):
        self.d = data
        if len(data) < 0x14:
            raise ValueError("too short for an ANM header")
        (self.dataOffset, self.rotationBoneCount, self.translationBoneCount,
         self.keyframeDataSize, self.boneCount, _pad, self.activeBones,
         self.fileSize, self.keyframeCount, self.scaleLog2,
         self.rootYOffset) = struct.unpack_from('<HBBHBbIIHBB', data, 0)
        self.bind = []
        for i in range(self.boneCount):
            o = 0x14 + i * 6
            if o + 6 > len(data):
                raise ValueError("bind pose %d past end of file" % i)
            p, r, t, tx, ty, tz = struct.unpack_from('<6b', data, o)
            self.bind.append({"parent": p, "rot": r, "trans": t, "t0": (tx, ty, tz)})

    def valid(self):
        if self.boneCount <= 0 or self.keyframeCount <= 0:
            return False
        if self.rotationBoneCount * 9 + self.translationBoneCount * 3 != self.keyframeDataSize:
            return False
        if self.dataOffset < 0x14 + self.boneCount * 6:
            return False
        # fileSize is the PADDED size, so several real files run 1-3 bytes over
        # their keyframe block; the tail check must be <=, never ==.
        return self.dataOffset + self.keyframeDataSize * self.keyframeCount <= len(self.d) + 4

    def _s8(self, o):
        return struct.unpack_from('<b', self.d, o)[0]

    def local(self, bone, kf):
        """Per-bone local (R q12 row-major, T) - Anim_BoneInit + Anim_BoneUpdate."""
        b = self.bind[bone]
        base = self.dataOffset + self.keyframeDataSize * kf
        s = self.scaleLog2
        if b["trans"] < 0:
            T = [v << s for v in b["t0"]]
        else:
            o = base + b["trans"] * 3
            T = [self._s8(o + i) << s for i in range(3)]
            if b["trans"] == 0:
                T[1] -= self.rootYOffset
        if b["rot"] < 0:
            R = list(IDENTITY_R)
        else:
            o = base + self.translationBoneCount * 3 + b["rot"] * 9
            R = [self._s8(o + i) << 5 for i in range(9)]
        return R, T

    def world(self, kf):
        """Compose W_child = W_parent * L_child for every bone (Vw_CoordHierarchyMatrixCompute).

        Resolved recursively: the bind table is not guaranteed to list parents
        before children, and a bad parentBone (out of range, or self) must be
        treated as a root rather than crash - the game only survives those via
        the COORD_PTR_OK guard in vw_calc.c.
        """
        out = [None] * self.boneCount
        state = [0] * self.boneCount  # 0 unvisited, 1 in progress, 2 done

        def resolve(i):
            if state[i] == 2:
                return out[i]
            if i == 0:
                # The root bone is NEVER animated. Anim_BoneInit calls
                # GsInitCoordinate2 (identity) and then loops from boneIdx = 1,
                # as does Anim_BoneUpdate - see bodyprog_anim_800445A4.c:64 and
                # :158. Bone 0's keyframe translation is root MOTION; what the
                # game actually puts in boneCoords[0].coord.t is the character's
                # map position (bloody_lisa.c:76, dahlia.c:100, kaufmann.c:82).
                # Baking it would float every model ~280 units off the floor.
                out[i] = (list(IDENTITY_R), [0, 0, 0])
                state[i] = 2
                return out[i]
            if state[i] == 1:
                print("   WARNING: bone %d is in a parent cycle; treated as a root" % i)
                R, T = self.local(i, kf)
                out[i] = (R, T)
                state[i] = 2
                return out[i]
            state[i] = 1
            R, T = self.local(i, kf)
            p = self.bind[i]["parent"]
            if 0 <= p < self.boneCount and p != i:
                PR, PT = resolve(p)
                NR = [0] * 9
                for a in range(3):
                    for b in range(3):
                        # Arithmetic floor-shift, matching the GTE. Do NOT
                        # "clean this up" to /4096: it rounds toward zero on
                        # negatives and diverges from the C# port.
                        NR[a * 3 + b] = sum(PR[a * 3 + k] * R[k * 3 + b] for k in range(3)) >> 12
                NT = [(sum(PR[a * 3 + k] * T[k] for k in range(3)) >> 12) + PT[a] for a in range(3)]
                R, T = NR, NT
            out[i] = (R, T)
            state[i] = 2
            return out[i]

        for i in range(self.boneCount):
            resolve(i)
        return out


def bone_of(name):
    """func_800452EC: the first two ASCII digits of the part name ARE the bone index."""
    if len(name) >= 2 and name[0].isdigit() and name[1].isdigit():
        return (ord(name[0]) - 48) * 10 + (ord(name[1]) - 48)
    return 0


def mat_inverse(R):
    """True 3x3 inverse of a q12 matrix, returned q12-scaled (inv(R/4096)*4096).

    The transpose is NOT usable here: every element is an s8<<5 and every
    composition step truncates >>12, so the composed matrices are only roughly
    orthonormal (|det| falls to ~0.72) and a transpose-unbake is wrong on 84% of
    the corpus. inv(R_q12) == inv(R_real)/4096, so the factor of 4096 below is
    load-bearing - drop it and every vertex comes back scaled by 4096 while
    still looking like a plausible small number.
    """
    m = [R[i] / float(Q12) for i in range(9)]
    a, b, c, d, e, f, g, h, i = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-9:
        return None
    inv = [(e * i - f * h), -(b * i - c * h), (b * f - c * e),
           -(d * i - f * g), (a * i - c * g), -(a * f - c * d),
           (d * h - e * g), -(a * h - b * g), (a * e - b * d)]
    return [v / det for v in inv]


def mat_inf_norm(M):
    return max(abs(M[r * 3]) + abs(M[r * 3 + 1]) + abs(M[r * 3 + 2]) for r in range(3))


def bake(R, T, v):
    """Local -> world. Float divide, not >>12: this is presentation, not the GTE."""
    return [sum(R[a * 3 + k] * v[k] for k in range(3)) / float(Q12) + T[a] for a in range(3)]


def bake_dir(R, v):
    return [sum(R[a * 3 + k] * v[k] for k in range(3)) / float(Q12) for a in range(3)]


def unbake(Rinv, T, w):
    d = [w[a] - T[a] for a in range(3)]
    return [sum(Rinv[a * 3 + k] * d[k] for k in range(3)) for a in range(3)]


def unbake_dir(Rinv, w):
    return [sum(Rinv[a * 3 + k] * w[k] for k in range(3)) for a in range(3)]


# ILM stem -> ANM stem, transcribed from CHARA_FILE_INFOS
# (src/bodyprog/sys/chara_data_info.c). First entry wins where one model has
# several animation sets (SIBYL/DARIA/KAU/MAR).
ILM_TO_ANM = {}
for _ilm, _anm in [
    ("HERO", "HB_BASE"), ("BIRD", "BIRD"), ("BD2", "BIRD"), ("DOG", "DOG"),
    ("DG2", "DOG"), ("CLD1", "CLD1"), ("CLD2", "CLD2"), ("CLD3", "CLD2"),
    ("CLD4", "CLD2"), ("SLT", "SLT"), ("COC", "COC"), ("JACK", "JACK"),
    ("CKN", "CKN"), ("FAT", "FAT"), ("MTH", "MTH"), ("PRS", "PRS"),
    ("DUMMY", "DUMMY"), ("PRSD", "PRS"), ("WRM", "WRM"), ("ROD", "ROD"),
    ("BOS", "BOS"), ("MAR", "MAR"), ("MSB", "MSB"), ("DEAD", "DEAD"),
    ("SIBYL", "SBL"), ("SRL", "SRL"), ("CAT", "CAT"), ("DARIA", "DA"),
    ("LISA", "LS"), ("BLISA", "BLS"), ("AR", "AR"), ("TAR", "TAR"),
    ("BAR", "BAR"), ("KAU", "KAU"), ("BFLU", "BFLU"), ("LITL", "LITL"),
    ("DOC", "DOC"), ("ICU", "ICU"),
]:
    ILM_TO_ANM[_ilm] = _anm


def _anm_dirs(ilm_path):
    d = os.path.dirname(os.path.abspath(ilm_path))
    return [os.path.join(os.path.dirname(d), "ANIM"), d]


def find_anm(ilm_path, models, override=None):
    """Locate the .ANM holding this model's skeleton, or None.

    A candidate is accepted only if it parses, passes its arithmetic
    invariants, AND covers every bone index the part names ask for - that last
    check is what correctly rejects the tempting PRS2 -> PRS.ANM guess.
    """
    maxbone = max([bone_of(m["name"]) for m in models] + [0])
    stem = os.path.splitext(os.path.basename(ilm_path))[0].upper()
    cands = []
    if override:
        cands.append(override)
    else:
        for name in ([ILM_TO_ANM[stem]] if stem in ILM_TO_ANM else []) + [stem]:
            for d in _anm_dirs(ilm_path):
                cands.append(os.path.join(d, name + ".ANM"))
    for c in cands:
        if not os.path.exists(c):
            continue
        try:
            anm = Anm(open(c, "rb").read())
        except Exception as ex:
            print("   WARNING: %s is not a readable ANM (%s)" % (os.path.basename(c), ex))
            continue
        if not anm.valid():
            print("   WARNING: %s failed the ANM header sanity checks" % os.path.basename(c))
            continue
        if maxbone >= anm.boneCount:
            print("   WARNING: %s has %d bones but this model needs bone %d"
                  % (os.path.basename(c), anm.boneCount, maxbone))
            continue
        return c, anm
    return None, None


def rest_poses(ilm_path, models, anm_override, keyframe):
    """Per-part (R q12, T) placing it in model space, plus provenance for the meta."""
    path, anm = find_anm(ilm_path, models, anm_override)
    ident = [(list(IDENTITY_R), [0, 0, 0]) for _ in models]
    if anm is None:
        stem = os.path.splitext(os.path.basename(ilm_path))[0].upper()
        if anm_override:
            tried = "%s was given with --anm but is not usable" % os.path.basename(anm_override)
        else:
            tried = ("looked for %s in ANIM/ and alongside the ILM"
                     % " and ".join(sorted(set([ILM_TO_ANM.get(stem, stem), stem]))))
        print("")
        print("   " + "!" * 68)
        print("   !! NO REST POSE for %s" % os.path.basename(ilm_path))
        print("   !! No usable .ANM was found (%s)." % tried)
        print("   !! Every part is exported in its OWN local space, so they will sit")
        print("   !! piled on the origin and joint seams will not line up.")
        print("   !! The round-trip is still byte-exact - only the layout is wrong.")
        print("   !! Pass --anm PATH if you know which animation file this model uses.")
        print("   " + "!" * 68)
        print("")
        return ident, {"anm": None, "keyframe": keyframe, "restPose": "identity"}

    kf = keyframe
    if not 0 <= kf < anm.keyframeCount:
        print("   WARNING: keyframe %d is out of range (%s has %d); using 0"
              % (kf, os.path.basename(path), anm.keyframeCount))
        kf = 0
    world = anm.world(kf)
    poses = [world[bone_of(m["name"])] for m in models]

    # Guard the unbake budget: half a quantisation step is 0.25 local units and
    # a 4-decimal OBJ carries ~5e-5 of world error. Measured worst case across
    # the corpus is 2.05, so this should never fire; it exists to turn "measured
    # on the keyframes I tried" into "enforced on whatever the user feeds it".
    for i, (R, T) in enumerate(poses):
        inv = mat_inverse(R)
        if inv is None or mat_inf_norm(inv) * 5e-5 >= 0.25:
            print("")
            print("   " + "!" * 68)
            print("   !! part %s has a %s rest-pose matrix; baking it would not survive"
                  % (models[i]["name"], "singular" if inv is None else "badly conditioned"))
            print("   !! the round-trip. Falling back to LOCAL space for the WHOLE model")
            print("   !! (parts piled on the origin). Try a different --keyframe.")
            print("   " + "!" * 68)
            print("")
            return ident, {"anm": None, "keyframe": keyframe, "restPose": "identity"}

    return poses, {"anm": os.path.basename(path), "keyframe": kf, "restPose": "anm"}


# ---- reading ----------------------------------------------------------------

class Ilm:
    def __init__(self, data):
        self.d = data
        d = data
        if d[0] != ord('0') or d[1] != 6:
            raise SystemExit("not an ILM (magic %r version %d)" % (chr(d[0]), d[1]))
        self.matCount = d[3]
        self.matsP = self._u32(4)
        self.modelCount = d[8]
        self.modelHdrsP = self._u32(0xC)
        self.modelOrderP = self._u32(0x10)
        self.materials = [self._material(i) for i in range(self.matCount)]
        self.models = [self._model(i) for i in range(self.modelCount)]

    def _u8(self, o):  return self.d[o]
    def _u16(self, o): return struct.unpack_from('<H', self.d, o)[0]
    def _s16(self, o): return struct.unpack_from('<h', self.d, o)[0]
    def _s8(self, o):  return struct.unpack_from('<b', self.d, o)[0]
    def _u32(self, o): return struct.unpack_from('<I', self.d, o)[0]

    def _name(self, o):
        return self.d[o:o + 8].split(b'\0')[0].decode('ascii', 'replace')

    def _material(self, i):
        b = self.matsP + i * 24
        return {"idx": i, "off": b, "name": self._name(b),
                "field_10": self._u16(b + 0x10),
                "baseClutY": self._u16(b + 0x10) >> 6}

    def _model(self, i):
        b = self.modelHdrsP + i * 16
        m = {"idx": i, "off": b, "name": self._name(b),
             "meshCount": self._u8(b + 8),
             "vertexOffset": self._u8(b + 9),
             "normalOffset": self._u8(b + 0xA),
             "bits": self._u8(b + 0xB),
             "meshHdrsP": self._u32(b + 0xC)}
        m["meshes"] = [self._mesh(m, k) for k in range(m["meshCount"])]
        return m

    def _mesh(self, model, k):
        mb = model["meshHdrsP"] + k * 24
        me = {"off": mb, "primCount": self._u8(mb), "vertexCount": self._u8(mb + 1),
              "normalCount": self._u8(mb + 2), "unkCount_3": self._u8(mb + 3),
              "primsP": self._u32(mb + 4), "xyP": self._u32(mb + 8),
              "zP": self._u32(mb + 0xC), "normP": self._u32(mb + 0x10),
              "unkP": self._u32(mb + 0x14)}
        me["verts"] = [(self._s16(me["xyP"] + v * 4),
                        self._s16(me["xyP"] + v * 4 + 2),
                        self._s16(me["zP"] + v * 2)) for v in range(me["vertexCount"])]
        me["normals"] = [(self._s8(me["normP"] + n * 4), self._s8(me["normP"] + n * 4 + 1),
                          self._s8(me["normP"] + n * 4 + 2), self._u8(me["normP"] + n * 4 + 3))
                         for n in range(me["normalCount"])]
        me["prims"] = [self._prim(me["primsP"] + p * 20) for p in range(me["primCount"])]
        return me

    def _prim(self, pb):
        flags = self._u16(pb + 6)
        vtx = [self.d[pb + 0xC + i] for i in range(4)]
        tri = vtx[3] == TRI_SENTINEL
        uvw = [self._u16(pb + o) for o in (0, 4, 8, 0xA)]
        return {"off": pb, "raw": bytes(self.d[pb:pb + 20]),
                "uv": [(w & 0xFF, (w >> 8) & 0xFF) for w in uvw],
                "clut": self._u16(pb + 2), "flags": flags,
                "materialIdx": (flags >> 8) & 0x7F,
                "isTransparent": (flags >> 15) & 1,
                "field_6_0": flags & 0xFF,
                "vtx": vtx, "nrm": [self.d[pb + 0x10 + i] for i in range(4)],
                "tri": tri}


def resolve_pool(ilm):
    """Map every primitive index to the (model, local index) it actually reads.

    Primitives do NOT index their own mesh: func_8005759C copies each part's
    vertices into a per-character scratch pool at ModelHeader.vertexOffset
    (screenXy_0[vertOffset]) and the prims index that POOL. Parts are processed
    in LmHeader.modelOrder, and their pool ranges overlap on purpose - a part
    reads vertices an earlier part left in the pool wherever they meet, which is
    how joint seams stay welded (CAT 01BODY_T reads pool 16, written by
    10HIP_TC). Resolving therefore means replaying the pool in draw order.
    """
    d = ilm.d
    order = list(d[ilm.modelOrderP:ilm.modelOrderP + ilm.modelCount])
    if sorted(order) != list(range(ilm.modelCount)):
        order = list(range(ilm.modelCount))  # not a permutation: fall back to file order
    vpool, npool = {}, {}
    for mi in order:
        model = ilm.models[mi]
        for k, mesh in enumerate(model["meshes"]):
            for j in range(mesh["vertexCount"]):
                vpool[model["vertexOffset"] + j] = (mi, k, j)
            for j in range(mesh["normalCount"]):
                npool[model["normalOffset"] + j] = (mi, k, j)
            for p in mesh["prims"]:
                n = 3 if p["tri"] else 4
                p["vref"] = [vpool.get(p["vtx"][i]) for i in range(n)]
                p["nref"] = [npool.get(p["nrm"][i]) for i in range(n)]
    return order


def clut_row(prim, materials):
    """Palette row a prim samples: (clut >> 6) - (material.field_10 >> 6)."""
    mi = prim["materialIdx"]
    base = materials[mi]["baseClutY"] if 0 <= mi < len(materials) else 0
    return (prim["clut"] >> 6) - base


def mtl_name(prim, materials):
    return "mat%02d_row%02d%s" % (prim["materialIdx"], clut_row(prim, materials),
                                  "_alpha" if prim["isTransparent"] else "")


# ---- export -----------------------------------------------------------------

def part_layout(ilm, model):
    """Ordered (own, foreign) vertex/normal reference lists for one `o` block.

    Every OBJ object must be self-contained - a face may only cite indices
    emitted inside its own block. Blender's OBJ exporter sorts objects
    alphabetically and re-partitions vertices by which faces touch them, so the
    old globally-numbered layout (a face in part A citing a `v` line inside part
    B) cannot survive a Blender round-trip at all. Seam vertices are therefore
    DUPLICATED into every part that reads them, baked by their OWNER's matrix.
    """
    mi = model["idx"]
    own_v = [(mi, k, j) for k, m in enumerate(model["meshes"]) for j in range(m["vertexCount"])]
    own_n = [(mi, k, j) for k, m in enumerate(model["meshes"]) for j in range(m["normalCount"])]
    own_vs, own_ns = set(own_v), set(own_n)
    for_v, for_n = [], []
    seen_v, seen_n = set(), set()
    for k, mesh in enumerate(model["meshes"]):
        for p in mesh["prims"]:
            n = 3 if p["tri"] else 4
            for i in range(n):
                vr, nr = p["vref"][i], p["nref"][i]
                if vr is not None and vr not in own_vs and vr not in seen_v:
                    seen_v.add(vr); for_v.append(vr)
                if nr is not None and nr not in own_ns and nr not in seen_n:
                    seen_n.add(nr); for_n.append(nr)
    return own_v, for_v, own_n, for_n


def export(ilm_path, out_path, anm_override=None, keyframe=0):
    ilm = Ilm(open(ilm_path, "rb").read())
    order = resolve_pool(ilm)
    poses, prov = rest_poses(ilm_path, ilm.models, anm_override, keyframe)
    mtl_path = os.path.splitext(out_path)[0] + ".mtl"
    meta_path = os.path.splitext(out_path)[0] + ".ilmmeta.json"

    obj = ["# Silent Hill ILM export: %s" % os.path.basename(ilm_path),
           "# Each 'o' is a rigid animated body part - do NOT rename/add/remove them.",
           "#",
           "# Vertices are in WORLD space (the %s rest pose is baked in)."
           % (prov["anm"] or "identity"),
           "# Seam vertices are DUPLICATED on purpose. A low-threshold 'Merge by",
           "# Distance' is harmless, but an aggressive one welds them and import",
           "# will refuse the file. Parts sharing a bone (hands, head/neck) overlap",
           "# in the viewport on purpose; the game draws one at a time.",
           "# Do NOT reorder vertices or faces (Mesh > Sort Elements) and do NOT move",
           "# a face to a different material - import matches them by order.",
           "# The model is ~390 units tall: press Home, or raise the viewport clip end.",
           "mtllib %s" % os.path.basename(mtl_path), ""]
    tbase = 1
    vbase = nbase = 1
    mats_used = {}
    dangling = 0
    meta = {"source": os.path.basename(ilm_path), "drawOrder": order,
            "anm": prov["anm"], "keyframe": prov["keyframe"],
            "restPose": prov["restPose"], "models": []}

    def vec(mk):
        m, k, j = mk
        return ilm.models[m]["meshes"][k]["verts"][j]

    def nrm(mk):
        m, k, j = mk
        return ilm.models[m]["meshes"][k]["normals"][j][:3]

    for model in ilm.models:
        R, T = poses[model["idx"]]
        own_v, for_v, own_n, for_n = part_layout(ilm, model)
        vidx = {r: vbase + i for i, r in enumerate(own_v + for_v)}
        nidx = {r: nbase + i for i, r in enumerate(own_n + for_n)}

        obj.append("o %s" % model["name"])
        for r in own_v + for_v:
            oR, oT = poses[r[0]]
            x, y, z = bake(oR, oT, vec(r))
            obj.append("v %.6f %.6f %.6f" % (x, _y_out(y), z))
        for r in own_n + for_n:
            oR, _oT = poses[r[0]]
            obj.append("vn %.6f %.6f %.6f" % _normal_out(bake_dir(oR, nrm(r))))
        vbase += len(own_v) + len(for_v)
        nbase += len(own_n) + len(for_n)

        mmeta = {"name": model["name"], "vertexOffset": model["vertexOffset"],
                 "normalOffset": model["normalOffset"],
                 "restPose": {"R": list(R), "T": list(T)},
                 "ownVertexCount": len(own_v), "ownNormalCount": len(own_n),
                 "foreignVerts": [{"ownerModel": r[0], "mesh": r[1], "local": r[2]} for r in for_v],
                 "foreignNorms": [{"ownerModel": r[0], "mesh": r[1], "local": r[2]} for r in for_n],
                 "meshes": []}

        for mi, mesh in enumerate(model["meshes"]):
            # UVs are per prim-corner, so emit one vt per corner in prim order.
            for p in mesh["prims"]:
                for c in range(3 if p["tri"] else 4):
                    u, v = p["uv"][c]
                    # PSX texel centre -> OBJ [0,1], V flipped (OBJ origin is bottom-left).
                    obj.append("vt %.6f %.6f" % ((u + 0.5) / 256.0, 1.0 - (v + 0.5) / 256.0))

            t = tbase
            pmeta = []
            for p in mesh["prims"]:
                mname = mtl_name(p, ilm.materials)
                mats_used[mname] = (p["materialIdx"], clut_row(p, ilm.materials),
                                    p["isTransparent"])
                obj.append("usemtl %s" % mname)
                n = 3 if p["tri"] else 4
                face = []
                # `t + i` indexes the vt emitted for PRIM corner i above, so the
                # vt/vn/v triple stays together as the loop reorders corners.
                for i in corner_order(p):
                    vr, nr = p["vref"][i], p["nref"][i]
                    if vr is None or nr is None:
                        dangling += 1
                        vr = vr or (model["idx"], mi, 0)
                        nr = nr or (model["idx"], mi, 0)
                    face.append("%d/%d/%d" % (vidx[vr], t + i, nidx[nr]))
                obj.append("f " + " ".join(face))
                t += n
                pmeta.append({"tri": p["tri"], "raw": p["raw"].hex()})

            mmeta["meshes"].append({
                "vertexCount": mesh["vertexCount"], "normalCount": mesh["normalCount"],
                "primCount": mesh["primCount"], "unkCount_3": mesh["unkCount_3"],
                "normalCounts": [n[3] for n in mesh["normals"]],
                "prims": pmeta})
            tbase = t
        meta["models"].append(mmeta)
        obj.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(obj))
    with open(mtl_path, "w") as f:
        f.write("# CLUT palette row is encoded in the material NAME (rowNN) - keep it.\n")
        for name, (_mi, _row, alpha) in sorted(mats_used.items()):
            f.write("\nnewmtl %s\nKd 1.0 1.0 1.0\n" % name)
            if alpha:
                f.write("d 0.5\n")
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=1)

    nv = sum(m["vertexCount"] for mo in ilm.models for m in mo["meshes"])
    npr = sum(m["primCount"] for mo in ilm.models for m in mo["meshes"])
    print("export: %s -> %s" % (os.path.basename(ilm_path), os.path.basename(out_path)))
    print("   %d parts, %d vertices (+%d seam duplicates), %d prims, %d materials"
          % (len(ilm.models), nv, vbase - 1 - nv, npr, len(mats_used)))
    print("   rest pose: %s%s" % (prov["anm"] or "IDENTITY (parts sit on the origin)",
                                  "" if prov["anm"] is None else " keyframe %d" % prov["keyframe"]))
    if dangling:
        print("   WARNING: %d prim corner(s) read an unwritten pool slot" % dangling)
    print("   %s (per-prim bytes preserved for import)" % os.path.basename(meta_path))
    return out_path


# ---- import ----------------------------------------------------------------

def parse_obj(path):
    """Minimal OBJ reader: objects in file order, each with its v/vn/faces."""
    verts, norms, uvs = [], [], []
    objs = []
    cur = None
    mtl = None
    for raw in open(path):
        ln = raw.strip()
        if not ln or ln[0] == '#':
            continue
        k, _, rest = ln.partition(' ')
        if k == 'usemtl':
            mtl = rest.strip()
            continue
        if k == 'o':
            cur = {"name": rest.strip(), "faces": [], "v": [], "vn": []}
            objs.append(cur)
        elif k == 'v':
            verts.append([float(x) for x in rest.split()[:3]])
            if cur is not None:
                cur["v"].append(len(verts))
        elif k == 'vn':
            norms.append([float(x) for x in rest.split()[:3]])
            if cur is not None:
                cur["vn"].append(len(norms))
        elif k == 'vt':
            uvs.append([float(x) for x in rest.split()[:2]])
        elif k == 'f':
            if cur is None:
                cur = {"name": "", "faces": [], "v": [], "vn": []}
                objs.append(cur)
            corners = []
            for tok in rest.split():
                p = (tok.split('/') + ['', ''])[:3]
                corners.append(tuple(int(v) if v else 0 for v in p))
            cur["faces"].append({"c": corners, "mtl": mtl})
    return verts, norms, uvs, objs


def pair_faces(model, obj, materials):
    """Line each ILM primitive up with the OBJ face that carries it.

    NOT positional: Blender's OBJ exporter groups a mesh's polygons by material
    so it can emit one `usemtl` per run, which reorders faces relative to the
    prim order we wrote. Relative order WITHIN a material is preserved by both
    writers, so consuming per-material queues in prim order re-pairs them
    exactly, and works unchanged on our own interleaved output.
    """
    prims = [p for mesh in model["meshes"] for p in mesh["prims"]]
    faces = obj["faces"]
    if any(f["mtl"] is None for f in faces):
        return faces  # no material info at all: fall back to file order
    buckets = {}
    for f in faces:
        buckets.setdefault(f["mtl"], []).append(f)
    pos = dict.fromkeys(buckets, 0)
    out = []
    for p in prims:
        name = mtl_name(p, materials)
        q = buckets.get(name)
        if q is None or pos[name] >= len(q):
            raise SystemExit(
                "part %r: the OBJ has no unassigned face left with material %r. Material "
                "names encode the CLUT palette row and are how faces are matched back to "
                "primitives - reassigning or renaming materials is not supported."
                % (obj["name"], name))
        out.append(q[pos[name]])
        pos[name] += 1
    return out


def _s16(v, what):
    v = int(math.floor(v + 0.5))
    if not -32768 <= v <= 32767:
        raise SystemExit("%s out of s16 range: %d (models use roughly -1365..573)" % (what, v))
    return v


def _moved(w_obj, w0):
    """Did the artist actually touch this? w0 is model-space, w_obj is OBJ-space."""
    return max(abs(w_obj[0] - w0[0]), abs(w_obj[1] - _y_out(w0[1])),
               abs(w_obj[2] - w0[2])) > UNMOVED_EPS


def _write_vertex(data, ilm, ref, w_obj, pose, inv, orig):
    """Unbake one owned vertex back into the ILM - but only if it moved.

    An untouched vertex is left byte-identical rather than round-tripped through
    the inverse, so an unedited export/import is byte-exact BY CONSTRUCTION: the
    inverse is not on the path at all, and a later change to the keyframe,
    the float precision or the matrix code cannot silently break that gate.
    """
    R, T = pose
    if not _moved(w_obj, bake(R, T, orig)):
        return
    if inv is None:
        raise SystemExit("part rest-pose matrix is singular; cannot unbake edited geometry")
    v = unbake(inv, T, [w_obj[0], _y_in(w_obj[1]), w_obj[2]])
    m, k, j = ref
    mesh = ilm.models[m]["meshes"][k]
    struct.pack_into('<hh', data, mesh["xyP"] + j * 4,
                     _s16(v[0], "vertex X"), _s16(v[1], "vertex Y"))
    struct.pack_into('<h', data, mesh["zP"] + j * 2, _s16(v[2], "vertex Z"))


def _unit(v):
    L = math.sqrt(sum(x * x for x in v))
    return None if L < 1e-12 else [x / L for x in v]


def _write_normal(data, ilm, ref, w_obj, pose, inv, orig):
    """Unbake one owned normal - DIRECTION only, keeping the original length.

    A normal's magnitude is not artist data: the ILM stores s8 components at a
    fixed scale (|n| ~= 127) and Blender's OBJ round-trip renormalises every
    one of them to unit length. Comparing and rewriting raw components would
    therefore rewrite all 416 of HERO's normals as (0, +-1, 0) on an untouched
    round-trip, so both the unmoved test and the write work on the direction and
    re-apply the stored length.
    """
    R, _T = pose
    L = math.sqrt(sum(x * x for x in orig))
    d_obj = _unit(_normal_in(w_obj))
    d_orig = _unit(bake_dir(R, orig))
    if d_obj is None or d_orig is None or L < 1e-9:
        return
    if max(abs(d_obj[a] - d_orig[a]) for a in range(3)) <= NORMAL_UNMOVED_EPS:
        return
    if inv is None:
        return
    n = _unit(unbake_dir(inv, d_obj))
    if n is None:
        return
    m, k, j = ref
    mesh = ilm.models[m]["meshes"][k]
    for off in range(3):
        iv = max(-128, min(127, int(math.floor(n[off] * L + 0.5))))
        struct.pack_into('<b', data, mesh["normP"] + j * 4 + off, iv)


def _uv_byte(f):
    """OBJ [0,1] -> PSX u8 texel. floor(), never round(): round() banker-rounds
    (u+0.5) up for odd u and drifts the texel by one every other coordinate,
    which accumulates into a visible texture shift across round-trips.
    Clamped to 255, the PSX page limit - NOT to the texture width, since a
    128-wide TIM sits inside a 256-wide page and real UVs do reach 255."""
    b = int(f * 256.0)          # floor for the non-negative range we produce
    return 0 if b < 0 else (255 if b > 255 else b)


def import_obj(obj_path, ilm_path, out_path):
    meta_path = os.path.splitext(obj_path)[0] + ".ilmmeta.json"
    if not os.path.exists(meta_path):
        raise SystemExit("missing %s - it is written next to the OBJ by `export` and is "
                         "required (it carries the bytes whose meaning is unknown)"
                         % os.path.basename(meta_path))
    meta = json.load(open(meta_path))
    data = bytearray(open(ilm_path, "rb").read())
    ilm = Ilm(bytes(data))
    resolve_pool(ilm)
    verts, norms, uvs, objs = parse_obj(obj_path)

    # Patch in place. A tight rewrite would drop the slack the original layout
    # leaves after each array, and func_8005A900/func_8005AA08 read vertices and
    # normals THREE at a time without a bounds check - a repacked file would hand
    # them an out-of-bounds read the original never had.
    if len(objs) != len(ilm.models):
        raise SystemExit("OBJ has %d objects but the ILM has %d parts. Parts are bones - "
                         "adding or removing them breaks the rig." % (len(objs), len(ilm.models)))
    # Match BY NAME, never by position: Blender's OBJ exporter sorts objects
    # alphabetically, so positional matching dies on the first divergence.
    by_name = {}
    for o in objs:
        if o["name"] in by_name:
            raise SystemExit("OBJ has two objects called %r - part names are the bone "
                             "binding and must be unique." % o["name"])
        by_name[o["name"]] = o
    missing = [m["name"] for m in ilm.models if m["name"] not in by_name]
    extra = [n for n in by_name if n not in {m["name"] for m in ilm.models}]
    if missing or extra:
        # Blender appends .001 when a name is already taken, so re-importing the
        # OBJ to compare versions renames every object at once. That is one
        # cause with one fix, not the 46-name wall the generic message prints.
        if extra and sorted(n.rsplit(".", 1)[0] for n in extra) == sorted(missing) \
                and all(n.rsplit(".", 1)[-1].isdigit() for n in extra):
            raise SystemExit(
                "every object carries a Blender '.001'-style suffix (Blender adds one when "
                "the name is already taken in the scene). Remove the suffixes, or re-import "
                "into an empty scene - part names are the bone binding.")
        raise SystemExit(
            "part name mismatch. Missing from the OBJ: %s. Not in the ILM: %s. The first "
            "two characters of the name ARE the bone index, so renaming silently re-binds "
            "the part to bone 0." % (", ".join(missing) or "none", ", ".join(extra) or "none"))

    if len(meta.get("models", [])) != len(ilm.models):
        raise SystemExit("the .ilmmeta.json describes %d parts but the ILM has %d - it was "
                         "written for a different model." % (len(meta.get("models", [])), len(ilm.models)))

    poses = []
    for mm in meta["models"]:
        rp = mm.get("restPose") or {"R": list(IDENTITY_R), "T": [0, 0, 0]}
        poses.append((rp["R"], rp["T"]))
    invs = [mat_inverse(R) for R, _T in poses]

    def orig_vec(r):
        m, k, j = r
        return ilm.models[m]["meshes"][k]["verts"][j]

    def orig_nrm(r):
        m, k, j = r
        return ilm.models[m]["meshes"][k]["normals"][j][:3]

    # Where each owned vertex lives in the OBJ, so a seam duplicate can be
    # compared against its owner's EDITED position rather than the owner's
    # ORIGINAL. Any global transform moves duplicate and owner identically, so
    # comparing against the original reported every seam as lost on something as
    # routine as a uniform scale - training the artist to ignore the warnings.
    owner_v = {}
    for _m in ilm.models:
        _ov, _fv, _a, _b = part_layout(ilm, _m)
        _o = by_name[_m["name"]]
        for _i, _r in enumerate(_ov):
            if _i < len(_o["v"]):
                owner_v[_r] = _o["v"][_i]

    nprim = nvert = nnorm = 0
    seam_warn = []
    seam_nwarn = []
    for model in ilm.models:
        o = by_name[model["name"]]
        # Normal counts are deliberately NOT checked: Blender deduplicates `vn`
        # lines, so the count legitimately changes. Normals come back through
        # the face corners instead.
        own_v, for_v, _own_n, _for_n = part_layout(ilm, model)
        # Vertex count is checked BEFORE the face count: a 'Merge by Distance'
        # drops both, and the merge-specific message is the useful one.
        if len(o["v"]) != len(own_v) + len(for_v):
            raise SystemExit(
                "part %r has %d vertices but the ILM expects %d (%d own + %d duplicated "
                "seam vertices). The usual cause is an aggressive 'Merge by Distance' - "
                "the seam duplicates are deliberate and must not be welded."
                % (o["name"], len(o["v"]), len(own_v) + len(for_v), len(own_v), len(for_v)))
        want = sum(m["primCount"] for m in model["meshes"])
        if len(o["faces"]) != want:
            raise SystemExit("part %r has %d faces but the ILM expects %d primitives. "
                             "Adding or removing faces is not supported (counts are u8 and "
                             "the bone/animation data lives in another file)."
                             % (o["name"], len(o["faces"]), want))

        # The OBJ's face->vertex indices are the AUTHORITATIVE mapping from a
        # primitive corner to a `v` line; vertex identity is otherwise inferred
        # positionally below, which any count-preserving reorder (Blender's
        # Mesh > Sort Elements) silently scrambles. Checking the two agree also
        # catches a face reordered or reassigned within one material, which
        # pair_faces cannot see when the arity matches.
        gidx = {r: o["v"][i] for i, r in enumerate(own_v + for_v)}

        paired = pair_faces(model, o, ilm.materials)
        fi = 0
        for mesh in model["meshes"]:
            for p in mesh["prims"]:
                face = paired[fi]["c"]; fi += 1
                n = 3 if p["tri"] else 4
                if len(face) != n:
                    raise SystemExit(
                        "part %r face %d has %d corners but the primitive is a %s. "
                        "Triangulating or merging faces is not supported."
                        % (o["name"], fi, len(face), "triangle" if p["tri"] else "quad"))
                for c, pc in enumerate(corner_order(p)):
                    want_v = gidx.get(p["vref"][pc])
                    if want_v is not None and face[c][0] != want_v:
                        raise SystemExit(
                            "part %r face %d corner %d references vertex %d but the "
                            "primitive needs %d. Vertices and faces must keep the order "
                            "they were exported in - do not use Mesh > Sort Elements, and "
                            "do not move a face to a different material."
                            % (o["name"], fi, c, face[c][0], want_v))

        # Normals are looked up through the FACE CORNERS, not by position:
        # Blender's OBJ exporter deduplicates `vn` lines, so the block's normal
        # order is not preserved the way the vertex order is.
        nvec = {}
        fi = 0
        for k, mesh in enumerate(model["meshes"]):
            for p in mesh["prims"]:
                face = paired[fi]["c"]; fi += 1
                for c, pc in enumerate(corner_order(p)):
                    nr, t = p["nref"][pc], face[c][2]
                    if nr is not None and t:
                        nvec.setdefault(nr, norms[t - 1])

        for i, r in enumerate(own_v):
            w = verts[o["v"][i] - 1]
            _write_vertex(data, ilm, r, w, poses[r[0]], invs[r[0]], orig_vec(r))
            nvert += 1
        # Seam duplicates are READ-ONLY: they have no storage of their own, the
        # position belongs to the part that owns the pool slot.
        for i, r in enumerate(for_v):
            w = verts[o["v"][len(own_v) + i] - 1]
            oi = owner_v.get(r)
            if oi is None:
                continue
            ow = verts[oi - 1]
            # Warn only when the duplicate itself was edited AND now disagrees
            # with its owner. Both conditions are needed: a global transform
            # moves the duplicate but keeps it equal to the owner (nothing is
            # lost), and editing the OWNER alone leaves the duplicate stale but
            # the edit is still applied, so neither deserves an alarm.
            if (max(abs(w[a] - ow[a]) for a in range(3)) > UNMOVED_EPS
                    and _moved(w, bake(poses[r[0]][0], poses[r[0]][1], orig_vec(r)))):
                seam_warn.append((model["name"], ilm.models[r[0]]["name"]))

        for r, w in nvec.items():
            if r[0] != model["idx"]:
                # A seam normal belongs to its owner's block and has no storage
                # here. Say so instead of dropping the edit in silence.
                d_obj, d_exp = _unit(w), _unit(bake_dir(poses[r[0]][0], orig_nrm(r)))
                if d_obj and d_exp and max(abs(d_obj[a] - _normal_out(d_exp)[a])
                                           for a in range(3)) > NORMAL_UNMOVED_EPS:
                    seam_nwarn.append((model["name"], ilm.models[r[0]]["name"]))
                continue
            _write_normal(data, ilm, r, w, poses[r[0]], invs[r[0]], orig_nrm(r))
            nnorm += 1

        fi = 0
        for mesh in model["meshes"]:
            for p in mesh["prims"]:
                face = paired[fi]["c"]; fi += 1
                for c, pc in enumerate(corner_order(p)):
                    t = face[c][1]
                    if not t:
                        continue
                    u_f, v_f = uvs[t - 1]
                    struct.pack_into('<H', data, p["off"] + (0, 4, 8, 0xA)[pc],
                                     _uv_byte(u_f) | (_uv_byte(1.0 - v_f) << 8))
                nprim += 1

    data[2] = 0  # isLoaded: a 1 here makes the runtime skip pointer fix-up and crash
    with open(out_path, "wb") as f:
        f.write(bytes(data))
    print("import: %s + %s -> %s" % (os.path.basename(obj_path), os.path.basename(ilm_path),
                                     os.path.basename(out_path)))
    print("   %d parts, %d vertices, %d normals, %d prims patched"
          % (len(ilm.models), nvert, nnorm, nprim))
    if meta.get("restPose") == "identity":
        print("   rest pose: IDENTITY (this OBJ was exported without an ANM)")
    for reader in sorted(set(r for r, _ in seam_warn)):
        owners = sorted(set(ow for r, ow in seam_warn if r == reader))
        print("   part %s: %d seam vertex/vertices moved and were IGNORED (owned by %s - "
              "edit them there)" % (reader, sum(1 for r, _ in seam_warn if r == reader),
                                    ", ".join(owners)))
    for reader in sorted(set(r for r, _ in seam_nwarn)):
        owners = sorted(set(ow for r, ow in seam_nwarn if r == reader))
        print("   part %s: %d seam normal(s) edited and were IGNORED (owned by %s - "
              "edit them there)" % (reader, sum(1 for r, _ in seam_nwarn if r == reader),
                                    ", ".join(owners)))
    return out_path


def main(argv):
    ap = argparse.ArgumentParser(description="Convert Silent Hill ILM models to/from OBJ.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="ILM -> OBJ + MTL + meta")
    e.add_argument("ilm")
    e.add_argument("-o", "--out")
    e.add_argument("--anm", help="rest-pose/animation file; auto-detected from CHARA_FILE_INFOS "
                                 "and ANIM/ when omitted")
    e.add_argument("--keyframe", type=int, default=0,
                   help="keyframe supplying the bone rotations (default 0). There is no true "
                        "bind pose in the data, so every export is a posed export; the matrices "
                        "are frozen into the meta, so this is purely cosmetic.")
    i = sub.add_parser("import", help="edited OBJ + original ILM -> new ILM")
    i.add_argument("obj")
    i.add_argument("ilm", help="the ORIGINAL ILM this OBJ was exported from")
    i.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    if a.cmd == "export":
        out = a.out or (os.path.splitext(a.ilm)[0] + ".obj")
        export(a.ilm, out, a.anm, a.keyframe)
    else:
        out = a.out or (os.path.splitext(a.obj)[0] + "_new.ILM")
        import_obj(a.obj, a.ilm, out)


if __name__ == "__main__":
    main(sys.argv[1:])
