"""Stock ILM (exported OBJ at the rest keyframe) -> native face/chest glb for the
skin loader: the game's own parts for bones 1 (chest) and 2 (head), in game
units and pose, rigid to those bones, with an SH_NATIVE marker node, plus the
face pass (face.py) adding blink/jawOpen geometry and shape keys.
usage: npc2glb.py NAME OBJ TIMBASE REST.txt OUT.glb [params.json]"""
import sys, json, struct, io, re, os
import numpy as np
from PIL import Image

name, objp, timbase, restp, outp = sys.argv[1:6]
params = {}
if len(sys.argv) > 6 and os.path.exists(sys.argv[6]):
    params = json.load(open(sys.argv[6])).get(name, {})

BONES = {'Spine': 1, 'Head': 2, 'RightShoulder': 3, 'RightArm': 4, 'RightForeArm': 5, 'RightHand': 6,
         'LeftShoulder': 7, 'LeftArm': 8, 'LeftForeArm': 9, 'LeftHand': 10, 'Hips': 11, 'RightUpLeg': 12,
         'RightLeg': 13, 'RightFoot': 14, 'LeftUpLeg': 15, 'LeftLeg': 16, 'LeftFoot': 17}


def read_obj(p):
    V, VT, VN, parts, vpart = [], [], [], {}, []
    cur = mat = None
    for l in open(p):
        t = l.split()
        if not t:
            continue
        if t[0] == 'o':
            cur = t[1]; parts[cur] = []
        elif t[0] == 'v':
            V.append([float(x) for x in t[1:4]]); vpart.append(cur)
        elif t[0] == 'vt':
            VT.append([float(x) for x in t[1:3]])
        elif t[0] == 'vn':
            VN.append([float(x) for x in t[1:4]])
        elif t[0] == 'usemtl':
            mat = t[1]
        elif t[0] == 'f':
            corners = []
            for c in t[1:]:
                f = (c.split('/') + ['', ''])[:3]
                corners.append([int(a) - 1 if a else -1 for a in f])
            for k in range(1, len(corners) - 1):
                parts[cur].append((mat, [corners[0], corners[k], corners[k + 1]]))
    return np.array(V), np.array(VT), np.array(VN), parts, vpart


def rest_RT(restfile):
    R, T = [], []
    for m in re.finditer(r'\{ \{ ([^}]*) \}, \{ ([^}]*) \}, \{ [^}]*\} \}', open(restfile).read()):
        R.append(np.array([float(x) for x in m.group(1).split(',')]).reshape(3, 3) / 4096.0)
        T.append([float(x) for x in m.group(2).split(',')])
    return R, np.array(T)


def to_head(P, R, T):
    # OBJ (x, -y, z) -> game -> head-bone local -> back to the OBJ's axis convention
    g = np.c_[P[:, 0], -P[:, 1], P[:, 2]] - T[2]
    l = g @ np.linalg.inv(R[2]).T
    return np.c_[l[:, 0], -l[:, 1], l[:, 2]]


def rest_T(restfile):
    T = []
    for m in re.finditer(r'\{ \{ [^}]*\}, \{ ([^}]*) \}, \{ [^}]*\} \}', open(restfile).read()):
        T.append([float(x) for x in m.group(1).split(',')])
    assert len(T) == 18, len(T)
    return np.array(T)


V, VT, VN, parts, vpart = read_obj(objp)
T = rest_T(restp)

# Each part lists its own vertices, then ones it shares from other parts (the
# neck's lower ring is the chest's). The game moves a shared vertex with its
# owner, so it is bound to the owner's bone here, or the seam would tear.
meta = json.load(open(objp[:-4] + '.ilmmeta.json'))
names = [m['name'] for m in meta['models']]
vbone = []
seen = {}
for vi, pn in enumerate(vpart):
    k = seen.get(pn, 0); seen[pn] = k + 1
    mm = meta['models'][names.index(pn)]
    if k < mm['ownVertexCount']:
        vbone.append(int(pn[:2]))
    else:
        owner = names[mm['foreignVerts'][k - mm['ownVertexCount']]['ownerModel']]
        vbone.append(int(owner[:2]))
