#!/usr/bin/env python3
"""ILM <-> OBJ converter for Silent Hill character/object models.

    export  ILM [-o out.obj]        ILM -> OBJ + MTL + .ilmmeta.json
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
    the 4th UV garbage. A QUAD is two triangles in PSX FT4 winding.
  * UVs are u8 per axis packed as u16, U in the low byte, V in the high byte.
  * Pointers in the file are file-relative offsets.
"""

import argparse
import json
import os
import struct
import sys

TRI_SENTINEL = 0xFF

# PSX Y grows downward; OBJ/Blender expect Y up, so Y is negated in both
# directions. Values are small, so the s16 range is never at risk.
def _y_out(v): return -v
def _y_in(v):  return -v


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


# ---- export -----------------------------------------------------------------

def export(ilm_path, out_path):
    ilm = Ilm(open(ilm_path, "rb").read())
    order = resolve_pool(ilm)
    # OBJ vertex/normal numbering: parts are emitted in file order, each mesh
    # contiguously, so (model, mesh, local) -> a stable 1-based OBJ index.
    vidx, nidx = {}, {}
    vn = nn = 1
    for model in ilm.models:
        for k, mesh in enumerate(model["meshes"]):
            for j in range(mesh["vertexCount"]):
                vidx[(model["idx"], k, j)] = vn; vn += 1
            for j in range(mesh["normalCount"]):
                nidx[(model["idx"], k, j)] = nn; nn += 1
    mtl_path = os.path.splitext(out_path)[0] + ".mtl"
    meta_path = os.path.splitext(out_path)[0] + ".ilmmeta.json"

    obj = ["# Silent Hill ILM export: %s" % os.path.basename(ilm_path),
           "# Each 'o' is a rigid animated body part - do NOT rename/add/remove them.",
           "mtllib %s" % os.path.basename(mtl_path), ""]
    tbase = 1
    mats_used = {}
    dangling = 0
    meta = {"source": os.path.basename(ilm_path), "drawOrder": order, "models": []}

    for model in ilm.models:
        obj.append("o %s" % model["name"])
        mmeta = {"name": model["name"], "vertexOffset": model["vertexOffset"],
                 "normalOffset": model["normalOffset"], "meshes": []}
        for mi, mesh in enumerate(model["meshes"]):
            for (x, y, z) in mesh["verts"]:
                obj.append("v %d %d %d" % (x, _y_out(y), z))
            for (nx, ny, nz, _c) in mesh["normals"]:
                obj.append("vn %d %d %d" % (nx, _y_out(ny), nz))
            # UVs are per prim-corner, so emit one vt per corner in prim order.
            for p in mesh["prims"]:
                for c in range(3 if p["tri"] else 4):
                    u, v = p["uv"][c]
                    # PSX texel centre -> OBJ [0,1], V flipped (OBJ origin is bottom-left).
                    obj.append("vt %.6f %.6f" % ((u + 0.5) / 256.0, 1.0 - (v + 0.5) / 256.0))

            t = tbase
            pmeta = []
            for p in mesh["prims"]:
                row = clut_row(p, ilm.materials)
                mname = "mat%02d_row%02d%s" % (p["materialIdx"], row,
                                               "_alpha" if p["isTransparent"] else "")
                mats_used[mname] = (p["materialIdx"], row, p["isTransparent"])
                obj.append("usemtl %s" % mname)
                n = 3 if p["tri"] else 4
                face = []
                for i in range(n):
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
    print("   %d parts, %d vertices, %d prims, %d materials"
          % (len(ilm.models), nv, npr, len(mats_used)))
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
    for raw in open(path):
        ln = raw.strip()
        if not ln or ln[0] == '#':
            continue
        k, _, rest = ln.partition(' ')
        if k == 'o':
            cur = {"name": rest.strip(), "faces": []}
            objs.append(cur)
        elif k == 'v':
            verts.append([float(x) for x in rest.split()[:3]])
        elif k == 'vn':
            norms.append([float(x) for x in rest.split()[:3]])
        elif k == 'vt':
            uvs.append([float(x) for x in rest.split()[:2]])
        elif k == 'f':
            if cur is None:
                cur = {"name": "", "faces": []}
                objs.append(cur)
            corners = []
            for tok in rest.split():
                p = (tok.split('/') + ['', ''])[:3]
                corners.append(tuple(int(v) if v else 0 for v in p))
            cur["faces"].append(corners)
    return verts, norms, uvs, objs


