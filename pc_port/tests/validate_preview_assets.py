#!/usr/bin/env python3
"""Validate the exact PNG/JPEG showcase manifest, without Pillow or a disc."""
import hashlib
import json
from pathlib import Path
import struct
import sys


def dimensions(data):
    if data.startswith(b'\x89PNG\r\n\x1a\n'):
        return struct.unpack('>II', data[16:24])
    if data[:2] != b'\xff\xd8':
        raise ValueError('not PNG/JPEG')
    pos = 2
    while pos < len(data):
        if data[pos] != 255:
            raise ValueError('invalid JPEG marker')
        while data[pos] == 255:
            pos += 1
        marker = data[pos]
        pos += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        size = int.from_bytes(data[pos:pos+2], 'big')
        if size < 2 or pos+size > len(data):
            raise ValueError('truncated JPEG segment')
        if marker in (0xC0, 0xC1, 0xC2):
            height, width = struct.unpack('>HH', data[pos+3:pos+7])
            return width, height
        pos += size
    raise ValueError('missing JPEG dimensions')


def validate(root):
    hashes = json.loads((root/'provenance.json').read_text())['files']
    rows = [line.split() for line in (root/'manifest.tsv').read_text().splitlines()
            if line.strip() and not line.startswith('#')]
    assert 0 < len(rows) <= 1024
    seen, files, settings = set(), set(), set()
    pgxp = {}
    for key, value, frames, fps, pattern in rows:
        value, frames, fps = float(value), int(frames), int(fps)
        assert (key, value) not in seen
        seen.add((key, value))
        settings.add(key)
        assert 1 <= frames <= 60 and 1 <= fps <= 60
        assert not Path(pattern).is_absolute() and '..' not in pattern and '\\' not in pattern
        if key == 'use_pgxp':
            assert frames == 1
            pgxp[value] = pattern
        for i in range(frames):
            name = pattern % i if frames > 1 else pattern
            data = (root/name).read_bytes()
            assert 0 < len(data) <= 4*1024*1024
            width, height = dimensions(data)
            assert 0 < width <= 1280 and 0 < height <= 720
            assert width*height*4*frames <= 32*1024*1024
            assert hashlib.sha256(data).hexdigest() == hashes[name], name
            files.add(name)
    assert set(files) == set(hashes), 'unreferenced or missing provenance entries'
    assert set(pgxp) == {0., 1.} and pgxp[0.] != pgxp[1.]
    print(f'PASS: {len(rows)} variants, {len(files)} images, {len(settings)} settings; static PGXP pair')


if __name__ == '__main__':
    validate(Path(sys.argv[1]))
