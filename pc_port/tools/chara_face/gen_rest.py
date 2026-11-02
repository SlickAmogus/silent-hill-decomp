"""Rest table + sole Y + toe directions for any character, from the ILM
converter's OBJ + meta (ANM keyframe 0). Usage: gen_rest.py META OBJ NAME"""
import json, sys
import numpy as np

meta = json.load(open(sys.argv[1]))
objp = sys.argv[2]
NAME = sys.argv[3]

bone_of = {'01CHEST_': 1, '01CHEST2': 1, '10RHAND_': 10, '02HEAD1': 2, '02NECK': 2, '03LSHOUL': 3, '04LJOU': 4, '05LZEN': 5, '06LHAND': 6,
           '07RSHOUL': 7, '08RJOU': 8, '09RZEN': 9, '10RHAND': 10, '11HIP_TC': 11, '11HIP2_T': 11,
           '12LMOMO': 12, '13LSUNE': 13, '14LFOOT': 14, '15RMOMO': 15, '16RSUNE': 16, '17RFOOT': 17}
pts = {}
cur = None
allv = []
for l in open(objp):
    if l.startswith('o '):
        cur = l.split()[1]
    elif l.startswith('v '):
        x, y, z = map(float, l.split()[1:4])
        p = (x, -y, z)
        allv.append((cur, p))
        if cur in bone_of:
            pts.setdefault(bone_of[cur], []).append(p)

R = {}; T = {}
for m in meta['models']:
    b = bone_of.get(m['name'])
    if b is not None and b not in R:
        R[b] = m['restPose']['R']; T[b] = m['restPose']['T']

sole = max(p[1] for n, p in allv if n in ('14LFOOT', '17RFOOT'))
top = min(p[1] for n, p in allv if n == '02HEAD1')


def toe(part, b):
    P = np.array([p for n, p in allv if n == part])
    P = P[P[:, 1] > sole - 15]
    xz = P[:, [0, 2]]
    c = xz.mean(0)
    w, v = np.linalg.eigh(np.cov((xz - c).T))
    d = v[:, np.argmax(w)]
    ank = np.array([T[b][0], T[b][2]])
    if np.dot(d, c - ank) < 0 and abs(np.dot(d, c - ank)) > 1e-6:
        d = -d
    if d[1] < 0:
        d = -d
    return d


t14 = toe('14LFOOT', 14); t17 = toe('17RFOOT', 17)
print('/* %s: sole Y %.1f, head top Y %.1f */' % (NAME, sole, top))
print('#define %s_SOLE_Y (%.1ff)' % (NAME, sole))
print('static const float s_%sToe[2][2] = { { %.4ff, %.4ff }, { %.4ff, %.4ff } };' % (NAME.lower(), t14[0], t14[1], t17[0], t17[1]))
print('static const PcSkinRestBone s_%sRest[PC_SKIN_BONES] = {' % NAME.lower())
print('    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },')
for b in range(1, 18):
    c = [sum(p[i] for p in pts[b]) / len(pts[b]) for i in range(3)]
    print('    { { %s }, { %d, %d, %d }, { %.1ff, %.1ff, %.1ff } },' % (
        ', '.join(str(v) for v in R[b]), T[b][0], T[b][1], T[b][2], c[0], c[1], c[2]))
print('};')