def _s16(v, what):
    v = int(round(v))
    if not -32768 <= v <= 32767:
        raise SystemExit("%s out of s16 range: %d (models use roughly -1365..573)" % (what, v))
    return v


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
    verts, norms, uvs, objs = parse_obj(obj_path)

    # Patch in place. A tight rewrite would drop the slack the original layout
    # leaves after each array, and func_8005A900/func_8005AA08 read vertices and
    # normals THREE at a time without a bounds check - a repacked file would hand
    # them an out-of-bounds read the original never had.
    if len(objs) != len(ilm.models):
        raise SystemExit("OBJ has %d objects but the ILM has %d parts. Parts are bones - "
                         "adding or removing them breaks the rig." % (len(objs), len(ilm.models)))
    for o, model in zip(objs, ilm.models):
        if o["name"] != model["name"]:
            raise SystemExit(
                "part name mismatch: OBJ has %r where the ILM has %r. The first two "
                "characters of the name ARE the bone index, so renaming silently re-binds "
                "the part to bone 0." % (o["name"], model["name"]))

    vi = ni = 0
    nprim = nvert = nnorm = 0
    for model, o in zip(ilm.models, objs):
        want = sum(m["primCount"] for m in model["meshes"])
        if len(o["faces"]) != want:
            raise SystemExit("part %r has %d faces but the ILM expects %d primitives. "
                             "Adding or removing faces is not supported (counts are u8 and "
                             "the bone/animation data lives in another file)."
                             % (o["name"], len(o["faces"]), want))
        fi = 0
        for mesh in model["meshes"]:
            for j in range(mesh["vertexCount"]):
                x, y, z = verts[vi]; vi += 1
                struct.pack_into('<hh', data, mesh["xyP"] + j * 4,
                                 _s16(x, "vertex X"), _s16(_y_in(y), "vertex Y"))
                struct.pack_into('<h', data, mesh["zP"] + j * 2, _s16(z, "vertex Z"))
                nvert += 1
            for j in range(mesh["normalCount"]):
                nx, ny, nz = norms[ni]; ni += 1
                for off, v in ((0, nx), (1, _y_in(ny)), (2, nz)):
                    iv = max(-128, min(127, int(round(v))))
                    struct.pack_into('<b', data, mesh["normP"] + j * 4 + off, iv)
                nnorm += 1
            for p in mesh["prims"]:
                face = o["faces"][fi]; fi += 1
                n = 3 if p["tri"] else 4
                if len(face) != n:
                    raise SystemExit(
                        "part %r face %d has %d corners but the primitive is a %s. "
                        "Triangulating or merging faces is not supported."
                        % (o["name"], fi, len(face), "triangle" if p["tri"] else "quad"))
                for c in range(n):
                    t = face[c][1]
                    if not t:
                        continue
                    u_f, v_f = uvs[t - 1]
                    struct.pack_into('<H', data, p["off"] + (0, 4, 8, 0xA)[c],
                                     _uv_byte(u_f) | (_uv_byte(1.0 - v_f) << 8))
                nprim += 1

    data[2] = 0  # isLoaded: a 1 here makes the runtime skip pointer fix-up and crash
    with open(out_path, "wb") as f:
        f.write(bytes(data))
    print("import: %s + %s -> %s" % (os.path.basename(obj_path), os.path.basename(ilm_path),
                                     os.path.basename(out_path)))
    print("   %d parts, %d vertices, %d normals, %d prims patched"
          % (len(ilm.models), nvert, nnorm, nprim))
    return out_path


def main(argv):
    ap = argparse.ArgumentParser(description="Convert Silent Hill ILM models to/from OBJ.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="ILM -> OBJ + MTL + meta")
    e.add_argument("ilm")
    e.add_argument("-o", "--out")
    i = sub.add_parser("import", help="edited OBJ + original ILM -> new ILM")
    i.add_argument("obj")
    i.add_argument("ilm", help="the ORIGINAL ILM this OBJ was exported from")
    i.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    if a.cmd == "export":
        out = a.out or (os.path.splitext(a.ilm)[0] + ".obj")
        export(a.ilm, out)
    else:
        out = a.out or (os.path.splitext(a.obj)[0] + "_new.ILM")
        import_obj(a.obj, a.ilm, out)


if __name__ == "__main__":
    main(sys.argv[1:])
