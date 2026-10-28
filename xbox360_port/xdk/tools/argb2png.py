#!/usr/bin/env python3
"""argb2png.py <file_WxH.argb>... - raw big-endian ARGB8888 page dumps from the
360 backend (gpu_d3d360.cpp) to PNG, for looking at what the sampler saw."""
import re, struct, sys, zlib

def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

for p in sys.argv[1:]:
    m = re.search(r'_(\d+)x(\d+)_', p)
    w, h = int(m.group(1)), int(m.group(2))
    d = open(p, 'rb').read()
    out = bytearray()
    hist = {}
    for i in range(0, w * h * 4, 4):
        a, r, g, b = d[i], d[i + 1], d[i + 2], d[i + 3]      # big-endian ARGB word
        out += bytes((r, g, b, 255 if a else 0))
        hist[d[i:i + 4]] = hist.get(d[i:i + 4], 0) + 1
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:3]
    png(p + '.png', w, h, bytes(out))
    print('%s: %d distinct colours; top: %s' % (p, len(hist), ', '.join('%s x%d' % (k.hex(), v) for k, v in top)))