vbone = [b if b < 18 else 2 for b in vbone]
sel = {k: v for k, v in parts.items() if k[:2] in ('01', '02')}
alpha = [k for k, v in sel.items() if any(m and m.endswith('_alpha') for m, _ in v)]
if alpha:
    print('SKIP %s: semi-transparent prims in %s' % (name, alpha))
    sys.exit(2)

imgcache = {}


def tex_for(mat):
    row = int(re.search(r'row(\d+)', mat).group(1))
    if row not in imgcache:
        imgcache[row] = Image.open('%s.p%02d.png' % (timbase, row)).convert('RGBA')
    return row, imgcache[row]


# Triangles per material, one output vertex per (position, uv, normal, bone).
mats = {}
for pname, faces in sel.items():
    bone = int(pname[:2])
    for mat, tri in faces:
        mats.setdefault(mat, []).append((bone, tri, pname))

prims = []
for mat, tris in mats.items():
    row, im = tex_for(mat)
    W, H = im.size
    keymap, P, N, UV, J, SRC, PART = {}, [], [], [], [], [], []
    idx = []
    for _, tri, pname in tris:
        for vi, ti, ni in tri:
            bone = vbone[vi]
            u_px = VT[ti][0] * 256.0 - 0.5
            v_px = (1.0 - VT[ti][1]) * 256.0 - 0.5
            uv = ((u_px + 0.5) / W, (v_px + 0.5) / H)
            key = (vi, round(uv[0], 6), round(uv[1], 6), ni, bone)
            if key not in keymap:
                keymap[key] = len(P)
                P.append(V[vi]); N.append(VN[ni] if ni >= 0 else [0, 1, 0]); UV.append(uv); J.append(bone)
                SRC.append(vi); PART.append(pname)
            idx.append(keymap[key])
    prims.append(dict(mat=mat, row=row, W=W, H=H, P=np.array(P, np.float64), N=np.array(N, np.float64),
                      UV=np.array(UV, np.float64), J=list(J), I=list(idx), SRC=SRC, PART=PART))

RR, _ = rest_RT(restp)

targets = []   # names, in order
if params.get('face'):
    import importlib.util
    sp = importlib.util.spec_from_file_location('face', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'face.py'))
    face = importlib.util.module_from_spec(sp); sp.loader.exec_module(face)
    targets = face.build(prims, params['face'], imgcache, tex_for, T, RR)

# Preview only: the head bone's frame, so every face looks straight down +z.
if os.environ.get('HEADLOCAL'):
    for pr in prims:
        pr['P'] = to_head(pr['P'], RR, T)
        pr['N'] = to_head(pr['N'], RR, T * 0)
        for k in pr.get('D', {}):
            pr['D'][k] = to_head(pr['D'][k], RR, T * 0)

print('%s: parts %s, materials %d, verts %d, keys %s' % (name, sorted(sel), len(prims),
      sum(len(p['P']) for p in prims), targets))

# ---- glb
bin_ = bytearray(); views = []; accs = []


def add_view(data, target=None):
    while len(bin_) % 4:
        bin_.append(0)
    off = len(bin_); bin_.extend(data)
    v = {'buffer': 0, 'byteOffset': off, 'byteLength': len(data)}
    if target:
        v['target'] = target
    views.append(v)
    return len(views) - 1


def add_acc(arr, ctype, typ, target=None, minmax=False):
    a = np.ascontiguousarray(arr)
    acc = {'bufferView': add_view(a.tobytes(), target), 'componentType': ctype, 'count': int(a.shape[0]), 'type': typ}
    if minmax:
        acc['min'] = [float(x) for x in a.min(0)]; acc['max'] = [float(x) for x in a.max(0)]
    accs.append(acc)
    return len(accs) - 1


