#!/usr/bin/env python3
"""ILM <-> OBJ converter for Silent Hill character/object models.

    export  ILM [-o out.obj] [--anm A.ANM] [--keyframe N]
                                    ILM -> OBJ + MTL + .ilmmeta.json
    import  OBJ  ILM [-o new.ILM]   edited OBJ + the ORIGINAL ILM -> new ILM
    import  OBJ  ILM [-o new.ILM] --grow
                                    same, but extra vertices/faces in a part
                                    become NEW geometry (full file rewrite)
    import  OBJ  ILM [-o new.ILM] --replace [--no-weld] [--weld-eps E]
                                    rebuild EVERY part's geometry from the OBJ;
                                    the ILM is only the rig + non-geometry
                                    template. Seams are re-derived by welding
                                    coincident vertices of adjacent parts onto
                                    the earlier-drawn part's pool slot.

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

# --replace seam welding radius. Half a local quantisation step: coincident seam
# vertices come out of the OBJ bit-identical (export bakes a foreign vertex with
# its OWNER's matrix, so the duplicate and the original are the same six
# decimals), so the default only has to absorb a modeller snapping by eye - it is
# not a "merge nearby geometry" dial.
#
# Deliberately tight, because the two failure modes are not symmetric: welding
# too little leaves a visible gap the modeller can see and fix by snapping the
# vertices; welding too much silently binds geometry to the WRONG BONE, which
# only shows up once that limb rotates. Measured over the shipped corpus, 0.5
# welded 4 vertices on BFLU of which ALL FOUR crossed a bone, while 0.01 keeps
# 121 of SRL's 122 real welds and crosses none.
WELD_EPS = 0.01

# How far past WELD_EPS to look purely to report a near miss. Wide enough to
# catch an eyeballed seam, never used to accept one.
NEAR_MISS_FACTOR = 100.0

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
    saw_o = False
    for raw in open(path):
        ln = raw.strip()
        if not ln or ln[0] == '#':
            continue
        k, _, rest = ln.partition(' ')
        if k == 'usemtl':
            mtl = rest.strip()
            continue
        # Milkshape 3D (and several older exporters) write groups as `g` and never
        # emit `o`; the part name is the bone binding either way. `o` wins when a
        # file carries both, because Blender writes `o NAME` plus a redundant
        # `g NAME` and treating those as two parts would double the count.
        if k == 'o' or (k == 'g' and not saw_o):
            if k == 'o' and not saw_o:
                saw_o = True
                if objs and all(not x["faces"] for x in objs):
                    objs = []  # drop `g` groups opened before the first real `o`
                    cur = None
            name = rest.strip()
            if k == 'g' and name in ('', 'default', 'off'):
                continue  # exporters emit these as "no group", not a part
            cur = {"name": name, "faces": [], "v": [], "vn": []}
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


def _pair_faces_grow(model, obj, materials):
    """pair_faces, but the per-material queues may hold MORE faces than the
    part has primitives: the unconsumed remainder (in file order) is the new
    geometry. Relative order within a material is preserved by both writers,
    so the first count(prims-with-M) faces of queue M are the originals."""
    prims = [p for mesh in model["meshes"] for p in mesh["prims"]]
    faces = obj["faces"]
    if any(f["mtl"] is None for f in faces):
        if len(faces) > len(prims):
            raise SystemExit(
                "part %r has faces without a usemtl - grow-mode needs material info to "
                "tell new faces from original ones." % obj["name"])
        return faces, []
    buckets = {}
    for f in faces:
        buckets.setdefault(f["mtl"], []).append(f)
    pos = dict.fromkeys(buckets, 0)
    out = []
    taken = set()
    for p in prims:
        name = mtl_name(p, materials)
        q = buckets.get(name)
        if q is None or pos[name] >= len(q):
            raise SystemExit(
                "part %r: the OBJ has no unassigned face left with material %r. Material "
                "names encode the CLUT palette row and are how faces are matched back to "
                "primitives - reassigning or renaming materials is not supported."
                % (obj["name"], name))
        f = q[pos[name]]
        out.append(f)
        taken.add(id(f))
        pos[name] += 1
    leftover = [f for f in faces if id(f) not in taken]
    return out, leftover


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


def import_obj(obj_path, ilm_path, out_path, grow=False, replace=False,
               weld=True, weld_eps=WELD_EPS):
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

    if replace:
        return _replace_import(obj_path, ilm_path, out_path, data, ilm, meta,
                               verts, norms, uvs, by_name, poses, invs, weld, weld_eps)

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

    if grow:
        need = False
        for model in ilm.models:
            o = by_name[model["name"]]
            _ov, _fv, _a, _b = part_layout(ilm, model)
            want = sum(m["primCount"] for m in model["meshes"])
            if len(o["v"]) > len(_ov) + len(_fv) or len(o["faces"]) > want:
                need = True
                break
        if need:
            return _grow_import(obj_path, ilm_path, out_path, data, ilm, meta,
                                verts, norms, uvs, by_name, poses, invs, owner_v)
        # Zero actual growth falls through to the patch-in-place path so that
        # `--grow` on an unedited OBJ stays byte-identical to a plain import.

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


# ---- validator twin (pc_port/src/lm_validate.c) -----------------------------

LM_POOL_SLOTS = 256
LM_VERT_SLOT_CAP = 255
LM_ARRAY_SLACK = 8
LM_SHADE_SLACK = 3
LM_MAX_PARTS = 56


def lm_validate(d, tail_slack=0, anm_bone_count=-1, skeletal=True):
    """Python twin of Lm_Validate (pc_port/src/lm_validate.c). The Stage 1 rule
    table is normative for both languages - a drift is a bug in whichever
    diverges. Grow-mode runs it at tail_slack=0: emitted files carry real
    in-file pad bytes, so they stay safe even for a loader with no malloc tail.
    Returns (0, None) on pass, else (ruleId, message); first failure wins in
    numeric rule order (every V5 site is reported before any V6 site)."""
    n = len(d)

    def u32(o):
        return struct.unpack_from('<I', d, o)[0]

    def nm(o):
        return d[o:o + 8].split(b'\0')[0].decode('ascii', 'replace')

    def extent_ok(off, cnt, es):
        return off <= n and cnt * es <= n - off

    if n < 20 or d[0] != 0x30 or d[1] != 6 or d[2] != 0:
        return 1, ("V1: not a raw LM file (magic=0x%02X ver=%d isLoaded=%d size=%u)"
                   % (d[0] if n > 0 else 0, d[1] if n > 1 else 0,
                      d[2] if n > 2 else 0, n))
    matCount, matOff = d[3], u32(4)
    modelCount, hdrsOff, orderOff = d[8], u32(12), u32(16)

    if not extent_ok(matOff, matCount, 24):
        return 2, ("V2: materials section out of file (off=0x%X len=0x%X size=0x%X)"
                   % (matOff, matCount * 24, n))
    if not extent_ok(hdrsOff, modelCount, 16):
        return 2, ("V2: modelHdrs section out of file (off=0x%X len=0x%X size=0x%X)"
                   % (hdrsOff, modelCount * 16, n))
    if not extent_ok(orderOff, modelCount, 1):
        return 2, ("V2: modelOrder section out of file (off=0x%X len=0x%X size=0x%X)"
                   % (orderOff, modelCount, n))

    def mesh_arrays(mb):
        return [("primitives", u32(mb + 4), d[mb], 20, 0),
                ("verticesXy", u32(mb + 8), d[mb + 1], 4, LM_ARRAY_SLACK),
                ("verticesZ", u32(mb + 12), d[mb + 1], 2, LM_ARRAY_SLACK),
                ("normals", u32(mb + 16), d[mb + 2], 4, LM_ARRAY_SLACK),
                ("shade", u32(mb + 20), d[mb + 3], 1, LM_SHADE_SLACK)]

    for i in range(modelCount):
        mh = hdrsOff + i * 16
        meshCount, meshOff = d[mh + 8], u32(mh + 12)
        if not extent_ok(meshOff, meshCount, 24):
            return 2, ("V2: part %s meshHdrs section out of file (off=0x%X len=0x%X size=0x%X)"
                       % (nm(mh), meshOff, meshCount * 24, n))
        for j in range(meshCount):
            for aname, off, cnt, es, _sl in mesh_arrays(meshOff + j * 24):
                if cnt == 0:
                    continue
                if not extent_ok(off, cnt, es):
                    return 2, ("V2: part %s mesh %u %s section out of file (off=0x%X len=0x%X size=0x%X)"
                               % (nm(mh), j, aname, off, cnt * es, n))
                if off & 3:
                    return 2, ("V2: part %s mesh %u %s section misaligned (off=0x%X)"
                               % (nm(mh), j, aname, off))

    if skeletal and modelCount > LM_MAX_PARTS:
        return 3, "V3: %u parts exceeds skeleton cap %d" % (modelCount, LM_MAX_PARTS)

    for i in range(modelCount):
        if d[orderOff + i] >= modelCount:
            return 4, ("V4: modelOrder[%u]=%u out of range (%u parts)"
                       % (i, d[orderOff + i], modelCount))

    vert_cap = LM_VERT_SLOT_CAP if skeletal else LM_POOL_SLOTS
    for i in range(modelCount):
        mh = hdrsOff + i * 16
        for j in range(d[mh + 8]):
            vc = d[u32(mh + 12) + j * 24 + 1]
            if d[mh + 9] + vc > vert_cap:
                return 5, ("V5: part %s mesh %u vertex window %u+%u exceeds %u"
                           % (nm(mh), j, d[mh + 9], vc, vert_cap))
    for i in range(modelCount):
        mh = hdrsOff + i * 16
        for j in range(d[mh + 8]):
            nc = d[u32(mh + 12) + j * 24 + 2]
            if d[mh + 10] + nc > LM_POOL_SLOTS:
                return 6, ("V6: part %s mesh %u normal window %u+%u exceeds %d"
                           % (nm(mh), j, d[mh + 10], nc, LM_POOL_SLOTS))
    for i in range(modelCount):
        mh = hdrsOff + i * 16
        for j in range(d[mh + 8]):
            sc = d[u32(mh + 12) + j * 24 + 3]
            if d[mh + 10] + sc > LM_POOL_SLOTS:
                return 7, ("V7: part %s mesh %u shade window %u+%u exceeds %d"
                           % (nm(mh), j, d[mh + 10], sc, LM_POOL_SLOTS))
    for i in range(modelCount):
        mh = hdrsOff + i * 16
        for j in range(d[mh + 8]):
            mb = u32(mh + 12) + j * 24
            # env mode 1 advances a slot even for a count-byte 0 (see the C
            # twin in lm_validate.c): per-normal advance is max(count, 1).
            walk = sum(d[u32(mb + 16) + k * 4 + 3] or 1 for k in range(d[mb + 2]))
            if d[mh + 10] + walk > LM_POOL_SLOTS:
                return 8, ("V8: part %s mesh %u shade walk %u+%u exceeds %d"
                           % (nm(mh), j, d[mh + 10], walk, LM_POOL_SLOTS))

    for i in range(modelCount):
        mh = hdrsOff + i * 16
        for j in range(d[mh + 8]):
            for aname, off, cnt, es, sl in mesh_arrays(u32(mh + 12) + j * 24):
                if cnt == 0 or sl == 0:
                    continue
                slack = n + tail_slack - (off + cnt * es)
                if slack < sl:
                    return 9, ("V9: part %s mesh %u %s array needs %u slack bytes, has %u"
                               % (nm(mh), j, aname, sl, slack))

    # V10 is structural (see lm_validate.h): V5-strict + V11 make a slot-255
    # quad corner impossible, and no file scan can detect one.

    if skeletal:
        vstamp = bytearray(LM_POOL_SLOTS)
        nstamp = bytearray(LM_POOL_SLOTS)
        for i in range(modelCount):
            mh = hdrsOff + d[orderOff + i] * 16
            vertOff, normOff, meshOff = d[mh + 9], d[mh + 10], u32(mh + 12)
            for j in range(d[mh + 8]):
                mb = meshOff + j * 24
                vstamp[vertOff:vertOff + d[mb + 1]] = b'\1' * d[mb + 1]
                nstamp[normOff:normOff + d[mb + 2]] = b'\1' * d[mb + 2]
            primIdx = 0
            for j in range(d[mh + 8]):
                mb = meshOff + j * 24
                primsOff = u32(mb + 4)
                for k in range(d[mb]):
                    pb = primsOff + k * 20
                    corners = 3 if d[pb + 0xF] == 0xFF else 4
                    for c in range(corners):
                        if not vstamp[d[pb + 0xC + c]]:
                            return 11, ("V11: part %s prim %u corner %u reads unwritten vertex slot %u"
                                        % (nm(mh), primIdx, c, d[pb + 0xC + c]))
                        if not nstamp[d[pb + 0x10 + c]]:
                            return 11, ("V11: part %s prim %u corner %u reads unwritten normal slot %u"
                                        % (nm(mh), primIdx, c, d[pb + 0x10 + c]))
                    primIdx += 1

    for i in range(modelCount):
        mh = hdrsOff + i * 16
        b4 = (d[mh + 11] >> 4) & 3
        if b4 != 0 and (d[mh + 9] != 0 or d[mh + 10] != 0):
            return 12, ("V12: part %s field_B_4=%u requires vertexOffset==0 && normalOffset==0 (got %u/%u)"
                        % (nm(mh), b4, d[mh + 9], d[mh + 10]))

    if anm_bone_count > 0:
        for i in range(modelCount):
            mh = hdrsOff + i * 16
            if 0x30 <= d[mh] <= 0x39 and 0x30 <= d[mh + 1] <= 0x39:
                bone = (d[mh] - 0x30) * 10 + (d[mh + 1] - 0x30)
                if bone >= anm_bone_count:
                    return 13, ("V13: part %s binds bone %d but the ANM has %d bones"
                                % (nm(mh), bone, anm_bone_count))

    return 0, None


# ---- grow-mode (import --grow) ----------------------------------------------

def _align4(nbytes):
    return (4 - nbytes % 4) % 4


def _shade_pad(cnt):
    """>= 3 zero bytes after the shade stream, landing the next section
    4-aligned (F1: func_800574D4's copy strides 4 bytes past the stream)."""
    pad = _align4(cnt)
    return pad + 4 if pad < 3 else pad


def _shade_edges(ilm, order):
    """Shade-stream bytes are vertex POOL SLOT references: replay the pool and
    resolve each byte to its writer, exactly like prim corners. Needed even
    though growing shaded parts is refused - an unshaded grown part can move a
    writer that a shaded ungrown part's stream references."""
    vpool = {}
    out = {}
    for mi in order:
        m = ilm.models[mi]
        for k, me in enumerate(m["meshes"]):
            for j in range(me["vertexCount"]):
                vpool[m["vertexOffset"] + j] = (mi, k, j)
        lst = []
        for k, me in enumerate(m["meshes"]):
            for b in range(me["unkCount_3"]):
                slot = ilm.d[me["unkP"] + b]
                lst.append((k, b, slot, vpool.get(slot)))
        if lst:
            out[mi] = lst
    return out


def _quant_new_normal(inv, w):
    """OBJ-space direction -> part-local s8 triple at the stock |n|=127 scale."""
    d = _unit(_normal_in(w))
    if d is None:
        return None
    nl = _unit(unbake_dir(inv, d))
    if nl is None:
        return None
    return tuple(max(-128, min(127, int(math.floor(v * 127 + 0.5)))) for v in nl)


def _grow_budget(ilm, grown, vO2, nO2, finals, old_size, new_size):
    print("   part              bone  verts        norms        prims        shade  window")
    ext_v = ext_n = 0
    for m in ilm.models:
        mi = m["idx"]
        vc, nc, pc, sw, dv, dn, dp = finals[mi]
        ext_v = max(ext_v, vO2[mi] + vc)
        ext_n = max(ext_n, nO2[mi] + nc)

        def col(total, delta):
            return "%d (%d+%d)" % (total, total - delta, delta) if delta else "%d" % total

        print("   %-8s%-9s %3d  %-12s %-12s %-12s %5d  v[%d,%d) n[%d,%d)"
              % (m["name"], " (grown)" if mi in grown else "", bone_of(m["name"]),
                 col(vc, dv), col(nc, dn), col(pc, dp), sw,
                 vO2[mi], vO2[mi] + vc, nO2[mi], nO2[mi] + nc))
    print("   vertex slots: extent %d / 255 (%d free)   normal slots: extent %d / 256"
          % (ext_v, 255 - ext_v, ext_n))
    print("   parts: %d / 56    file: 0x%X (was 0x%X)"
          % (ilm.modelCount, new_size, old_size))


def _read_mesh_verts(data, me):
    """Re-read one mesh's vertices out of the PATCHED template bytes.

    Not me["verts"]: that was parsed before the in-place edit pass ran, so it
    still holds the pre-edit values."""
    return [(struct.unpack_from('<h', data, me["xyP"] + j * 4)[0],
             struct.unpack_from('<h', data, me["xyP"] + j * 4 + 2)[0],
             struct.unpack_from('<h', data, me["zP"] + j * 2)[0])
            for j in range(me["vertexCount"])]


def _read_mesh_norms(data, me):
    return [tuple(struct.unpack_from('<bbbB', data, me["normP"] + j * 4))
            for j in range(me["normalCount"])]


def _emit_ilm(data, ilm, parts, vO2, nO2):
    """Serialize a whole ILM from per-part geometry, template for everything else.

    `parts` maps model index -> one dict per mesh with already-resolved bytes:
    prims (20 bytes each, pool slots baked in), verts (s16 x/y/z), norms
    (s8 x/y/z + count byte) and shade (pool slot bytes). Both rewrite paths
    (--grow, --replace) funnel through here so there is exactly one place that
    knows the file layout.

    Layout mirrors stock: header | materials | modelHdrs | reserved zero block |
    modelOrder | pad4 | per part meshHdrs + arrays, each array followed by real
    in-file pad bytes so the file passes V9 at tailSlack 0.
    """
    out = bytearray()
    out += data[0:20]
    matsP = len(out)
    out += data[ilm.matsP:ilm.matsP + 24 * ilm.matCount]
    hdrsP = len(out)
    out += bytes(16 * ilm.modelCount)
    out += data[ilm.modelHdrsP + 16 * ilm.modelCount:ilm.modelOrderP]
    orderP = len(out)
    out += data[ilm.modelOrderP:ilm.modelOrderP + ilm.modelCount]
    out += bytes(_align4(len(out)))
    struct.pack_into('<I', out, 4, matsP)
    struct.pack_into('<I', out, 0xC, hdrsP)
    struct.pack_into('<I', out, 0x10, orderP)
    out[2] = 0  # isLoaded: a 1 makes the runtime skip pointer fix-up and crash

    for m in ilm.models:
        mi = m["idx"]
        meshHdrsP2 = len(out)
        out += bytes(24 * m["meshCount"])
        mesh_info = []
        for k, blk in enumerate(parts[mi]):
            primsP = len(out)
            for raw in blk["prims"]:
                out += raw
            xyP = len(out)
            for x, y, _z in blk["verts"]:
                out += struct.pack('<hh', x, y)
            out += bytes(LM_ARRAY_SLACK)
            zP = len(out)
            for _x, _y, z in blk["verts"]:
                out += struct.pack('<h', z)
            out += bytes(_align4(len(out)) + LM_ARRAY_SLACK)
            normP = len(out)
            for sx, sy, sz, cb in blk["norms"]:
                out += struct.pack('<bbbB', sx, sy, sz, cb)
            out += bytes(LM_ARRAY_SLACK)
            cnt = len(blk["shade"])
            if cnt:
                unkP = len(out)
                out += bytes(blk["shade"])
            else:
                # Stock relation unkP = normP + 4*normalCount; with count 0 the
                # pointer is never dereferenced beyond address formation.
                unkP = normP + 4 * len(blk["norms"])
            out += bytes(_shade_pad(cnt))
            mesh_info.append((len(blk["prims"]), len(blk["verts"]), len(blk["norms"]),
                              cnt, primsP, xyP, zP, normP, unkP))
        for k, (pc2, vc2, nc2, cnt, primsP, xyP, zP, normP, unkP) in enumerate(mesh_info):
            hb = meshHdrsP2 + 24 * k
            out[hb] = pc2
            out[hb + 1] = vc2
            out[hb + 2] = nc2
            out[hb + 3] = cnt
            struct.pack_into('<IIIII', out, hb + 4, primsP, xyP, zP, normP, unkP)
        hb = hdrsP + 16 * mi
        out[hb:hb + 8] = data[m["off"]:m["off"] + 8]
        out[hb + 8] = m["meshCount"]
        out[hb + 9] = vO2[mi]
        out[hb + 10] = nO2[mi]
        out[hb + 11] = m["bits"]
        struct.pack_into('<I', out, hb + 0xC, meshHdrsP2)
    return bytes(out)


def _grow_import(obj_path, ilm_path, out_path, data, ilm, meta, verts, norms, uvs,
                 by_name, poses, invs, owner_v):
    """Full-rewrite import: original geometry keeps its bytes (modulo edits and
    renumbered pool slots), new geometry is appended, windows are re-laid-out.

    Preservation argument for the re-layout: an original sharing edge
    (writer W, local j, reader R) needs slot vO'_W+j untouched by every part
    drawn strictly between W and R. (a) W grown -> its slots are fresh and
    exclusive, nobody else ever writes them. (b) W ungrown -> the slot is the
    original vO_W+j; the original file guarantees no in-between part covered
    it, and relocated grown parts only move AWAY (a removed intermediate
    writer cannot break a last-writer relationship). (c) A part's own reads
    are always safe (it writes immediately before it reads). Pinned offset-0
    parts (field_B_4, V12) EXPAND instead of moving, which case (b) does not
    cover - hence the explicit expansion-zone conflict check below."""
    order = list(ilm.d[ilm.modelOrderP:ilm.modelOrderP + ilm.modelCount])
    if sorted(order) != list(range(ilm.modelCount)):
        raise SystemExit("--grow: modelOrder is not a permutation, so the sharing-graph "
                         "replay cannot be trusted (no stock file does this).")
    dpos = {mi: i for i, mi in enumerate(order)}
    shade_map = _shade_edges(ilm, order)

    # A dangling ref means either the traversal is wrong or this is a prop file
    # whose corners resolve against the wielder's pool - either way the sharing
    # graph cannot be rebuilt, so growth must refuse (never add a fallback).
    for m in ilm.models:
        for me in m["meshes"]:
            for p in me["prims"]:
                n = 3 if p["tri"] else 4
                if any(p["vref"][i] is None or p["nref"][i] is None for i in range(n)):
                    raise SystemExit(
                        "part %r reads a pool slot no part has written (a held-item/prop "
                        "file resolves those against the wielder's pool at draw time) - "
                        "--grow requires a fully self-contained skeletal file." % m["name"])
    for mi, lst in shade_map.items():
        if any(ref is None for _k, _b, _s, ref in lst):
            raise SystemExit("part %r shade stream reads an unwritten pool slot - "
                             "--grow requires a fully self-contained skeletal file."
                             % ilm.models[mi]["name"])

    new_verts = {}       # mi -> [(x, y, z) s16 local]
    new_norm_list = {}   # mi -> [(sx, sy, sz) s8, count byte 1]
    new_prims = {}       # mi -> [{tri, tmpl, vref, nref, uv}]
    grown = set()
    seam_warn = []
    seam_nwarn = []
    nvert = nnorm = nprim = 0

    for model in ilm.models:
        o = by_name[model["name"]]
        mi = model["idx"]
        own_v, for_v, _own_n, _for_n = part_layout(ilm, model)
        exp_v = len(own_v) + len(for_v)
        want = sum(m["primCount"] for m in model["meshes"])
        if len(o["v"]) < exp_v:
            raise SystemExit(
                "part %r has %d vertices but the ILM expects %d (%d own + %d duplicated "
                "seam vertices). The usual cause is an aggressive 'Merge by Distance' - "
                "the seam duplicates are deliberate and must not be welded."
                % (o["name"], len(o["v"]), exp_v, len(own_v), len(for_v)))
        if len(o["faces"]) < want:
            raise SystemExit("part %r has %d faces but the ILM has %d primitives - "
                             "--grow only ADDS geometry, it cannot remove faces."
                             % (o["name"], len(o["faces"]), want))
        paired, leftover = _pair_faces_grow(model, o, ilm.materials)
        extra_v = len(o["v"]) - exp_v
        if extra_v or leftover:
            grown.add(mi)
            if model["meshCount"] > 1:
                raise SystemExit("part %r has %d meshes; growth supports single-mesh "
                                 "parts only" % (o["name"], model["meshCount"]))
            if any(me["unkCount_3"] for me in model["meshes"]):
                raise SystemExit(
                    "part %r carries a shade stream (%d bytes); growing shaded parts is "
                    "not supported yet" % (o["name"],
                                           sum(me["unkCount_3"] for me in model["meshes"])))

        # Original subset: same verification and same in-place edits as the
        # plain import path (the rewrite below then copies the patched bytes).
        gidx = {r: o["v"][i] for i, r in enumerate(own_v + for_v)}
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

        nvec = {}
        fi = 0
        for k, mesh in enumerate(model["meshes"]):
            for p in mesh["prims"]:
                face = paired[fi]["c"]; fi += 1
                for c, pc in enumerate(corner_order(p)):
                    nr, t = p["nref"][pc], face[c][2]
                    if nr is not None and t:
                        nvec.setdefault(nr, norms[t - 1])

        def orig_vec(r):
            m2, k2, j2 = r
            return ilm.models[m2]["meshes"][k2]["verts"][j2]

        def orig_nrm(r):
            m2, k2, j2 = r
            return ilm.models[m2]["meshes"][k2]["normals"][j2][:3]

        for i, r in enumerate(own_v):
            w = verts[o["v"][i] - 1]
            _write_vertex(data, ilm, r, w, poses[r[0]], invs[r[0]], orig_vec(r))
            nvert += 1
        for i, r in enumerate(for_v):
            w = verts[o["v"][len(own_v) + i] - 1]
            oi = owner_v.get(r)
            if oi is None:
                continue
            ow = verts[oi - 1]
            if (max(abs(w[a] - ow[a]) for a in range(3)) > UNMOVED_EPS
                    and _moved(w, bake(poses[r[0]][0], poses[r[0]][1], orig_vec(r)))):
                seam_warn.append((model["name"], ilm.models[r[0]]["name"]))

        for r, w in nvec.items():
            if r[0] != mi:
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

        if mi not in grown:
            continue

        # New geometry.
        inv = invs[mi]
        _R, T = poses[mi]
        if inv is None:
            raise SystemExit("part %r rest-pose matrix is singular; cannot unbake new "
                             "geometry" % o["name"])
        nv = []
        for pos in range(exp_v, len(o["v"])):
            w = verts[o["v"][pos] - 1]
            v = unbake(inv, T, [w[0], _y_in(w[1]), w[2]])
            nv.append((_s16(v[0], "new vertex X"), _s16(v[1], "new vertex Y"),
                       _s16(v[2], "new vertex Z")))
        new_verts[mi] = nv

        vline_pos = {vl: i for i, vl in enumerate(o["v"])}
        legal = sorted(set(mtl_name(p, ilm.materials)
                           for me in model["meshes"] for p in me["prims"]))
        prims_flat = [p for me in model["meshes"] for p in me["prims"]]
        ndedup = {}
        nlist = []

        def new_normal(w):
            key = _quant_new_normal(inv, w)
            if key is None:
                raise SystemExit("part %r: a new face has a degenerate normal" % o["name"])
            if key not in ndedup:
                ndedup[key] = len(nlist)
                nlist.append(key)
            return ndedup[key]

        np_list = []
        for f in leftover:
            face = f["c"]
            arity = len(face)
            if arity not in (3, 4):
                raise SystemExit("part %r: a new face has %d corners; the format only has "
                                 "triangles and quads" % (o["name"], arity))
            if f["mtl"] not in legal:
                raise SystemExit(
                    "part %r: new face uses material %r; new faces must reuse one of the "
                    "part's existing materials (%s)" % (o["name"], f["mtl"], ", ".join(legal)))
            tmpl = next(p for p in prims_flat if mtl_name(p, ilm.materials) == f["mtl"])
            tri = arity == 3
            loop = TRI_LOOP if tri else QUAD_LOOP
            vref = []
            nref = []
            uv8 = []
            flat_slot = None
            for i in range(arity):
                vl, tl, nl = face[loop[i]]
                pos = vline_pos.get(vl)
                if pos is None:
                    raise SystemExit(
                        "part %r: a new face cites vertex %d outside its own 'o' block - "
                        "new cross-part seams are not supported (weld to an existing seam "
                        "duplicate instead)" % (o["name"], vl))
                if pos < len(own_v):
                    vref.append(('own', own_v[pos][2]))
                elif pos < exp_v:
                    r = for_v[pos - len(own_v)]
                    vref.append(('for', r[0], r[2]))
                else:
                    vref.append(('new', pos - exp_v))
                if not tl:
                    raise SystemExit("new face in part %r has no UVs - unwrap the new "
                                     "geometry" % o["name"])
                u_f, v_f = uvs[tl - 1]
                uv8.append((_uv_byte(u_f), _uv_byte(1.0 - v_f)))
                if nl:
                    nref.append(('newn', new_normal(norms[nl - 1])))
                else:
                    if flat_slot is None:
                        p0, p1, p2 = (verts[face[c][0] - 1] for c in range(3))
                        e1 = [p1[a] - p0[a] for a in range(3)]
                        e2 = [p2[a] - p0[a] for a in range(3)]
                        flat_slot = new_normal([e1[1] * e2[2] - e1[2] * e2[1],
                                                e1[2] * e2[0] - e1[0] * e2[2],
                                                e1[0] * e2[1] - e1[1] * e2[0]])
                    nref.append(('newn', flat_slot))
            np_list.append({"tri": tri, "tmpl": tmpl, "vref": vref, "nref": nref, "uv": uv8})
        new_prims[mi] = np_list
        new_norm_list[mi] = nlist
        nprim += len(np_list)

    # Re-layout: ungrown parts keep their windows; grown parts relocate whole
    # to fresh exclusive ranges above the original extents; pinned (field_B_4)
    # parts expand in place at offset 0 (V12: the third chain ignores offsets).
    def vc_new(mi):
        return ilm.models[mi]["meshes"][0]["vertexCount"] + len(new_verts.get(mi, []))

    def nc_new(mi):
        return ilm.models[mi]["meshes"][0]["normalCount"] + len(new_norm_list.get(mi, []))

    ext_v = max(m["vertexOffset"] + me["vertexCount"]
                for m in ilm.models for me in m["meshes"])
    ext_n = max(m["normalOffset"] + me["normalCount"]
                for m in ilm.models for me in m["meshes"])
    vO2 = {}
    nO2 = {}
    pinned = []
    for m in ilm.models:
        mi = m["idx"]
        if mi not in grown:
            vO2[mi], nO2[mi] = m["vertexOffset"], m["normalOffset"]
        elif (m["bits"] >> 4) & 3:
            if m["vertexOffset"] or m["normalOffset"]:
                raise SystemExit(
                    "part %r: field_B_4=%d with window offsets %d/%d violates V12 in the "
                    "ORIGINAL file - fix the source ILM first."
                    % (m["name"], (m["bits"] >> 4) & 3, m["vertexOffset"], m["normalOffset"]))
            vO2[mi] = nO2[mi] = 0
            pinned.append(mi)
    vbase = max([ext_v] + [vc_new(mi) for mi in pinned])
    nbase = max([ext_n] + [nc_new(mi) for mi in pinned])
    for m in ilm.models:
        mi = m["idx"]
        if mi in grown and mi not in pinned:
            vO2[mi] = vbase
            vbase += vc_new(mi)
            nO2[mi] = nbase
            nbase += nc_new(mi)

    finals = {}
    for m in ilm.models:
        mi = m["idx"]
        vc = sum(me["vertexCount"] for me in m["meshes"]) + len(new_verts.get(mi, []))
        nc = sum(me["normalCount"] for me in m["meshes"]) + len(new_norm_list.get(mi, []))
        pc = sum(me["primCount"] for me in m["meshes"]) + len(new_prims.get(mi, []))
        sw = sum(n[3] for me in m["meshes"] for n in me["normals"]) + len(new_norm_list.get(mi, []))
        finals[mi] = (vc, nc, pc, sw, len(new_verts.get(mi, [])),
                      len(new_norm_list.get(mi, [])), len(new_prims.get(mi, [])))
    for m in ilm.models:
        vc, nc, pc = finals[m["idx"]][:3]
        if pc > 255 or vc > 255 or nc > 255:
            _grow_budget(ilm, grown, vO2, nO2, finals, len(data), 0)
            raise SystemExit("part %r: grown counts %d verts / %d normals / %d prims "
                             "exceed the u8 ceiling 255" % (m["name"], vc, nc, pc))

    if vbase > 255:
        _grow_budget(ilm, grown, vO2, nO2, finals, len(data), 0)
        raise SystemExit(
            "REFUSED V5: vertex slot extent %d exceeds 255 - vertex slot 255 is reserved "
            "(0xFF in a quad's 4th corner byte is the triangle sentinel)" % vbase)
    if nbase > 256:
        _grow_budget(ilm, grown, vO2, nO2, finals, len(data), 0)
        raise SystemExit("REFUSED V6: normal slot extent %d exceeds 256" % nbase)

    # Pinned expansion conflict check (see the preservation argument above).
    if pinned:
        vedges = []
        nedges = []
        for m in ilm.models:
            rmi = m["idx"]
            for me in m["meshes"]:
                for p in me["prims"]:
                    n = 3 if p["tri"] else 4
                    for i in range(n):
                        vedges.append((p["vtx"][i], p["vref"][i][0], rmi))
                        nedges.append((p["nrm"][i], p["nref"][i][0], rmi))
            for _k, _b, slot, ref in shade_map.get(rmi, []):
                vedges.append((slot, ref[0], rmi))
        for P in pinned:
            m = ilm.models[P]
            zones = [(m["meshes"][0]["vertexCount"], vc_new(P), vedges, "vertex"),
                     (m["meshes"][0]["normalCount"], nc_new(P), nedges, "normal")]
            for lo, hi, edges, kind in zones:
                for s, w, r in edges:
                    if lo <= s < hi and dpos[w] < dpos[P] < dpos[r]:
                        raise SystemExit(
                            "part %r cannot grow at offset 0: expanding its %s window to "
                            "%d would overwrite pool slot %d between writer %s and reader "
                            "%s" % (m["name"], kind, hi, s,
                                    ilm.models[w]["name"], ilm.models[r]["name"]))

    parts = {}
    for m in ilm.models:
        mi = m["idx"]
        blocks = []
        for k, me in enumerate(m["meshes"]):
            add_v = new_verts.get(mi, []) if k == 0 else []
            add_n = new_norm_list.get(mi, []) if k == 0 else []
            add_p = new_prims.get(mi, []) if k == 0 else []
            prims = []
            for p in me["prims"]:
                raw = bytearray(data[p["off"]:p["off"] + 20])
                n = 3 if p["tri"] else 4
                for i in range(n):
                    vr, nr = p["vref"][i], p["nref"][i]
                    raw[0xC + i] = vO2[vr[0]] + vr[2]
                    raw[0x10 + i] = nO2[nr[0]] + nr[2]
                prims.append(bytes(raw))
            for npd in add_p:
                tp = npd["tmpl"]
                raw = bytearray(data[tp["off"]:tp["off"] + 20])
                arity = 3 if npd["tri"] else 4
                for i in range(arity):
                    ref = npd["vref"][i]
                    if ref[0] == 'own':
                        slot = vO2[mi] + ref[1]
                    elif ref[0] == 'for':
                        slot = vO2[ref[1]] + ref[2]
                    else:
                        slot = vO2[mi] + me["vertexCount"] + ref[1]
                    raw[0xC + i] = slot
                    raw[0x10 + i] = nO2[mi] + me["normalCount"] + npd["nref"][i][1]
                    u, v = npd["uv"][i]
                    struct.pack_into('<H', raw, (0, 4, 8, 0xA)[i], u | (v << 8))
                if npd["tri"]:
                    raw[0xC + 3] = 0xFF
                    raw[0x10 + 3] = 0  # stock convention: 6109/6109 tris carry 0
                prims.append(bytes(raw))
            # Count byte 1 on a new normal: the stock 1:1 pattern
            # (sum(count) == vertexCount); it only feeds the env-mode-1/2 walk,
            # which V8 bounds.
            blocks.append({
                "prims": prims,
                "verts": _read_mesh_verts(data, me) + list(add_v),
                "norms": _read_mesh_norms(data, me) + [(sx, sy, sz, 1) for sx, sy, sz in add_n],
                "shade": [vO2[ref[0]] + ref[2]
                          for _k2, _b, _slot, ref in shade_map.get(mi, []) if _k2 == k]})
        parts[mi] = blocks

    blob = _emit_ilm(data, ilm, parts, vO2, nO2)

    # The validator twin at strict authoring settings; V13 through the ANM.
    _ap, anm = find_anm(ilm_path, ilm.models)
    rid, msg = lm_validate(blob, tail_slack=0,
                           anm_bone_count=anm.boneCount if anm else -1, skeletal=True)
    if rid:
        _grow_budget(ilm, grown, vO2, nO2, finals, len(data), len(blob))
        raise SystemExit("REFUSED %s" % msg)

    # Sharing-graph verification on the EMITTED bytes: every original corner
    # and shade byte must resolve to the same (writer part, local index), every
    # new corner to its own part. This is the converter's own acceptance test.
    ilm2 = Ilm(blob)
    order2 = resolve_pool(ilm2)
    if order2 != order:
        raise SystemExit("grow internal error: modelOrder changed")
    foreign = kept = 0
    for m in ilm.models:
        mi = m["idx"]
        m2 = ilm2.models[mi]
        for k, me in enumerate(m["meshes"]):
            me2 = m2["meshes"][k]
            for pi, p in enumerate(me["prims"]):
                p2 = me2["prims"][pi]
                n = 3 if p["tri"] else 4
                for i in range(n):
                    for old, new in ((p["vref"][i], p2["vref"][i]),
                                     (p["nref"][i], p2["nref"][i])):
                        expect = (ilm.models[old[0]]["name"], old[2])
                        got = None if new is None else (ilm2.models[new[0]]["name"], new[2])
                        if old[0] != mi:
                            foreign += 1
                            kept += got == expect
                        if got != expect:
                            raise SystemExit(
                                "grow internal error: part %s prim %d corner %d edge "
                                "%s != %s" % (m["name"], pi, i, got, expect))
        vcold = m["meshes"][0]["vertexCount"]
        ncold = m["meshes"][0]["normalCount"]
        for j, npd in enumerate(new_prims.get(mi, [])):
            p2 = m2["meshes"][0]["prims"][m["meshes"][0]["primCount"] + j]
            arity = 3 if npd["tri"] else 4
            for i in range(arity):
                ref = npd["vref"][i]
                if ref[0] == 'own':
                    expect = (m["name"], ref[1])
                elif ref[0] == 'for':
                    expect = (ilm.models[ref[1]]["name"], ref[2])
                else:
                    expect = (m["name"], vcold + ref[1])
                new = p2["vref"][i]
                got = None if new is None else (ilm2.models[new[0]]["name"], new[2])
                if got != expect:
                    raise SystemExit("grow internal error: new prim %d corner %d vertex "
                                     "edge %s != %s" % (j, i, got, expect))
                new = p2["nref"][i]
                got = None if new is None else (ilm2.models[new[0]]["name"], new[2])
                if got != (m["name"], ncold + npd["nref"][i][1]):
                    raise SystemExit("grow internal error: new prim %d corner %d normal "
                                     "edge mismatch" % (j, i))
    shade2 = _shade_edges(ilm2, order2)
    for mi, lst in shade_map.items():
        lst2 = shade2.get(mi, [])
        if len(lst) != len(lst2):
            raise SystemExit("grow internal error: shade stream length changed")
        for (k, b, _s, ref), (k2, b2, _s2, ref2) in zip(lst, lst2):
            expect = (ilm.models[ref[0]]["name"], ref[2])
            got = None if ref2 is None else (ilm2.models[ref2[0]]["name"], ref2[2])
            if (k, b) != (k2, b2) or got != expect:
                raise SystemExit("grow internal error: part %s shade byte %d edge %s != %s"
                                 % (ilm.models[mi]["name"], b, got, expect))

    with open(out_path, "wb") as f:
        f.write(blob)
    print("grow-import: %s + %s -> %s" % (os.path.basename(obj_path),
                                          os.path.basename(ilm_path),
                                          os.path.basename(out_path)))
    for mi in sorted(grown):
        print("   part %s: +%d verts, +%d normals, +%d prims%s"
              % (ilm.models[mi]["name"], len(new_verts.get(mi, [])),
                 len(new_norm_list.get(mi, [])), len(new_prims.get(mi, [])),
                 " (pinned at offset 0: third-chain part)" if mi in pinned else ""))
    print("   %d original vertices, %d normals patched; sharing graph preserved: "
          "%d/%d foreign edges" % (nvert, nnorm, kept, foreign))
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
    _grow_budget(ilm, grown, vO2, nO2, finals, len(data), len(blob))
    return out_path


# ---- full replacement (import --replace) ------------------------------------

def _cell(p, size):
    return (int(math.floor(p[0] / size)), int(math.floor(p[1] / size)),
            int(math.floor(p[2] / size)))


def _grid_add(grid, p, size, val):
    grid.setdefault(_cell(p, size), []).append(val)


def _grid_near(grid, p, size):
    """Everything within `size` of p, via the 27 cells around it."""
    cx, cy, cz = _cell(p, size)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for v in grid.get((cx + dx, cy + dy, cz + dz), ()):
                    yield v


def _dist2(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3))


