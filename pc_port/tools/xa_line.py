"""Decode disc voice lines (g_XaItemData) to mono 16-bit WAV files.

    python xa_line.py XA_DIR OUT_DIR IDX [IDX ...]
    python xa_line.py XA_DIR OUT_DIR speaker=HERO,SIBYL     (lines lip_speakers.txt gives them)

XA_DIR is the extracted XA folder (raw 2336-byte sectors); the game's XA file
index 1..9 is the nine numbered voice files in disc order (05_ .. 45_). The
decoding is the launcher's Voices tool's (XaToolForm.cs), the game's own
arithmetic: follow the line's first sector's file/channel until its authored
length is covered.
"""
import os
import re
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FPOS = (0, 60, 115, 98, 122, 0, 0, 0)
FNEG = (0, 0, -52, -55, -60, 0, 0, 0)


def items():
    src = open(os.path.join(ROOT, 'src/bodyprog/sound/sound_data.c'), encoding='utf-8', errors='replace').read()
    m = re.search(r"g_XaItemData\[727\]\s*=\s*\{(.*?)\n\};", src, re.S)
    rows = re.findall(r"\{\s*([^}]*?)\s*\}", m.group(1))
    out = []
    for r in rows:
        f = [x.strip() for x in r.split(',')]
        out.append((int(f[0], 0), int(f[4], 0), int(f[6], 0)))
    return out


def subblock(sec, group, sb, prev):
    hdr = sec[group + 4 + sb]
    shift, fi = hdr & 0xF, (hdr >> 4) & 7
    fp, fn = FPOS[fi], FNEG[fi]
    byte_i, nib = sb >> 1, 4 if sb & 1 else 0
    out = []
    p0, p1 = prev
    for w in range(28):
        n = (sec[group + 16 + w * 4 + byte_i] >> nib) & 0xF
        s = ((n << 12) & 0xFFFF)
        s = (s - 0x10000 if s & 0x8000 else s) >> shift
        s += (p0 * fp + p1 * fn + 32) >> 6
        p1, p0 = p0, s
        out.append(32767 if s > 32767 else -32768 if s < -32768 else s)
    prev[0], prev[1] = p0, p1
    return out


def decode(xa_files, item):
    fi, sector, frames = item
    path = xa_files[fi - 1]
    with open(path, 'rb') as f:
        def read(i):
            f.seek(i * 2336)
            b = f.read(2336)
            return b if len(b) == 2336 else None
        first = read(sector)
        file_no, chan, coding = first[0], first[1], first[3]
        stereo = coding & 1
        rate = 37800 if ((coding >> 2) & 3) == 0 else 18900
        per = 2016 if stereo else 4032
        wanted = ((rate // 60) * frames + per - 1) // per
        hl, hr = [0, 0], [0, 0]
        pcm = []
        cur, matched, cap = sector, 0, wanted * 32
        while matched < wanted and cap > 0:
            cap -= 1
            sec = read(cur)
            cur += 1
            if sec is None:
                break
            if sec[0] != file_no or sec[1] != chan:
                continue
            for g in range(18):
                grp = 8 + g * 128
                if stereo:
                    for p in range(4):
                        a = subblock(sec, grp, 2 * p, hl)
                        b = subblock(sec, grp, 2 * p + 1, hr)
                        pcm += [(x + y) // 2 for x, y in zip(a, b)]
                else:
                    for sb in range(8):
                        pcm += subblock(sec, grp, sb, hl)
            matched += 1
    return pcm, rate


def write_wav(path, pcm, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))


def main():
    xa_dir, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    xa_files = sorted(os.path.join(xa_dir, n) for n in os.listdir(xa_dir) if re.match(r'\d\d_\d+$', n))
    assert len(xa_files) == 9, xa_files
    table = items()
    want = []
    for a in sys.argv[3:]:
        if a.startswith('speaker='):
            names = set(a.split('=', 1)[1].split(','))
            for line in open(os.path.join(HERE, 'lip_speakers.txt'), encoding='utf-8'):
                m = re.match(r'\s*(\d+)\s*=\s*(\w+)', line)
                if m and m.group(2) in names:
                    want.append(int(m.group(1)))
        else:
            want.append(int(a))
    for idx in want:
        pcm, rate = decode(xa_files, table[idx])
        write_wav(os.path.join(out, '%03d.wav' % idx), pcm, rate)
        print('%3d: %.2f s (authored %.2f s)' % (idx, len(pcm) / rate, table[idx][2] / 60.0))


if __name__ == '__main__':
    main()