jnames = list(BONES)
jpos = np.array([[T[b][0], -T[b][1], T[b][2]] for b in BONES.values()], np.float32)
nodes = [{'name': 'Armature', 'children': list(range(1, 1 + len(jnames)))}]
for n, p in zip(jnames, jpos):
    nodes.append({'name': n, 'translation': [float(x) for x in p]})
ibm = np.zeros((len(jnames), 16), np.float32)
for i, p in enumerate(jpos):
    m = np.eye(4, dtype=np.float32); m[0:3, 3] = -p
    ibm[i] = m.T.reshape(16)
skin = {'joints': list(range(1, 1 + len(jnames))), 'inverseBindMatrices': add_acc(ibm, 5126, 'MAT4')}
bone2joint = {b: jnames.index(n) for n, b in BONES.items()}

images, textures, materials, meshprims = [], [], [], []
rowtex = {}
for pr in prims:
    if pr['row'] not in rowtex:
        b = io.BytesIO(); imgcache[pr['row']].save(b, 'PNG')
        images.append({'bufferView': add_view(b.getvalue()), 'mimeType': 'image/png'})
        textures.append({'source': len(images) - 1, 'sampler': 0})
        rowtex[pr['row']] = len(textures) - 1
    materials.append({'name': pr['mat'], 'pbrMetallicRoughness': {
        'baseColorTexture': {'index': rowtex[pr['row']]}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0}})
    nv = len(pr['P'])
    joints = np.zeros((nv, 4), np.uint16); joints[:, 0] = [bone2joint[b] for b in pr['J']]
    weights = np.zeros((nv, 4), np.float32); weights[:, 0] = 1.0
    attrs = {'POSITION': add_acc(pr['P'].astype(np.float32), 5126, 'VEC3', 34962, True),
             'NORMAL': add_acc(pr['N'].astype(np.float32), 5126, 'VEC3', 34962),
             'TEXCOORD_0': add_acc(pr['UV'].astype(np.float32), 5126, 'VEC2', 34962),
             'JOINTS_0': add_acc(joints, 5123, 'VEC4', 34962),
             'WEIGHTS_0': add_acc(weights, 5126, 'VEC4', 34962)}
    mp = {'attributes': attrs, 'indices': add_acc(np.array(pr['I'], np.uint32), 5125, 'SCALAR', 34963),
          'material': len(materials) - 1}
    if targets:
        mp['targets'] = [{'POSITION': add_acc(pr['D'][t].astype(np.float32), 5126, 'VEC3', 34962, True)}
                         for t in targets]
    meshprims.append(mp)
mesh = {'name': 'face', 'primitives': meshprims}
if targets:
    mesh['extras'] = {'targetNames': targets}
    mesh['weights'] = [0.0] * len(targets)
nodes.append({'name': 'face', 'mesh': 0, 'skin': 0})
# The bones whose stock parts this replaces; shared seam vertices on other
# bones do not count.
nodes.append({'name': 'SH_NATIVE ' + ' '.join(str(b) for b in sorted({int(k[:2]) for k in sel}))})
g = {'asset': {'version': '2.0', 'generator': 'npc2glb'}, 'scene': 0,
     'scenes': [{'nodes': [0, len(nodes) - 2, len(nodes) - 1]}], 'nodes': nodes, 'meshes': [mesh], 'skins': [skin],
     'materials': materials, 'textures': textures, 'images': images,
     'samplers': [{'magFilter': 9728, 'minFilter': 9728, 'wrapS': 33071, 'wrapT': 33071}],
     'accessors': accs, 'bufferViews': views, 'buffers': [{'byteLength': 0}]}
while len(bin_) % 4:
    bin_.append(0)
g['buffers'][0]['byteLength'] = len(bin_)
j = json.dumps(g, separators=(',', ':')).encode()
while len(j) % 4:
    j += b' '
with open(outp, 'wb') as f:
    f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(bin_)))
    f.write(struct.pack('<II', len(j), 0x4E4F534A)); f.write(j)
    f.write(struct.pack('<II', len(bin_), 0x004E4942)); f.write(bytes(bin_))
print('wrote', outp, len(bin_))