def _pool_alloc(cap, place_order, dpos, counts, life, bound, keep):
    """Assign every part a contiguous pool window no in-between part clobbers.

    Lifetimes are PER SLOT, not per part: a window is contiguous but its slots
    die at different times, which is how stock files fit 466 vertices (KAU) into
    a 90-slot pool. Slot `off+j` is busy from the writer's draw position until
    the draw position of the last part that reads it, and two occupancies of one
    slot clash when those closed intervals touch - a part that writes a slot at
    the very position it is last read still clobbers it, because vertices are
    copied into the pool before the part's own primitives draw. The coarser
    per-PART model was tried first and rejected by the corpus: it calls 7488
    windows in the 257 shipped files illegal, the per-slot model calls zero.

    First fit over that occupancy, walking parts in draw order and trying the
    template's own offset before anything else, so an unedited --replace
    reproduces the original windows exactly. `bound` raises the cap requirement
    without reserving slots (the shade stream indexes a different array off the
    same normalOffset, so V7/V8 bound it but it never occupies a normal slot).
    Returns (offsets, None) or (None, (part index, blocking part indices)).
    """
    busy = {}
    off = {}

    def clear(o, mi):
        s = dpos[mi]
        for j in range(counts[mi]):
            e = life[mi][j]
            for (s2, e2, _w) in busy.get(o + j, ()):
                if s <= e2 and s2 <= e:
                    return False
        return True

    for mi in place_order:
        need = max(counts[mi], bound.get(mi, 0))
        cands = []
        if keep.get(mi) is not None and keep[mi] + need <= cap:
            cands.append(keep[mi])
        cands.extend(range(0, cap - need + 1))
        for o in cands:
            if clear(o, mi):
                off[mi] = o
                break
        else:
            # Blockers are the parts whose slots stay live anywhere across this
            # part's OWN live span, not just at its draw position - a window has
            # to be free for its whole span, which is what made the search fail.
            span = (dpos[mi], max(life[mi]) if counts[mi] else dpos[mi])
            live = sorted(set(w for lst in busy.values() for (s2, e2, w) in lst
                              if span[0] <= e2 and s2 <= span[1]))
            return None, (mi, live)
        for j in range(counts[mi]):
            busy.setdefault(off[mi] + j, []).append((dpos[mi], life[mi][j], mi))
    return off, None


def _replace_import(obj_path, ilm_path, out_path, data, ilm, meta, verts, norms, uvs,
                    by_name, poses, invs, weld, weld_eps):
    """Rebuild every part's geometry from the OBJ, keeping the template's rig.

    Unlike --grow this does not need the original vertices or faces to survive:
    counts, topology and material assignment all come from the OBJ. What still
    comes from the template ILM is everything whose meaning is unknown or
    runtime-owned - the 24-byte material blocks, modelOrder, ModelHeader byte
    0xB, the align4(modelCount) reserved block, and each primitive's non-index
    bytes (clut, flags, the runtime's tpage low byte, materialIdx 127) taken
    from a primitive of the SAME material name.

    Seams are re-derived rather than declared: coincident vertices in different
    parts collapse onto the earliest-drawn part's pool slot, which is the only
    welding mechanism the format has (no skinning, no weights - a corner that
    reads a foreign slot simply follows that part's bone).
    """
    order = list(ilm.d[ilm.modelOrderP:ilm.modelOrderP + ilm.modelCount])
    if sorted(order) != list(range(ilm.modelCount)):
        raise SystemExit("--replace: modelOrder is not a permutation, so the draw order "
                         "that decides seam ownership cannot be trusted (no stock file "
                         "does this).")
    if weld and meta.get("restPose") == "identity":
        # Without a rest pose every part sits on the origin, so "coincident"
        # stops meaning "meets at a joint" and welds the model to itself.
        raise SystemExit(
            "this OBJ was exported without a rest pose, so every part is piled on the "
            "origin and coincidence no longer means adjacency - auto-weld would fuse "
            "unrelated parts. Re-export with --anm PATH, or pass --no-weld.")
    dpos = {mi: i for i, mi in enumerate(order)}
    names = {m["idx"]: m["name"] for m in ilm.models}

    for m in ilm.models:
        if m["meshCount"] != 1:
            raise SystemExit("part %r has %d meshes; --replace rebuilds one mesh per part "
                             "and cannot guess how to split the OBJ across them."
                             % (m["name"], m["meshCount"]))
        for me in m["meshes"]:
            for p in me["prims"]:
                n = 3 if p["tri"] else 4
                if any(p["vref"][i] is None or p["nref"][i] is None for i in range(n)):
                    raise SystemExit(
                        "part %r reads a pool slot no part has written (a held-item/prop "
                        "file resolves those against the wielder's pool at draw time) - "
                        "--replace requires a fully self-contained skeletal file."
                        % m["name"])
    shade_map = _shade_edges(ilm, order)
    for mi, lst in shade_map.items():
        if any(ref is None for _k, _b, _s, ref in lst):
            raise SystemExit("part %r shade stream reads an unwritten pool slot - "
                             "--replace requires a fully self-contained skeletal file."
                             % names[mi])

    # Ownership of a vertex line is fixed by the 'o' block that declares it; the
    # weld below only decides whose POOL SLOT it ends up in.
    line_part = {}
    for m in ilm.models:
        for vl in by_name[m["name"]]["v"]:
            line_part[vl] = m["idx"]

    # Weld, walking parts in draw order against a grid of the vertices earlier
    # parts have already claimed. Owners are therefore always drawn earlier by
    # construction - the direction the mechanic forces (verified over 20 stock
    # files: all 2969 cross-part corner references point backwards in
    # modelOrder, zero exceptions). Each part is added to the grid only after
    # its own lines are resolved, so nothing ever welds to itself.
    redirect = {}
    owned = {}
    weld_pairs = {}
    grid = {}
    crossers = {}
    near_miss = [None]  # closest squared distance that fell OUTSIDE weld_eps
    # One cell size for both add and query, or the keys do not match and nothing
    # is ever found. Sized for the near-miss radius, the wider of the two.
    grid_size = weld_eps * NEAR_MISS_FACTOR
    for mi in order:
        mine = []
        for vl in by_name[names[mi]]["v"]:
            w = verts[vl - 1]
            best = bestd = None
            if weld:
                # Search a wider radius than we accept, purely so a modeller who
                # placed a seam by eye gets told to raise --weld-eps instead of
                # silently shipping a gap that only opens when the limb rotates.
                for cand in _grid_near(grid, w, grid_size):
                    d = _dist2(w, verts[cand - 1])
                    if d <= weld_eps * weld_eps:
                        if bestd is None or d < bestd:
                            best, bestd = cand, d
                    elif near_miss[0] is None or d < near_miss[0]:
                        near_miss[0] = d
            if best is None:
                mine.append(vl)
            else:
                redirect[vl] = best
                key = (names[mi], names[line_part[best]])
                weld_pairs[key] = weld_pairs.get(key, 0) + 1
                # A weld that both MOVES a vertex and hands it to a different
                # BONE is the only kind that can change how the model animates;
                # between parts sharing a bone the matrix is the same one, and a
                # zero-distance weld does not move anything. Worth naming.
                if (bestd > 0 and bone_of(names[mi]) != bone_of(names[line_part[best]])):
                    c = crossers.setdefault(key, [0, 0.0])
                    c[0] += 1
                    c[1] = max(c[1], math.sqrt(bestd))
        owned[mi] = mine
        for vl in mine:
            _grid_add(grid, verts[vl - 1], grid_size, vl)

    vlocal = {}
    for mi in order:
        for j, vl in enumerate(owned[mi]):
            vlocal[vl] = (mi, j)

    def resolve_v(vl, part_name):
        r = redirect.get(vl, vl)
        if r not in vlocal:
            raise SystemExit("part %r cites vertex %d, which no 'o' block declares."
                             % (part_name, vl))
        return vlocal[r]

    # Vertex bytes. An unmoved vertex keeps the template's exact s16 triple
    # rather than being round-tripped through the inverse matrix, the same
    # contract the patch-in-place path gives - so an unedited --replace carries
    # the original geometry bytes, not a re-quantisation of them.
    #
    # orig2new follows the weld, and has to: a shade stream names the template's
    # vertices, and a vertex of its own part can perfectly well end up owned by
    # a neighbour (CKN.ILM does exactly that).
    new_verts = {mi: [] for mi in order}
    orig2new = {mi: {} for mi in order}
    for mi in order:
        R, T = poses[mi]
        inv = invs[mi]
        me = ilm.models[mi]["meshes"][0]
        og = {}
        for j, ov in enumerate(me["verts"]):
            b = bake(R, T, ov)
            _grid_add(og, (b[0], _y_out(b[1]), b[2]), UNMOVED_EPS, j)
        for vl in by_name[names[mi]]["v"]:
            w = verts[vl - 1]
            hit = None
            for cand in _grid_near(og, w, UNMOVED_EPS):
                if not _moved(w, bake(R, T, me["verts"][cand])):
                    hit = cand
                    break
            if vl in vlocal:
                if hit is not None:
                    new_verts[mi].append(me["verts"][hit])
                else:
                    if inv is None:
                        raise SystemExit("part %r rest-pose matrix is singular; cannot "
                                         "unbake replacement geometry" % names[mi])
                    v = unbake(inv, T, [w[0], _y_in(w[1]), w[2]])
                    new_verts[mi].append((_s16(v[0], "vertex X"), _s16(v[1], "vertex Y"),
                                          _s16(v[2], "vertex Z")))
            if hit is not None:
                orig2new[mi].setdefault(hit, vlocal[redirect.get(vl, vl)])

    # Normal directions of the template, in OBJ space, for the unmoved test.
    orig_dirs = {}
    for mi in order:
        R, _T = poses[mi]
        orig_dirs[mi] = [_unit(_normal_out(bake_dir(R, n[:3])))
                         for n in ilm.models[mi]["meshes"][0]["normals"]]

    used_orig = {mi: {} for mi in order}   # original normal index -> first use rank
    fresh = {mi: {} for mi in order}       # quantised triple -> first use rank
    corner_norm = {}                       # (part idx, face idx, corner) -> key

    def normal_key(owner, w_obj):
        """Normal ownership follows VERTEX ownership - verified exactly true of
        the corpus: all 260215 corners in the 257 shipped files have the same
        owner for their vertex slot and their normal slot."""
        d = _unit(w_obj)
        if d is None:
            raise SystemExit("part %r: a face has a degenerate normal" % names[owner])
        hit = hitd = None
        for j, od in enumerate(orig_dirs[owner]):
            if od is None:
                continue
            e = max(abs(d[a] - od[a]) for a in range(3))
            if e <= NORMAL_UNMOVED_EPS and (hitd is None or e < hitd):
                hit, hitd = j, e
        if hit is not None:
            used_orig[owner].setdefault(hit, len(used_orig[owner]) + len(fresh[owner]))
            return ('o', hit)
        q = _quant_new_normal(invs[owner], w_obj)
        if q is None:
            raise SystemExit("part %r: a face has a degenerate normal" % names[owner])
        fresh[owner].setdefault(q, len(used_orig[owner]) + len(fresh[owner]))
        return ('n', q)

    # Faces -> primitive plans. Corner order is the FT4 strip/loop involution,
    # exactly as the other two import paths use it.
    plans = {}
    for mi in order:
        o = by_name[names[mi]]
        plan = []
        for fidx, f in enumerate(o["faces"]):
            face = f["c"]
            arity = len(face)
            if arity not in (3, 4):
                raise SystemExit("part %r face %d has %d corners; the format only has "
                                 "triangles and quads - triangulate or quadrangulate the "
                                 "mesh first." % (names[mi], fidx + 1, arity))
            loop = TRI_LOOP if arity == 3 else QUAD_LOOP
            corners = []
            flat = None
            for i in range(arity):
                vl, tl, nl = face[loop[i]]
                own, loc = resolve_v(vl, names[mi])
                if dpos[own] > dpos[mi]:
                    raise SystemExit(
                        "part %r face %d welds to part %r, which draws LATER (position %d "
                        "vs %d). A pool slot only survives forwards, so the owner must be "
                        "the earlier-drawn part - move the geometry, or pass --no-weld."
                        % (names[mi], fidx + 1, names[own], dpos[own], dpos[mi]))
                if not tl:
                    raise SystemExit("part %r face %d has no UVs - unwrap the geometry "
                                     "(every primitive corner carries a texel)."
                                     % (names[mi], fidx + 1))
                if nl:
                    nvec = norms[nl - 1]
                else:
                    if flat is None:
                        p0, p1, p2 = (verts[face[c][0] - 1] for c in range(3))
                        e1 = [p1[a] - p0[a] for a in range(3)]
                        e2 = [p2[a] - p0[a] for a in range(3)]
                        flat = [e1[1] * e2[2] - e1[2] * e2[1],
                                e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]]
                    nvec = flat
                u_f, v_f = uvs[tl - 1]
                corners.append((own, loc, normal_key(own, nvec),
                                (_uv_byte(u_f), _uv_byte(1.0 - v_f))))
            plan.append({"tri": arity == 3, "mtl": f["mtl"], "corners": corners})
        plans[mi] = plan

    nlocal = {}
    new_norms = {}
    for mi in order:
        me = ilm.models[mi]["meshes"][0]
        # Matched originals first, in the template's own order, so an unedited
        # --replace reproduces the normal array instead of permuting it.
        idx = {}
        lst = []
        for j in sorted(used_orig[mi]):
            idx[('o', j)] = len(lst)
            lst.append(me["normals"][j])
        for q, _rank in sorted(fresh[mi].items(), key=lambda kv: kv[1]):
            idx[('n', q)] = len(lst)
            lst.append((q[0], q[1], q[2], 1))
        nlocal[mi] = idx
        new_norms[mi] = lst

    # Slot lifetimes: a slot lives from its writer's draw position to the last
    # position that reads it, counting the shade stream as a reader.
    vlife = {mi: [dpos[mi]] * len(new_verts[mi]) for mi in order}
    nlife = {mi: [dpos[mi]] * len(new_norms[mi]) for mi in order}
    for mi in order:
        for pl in plans[mi]:
            for own, loc, nk, _uv in pl["corners"]:
                vlife[own][loc] = max(vlife[own][loc], dpos[mi])
                nl = nlocal[own][nk]
                nlife[own][nl] = max(nlife[own][nl], dpos[mi])
    new_shade = {}
    for mi in order:
        refs = []
        for _k, b, _s, ref in shade_map.get(mi, []):
            hit = orig2new[ref[0]].get(ref[2])
            if hit is None:
                raise SystemExit(
                    "part %r has a shade stream whose byte %d points at a vertex of part "
                    "%r that the replacement removed. Shade streams are per-vertex and "
                    "cannot be re-derived - keep that vertex, or replace a model without "
                    "one." % (names[mi], b, names[ref[0]]))
            refs.append(hit)
            vlife[hit[0]][hit[1]] = max(vlife[hit[0]][hit[1]], dpos[mi])
        new_shade[mi] = refs

    for mi in order:
        for what, n in (("vertices", len(new_verts[mi])), ("normals", len(new_norms[mi])),
                        ("primitives", len(plans[mi]))):
            if n > 255:
                raise SystemExit("part %r has %d %s; the count is a u8 and the ceiling is "
                                 "255. Split the detail across parts, or decimate."
                                 % (names[mi], n, what))

    vcnt = {mi: len(new_verts[mi]) for mi in order}
    ncnt = {mi: len(new_norms[mi]) for mi in order}
    nbound = {mi: max(ncnt[mi], len(new_shade[mi]),
                      sum(n[3] or 1 for n in new_norms[mi])) for mi in order}
    keep_v, keep_n = {}, {}
    for m in ilm.models:
        # V12: the third draw chain ignores window offsets, so those parts are
        # pinned to 0 and may not be relocated.
        if (m["bits"] >> 4) & 3:
            if m["vertexOffset"] or m["normalOffset"]:
                raise SystemExit(
                    "part %r: field_B_4=%d with window offsets %d/%d violates V12 in the "
                    "ORIGINAL file - fix the source ILM first."
                    % (m["name"], (m["bits"] >> 4) & 3, m["vertexOffset"], m["normalOffset"]))
            keep_v[m["idx"]] = keep_n[m["idx"]] = 0
        else:
            keep_v[m["idx"]] = m["vertexOffset"]
            keep_n[m["idx"]] = m["normalOffset"]
    pinned = set(mi for mi in order if (ilm.models[mi]["bits"] >> 4) & 3)

    def allocate(cap, counts, life, bound, keep, kind):
        big = sorted(order, key=lambda mi: -counts[mi])
        for tag, po, kp in (("template windows", order, keep),
                            ("first fit", order, {}),
                            ("largest first", big, {})):
            if pinned and po is not order:
                continue  # relocating a pinned part is not an option to try
            off, fail = _pool_alloc(cap, po, dpos, counts, life, bound, kp)
            if off is not None:
                return off, tag
        mi, live = fail
        hogs = sorted(live, key=lambda w: -counts[w])[:6]
        raise SystemExit(
            "REFUSED: no %s window fits part %r (%d slots, cap %d). The pool is held "
            "across its draw position by %s%s. Either shed geometry in those parts, or "
            "shorten the welds that keep their slots alive - a slot cannot be reused "
            "until the last part that reads it has drawn."
            % (kind, names[mi], max(counts[mi], bound.get(mi, 0)), cap,
               ", ".join("%s (%d)" % (names[w], counts[w]) for w in hogs) or "nothing",
               " and %d more" % (len(live) - len(hogs)) if len(live) > len(hogs) else ""))

    vO2, vtag = allocate(LM_VERT_SLOT_CAP, vcnt, vlife, {}, keep_v, "vertex")
    nO2, ntag = allocate(LM_POOL_SLOTS, ncnt, nlife, nbound, keep_n, "normal")

    # Primitive bytes. Every prim inherits a template of the SAME material name,
    # consumed in order so an unedited --replace reuses each original's own
    # bytes; the surplus of a grown material falls back to that material's first.
    parts = {}
    intent = {}
    for mi in order:
        me = ilm.models[mi]["meshes"][0]
        buckets = {}
        for p in me["prims"]:
            buckets.setdefault(mtl_name(p, ilm.materials), []).append(p)
        legal = sorted(buckets)
        pos = dict.fromkeys(buckets, 0)
        prims = []
        for fidx, pl in enumerate(plans[mi]):
            name = pl["mtl"]
            if name is None:
                if len(legal) != 1:
                    raise SystemExit(
                        "part %r face %d has no usemtl and the part uses %d materials "
                        "(%s). Material names encode the CLUT palette row, so a face "
                        "without one cannot be placed." % (names[mi], fidx + 1,
                                                           len(legal), ", ".join(legal)))
                name = legal[0]
            if name not in buckets:
                raise SystemExit(
                    "part %r face %d uses material %r; a replacement may only use "
                    "materials the part already had (%s) - the name encodes the CLUT "
                    "palette row and there is nowhere to invent one."
                    % (names[mi], fidx + 1, name, ", ".join(legal) or "none"))
            tp = buckets[name][min(pos[name], len(buckets[name]) - 1)]
            pos[name] += 1
            raw = bytearray(data[tp["off"]:tp["off"] + 20])
            for i, (own, loc, nk, uv) in enumerate(pl["corners"]):
                raw[0xC + i] = vO2[own] + loc
                raw[0x10 + i] = nO2[own] + nlocal[own][nk]
                struct.pack_into('<H', raw, (0, 4, 8, 0xA)[i], uv[0] | (uv[1] << 8))
                intent[(mi, fidx, i)] = ((own, 0, loc), (own, 0, nlocal[own][nk]))
            if pl["tri"]:
                raw[0xC + 3] = TRI_SENTINEL
                raw[0x10 + 3] = 0  # stock convention: 6109/6109 tris carry 0
            prims.append(bytes(raw))
        parts[mi] = [{"prims": prims, "verts": new_verts[mi], "norms": new_norms[mi],
                      "shade": [vO2[w] + loc for w, loc in new_shade[mi]]}]

    blob = _emit_ilm(data, ilm, parts, vO2, nO2)

    _ap, anm = find_anm(ilm_path, ilm.models)
    rid, msg = lm_validate(blob, tail_slack=0,
                           anm_bone_count=anm.boneCount if anm else -1, skeletal=True)
    if rid:
        raise SystemExit("REFUSED %s" % msg)

    # Acceptance test on the EMITTED bytes: resolve_pool replays the pool in
    # modelOrder, so p["vref"] is literally what the slot HOLDS at the moment
    # that primitive draws. Any weld broken by an intervening writer shows up
    # here - and nowhere else, since a static render cannot see it.
    ilm2 = Ilm(blob)
    resolve_pool(ilm2)
    for mi in order:
        for fidx, p2 in enumerate(ilm2.models[mi]["meshes"][0]["prims"]):
            n = 3 if p2["tri"] else 4
            for i in range(n):
                want_v, want_n = intent[(mi, fidx, i)]
                if p2["vref"][i] != want_v or p2["nref"][i] != want_n:
                    raise SystemExit(
                        "replace internal error: part %s prim %d corner %d reads %s/%s but "
                        "was built for %s/%s - the window layout clobbers a weld."
                        % (names[mi], fidx, i, p2["vref"][i], p2["nref"][i], want_v, want_n))

    with open(out_path, "wb") as f:
        f.write(blob)

    print("replace-import: %s + %s -> %s"
          % (os.path.basename(obj_path), os.path.basename(ilm_path),
             os.path.basename(out_path)))
    print("   %d parts, %d vertices, %d normals, %d prims rebuilt"
          % (ilm.modelCount, sum(vcnt.values()), sum(ncnt.values()),
             sum(len(p) for p in plans.values())))
    if weld:
        tot = sum(weld_pairs.values())
        print("   welded %d vertices across %d part pairs (eps %g):"
              % (tot, len(weld_pairs), weld_eps))
        for (r, ow), c in sorted(weld_pairs.items()):
            print("      %-10s reads %-10s  %d" % (r, ow, c))
        if not weld_pairs:
            print("      (none - no coincident vertices in different parts)")
        if near_miss[0] is not None:
            d = math.sqrt(near_miss[0])
            print("   HINT: the closest UNWELDED pair of vertices in different parts is "
                  "%.3f units apart, just outside --weld-eps %g. If those were meant to "
                  "be one seam, snap them together in your modeller (exact is best) or "
                  "re-run with --weld-eps %.3f." % (d, weld_eps, d * 1.01))
        for (r, ow), (c, d) in sorted(crossers.items()):
            print("   NOTE: %d vertex/vertices of %s moved up to %.3f units onto %s, which "
                  "is a DIFFERENT bone - they will now animate with %s. Lower --weld-eps "
                  "if that is not what you meant." % (c, r, d, ow, ow))
    else:
        print("   welding DISABLED: every part carries its own seam vertices, so the "
              "joints only stay closed while the parts overlap")
    print("   windows: vertex extent %d / %d (%s), normal extent %d / %d (%s)"
          % (max(vO2[mi] + vcnt[mi] for mi in order), LM_VERT_SLOT_CAP, vtag,
             max(nO2[mi] + ncnt[mi] for mi in order), LM_POOL_SLOTS, ntag))
    print("   file: 0x%X (was 0x%X)" % (len(blob), len(data)))
    if meta.get("restPose") == "identity":
        print("   rest pose: IDENTITY (this OBJ was exported without an ANM)")
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
    i.add_argument("--grow", action="store_true",
                   help="allow extra vertices/faces per part (full rewrite within the "
                        "u8 pool limits; without this flag any topology change refuses)")
    i.add_argument("--replace", action="store_true",
                   help="rebuild ALL geometry from the OBJ - the original vertices and "
                        "faces need not survive. The ILM is still needed as the template "
                        "for the rig and for every non-geometry byte. Part count and "
                        "part names may not change: parts ARE bones.")
    i.add_argument("--no-weld", dest="weld", action="store_false",
                   help="--replace only: do not collapse coincident vertices of adjacent "
                        "parts onto one pool slot. Joints then stay closed only while the "
                        "parts' geometry overlaps.")
    i.add_argument("--weld-eps", type=float, default=WELD_EPS,
                   help="--replace only: seam welding radius in model units (default %g, "
                        "half a local quantisation step)" % WELD_EPS)
    a = ap.parse_args(argv)
    if a.cmd == "export":
        out = a.out or (os.path.splitext(a.ilm)[0] + ".obj")
        export(a.ilm, out, a.anm, a.keyframe)
    else:
        if a.grow and a.replace:
            raise SystemExit("--grow and --replace are alternatives: --grow keeps the "
                             "original geometry and adds to it, --replace rebuilds it.")
        out = a.out or (os.path.splitext(a.obj)[0] + "_new.ILM")
        import_obj(a.obj, a.ilm, out, a.grow, a.replace, a.weld, a.weld_eps)


if __name__ == "__main__":
    main(sys.argv[1:])
