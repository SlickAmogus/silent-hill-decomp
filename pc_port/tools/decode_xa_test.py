#!/usr/bin/env python3
"""Standalone XA-ADPCM 4-bit stereo decoder, ported from vgmstream xa_decoder.c
   Used to produce a reference WAV for comparison against the C decoder output.

   Usage:
     python decode_xa_test.py <xa_file> <start_sector> <num_sectors> <output.wav>
"""

import struct
import sys
import wave
from pathlib import Path

K0 = [0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
K1 = [0,  0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]

XA_SECTOR_SIZE = 2336
XA_PAYLOAD_OFFSET = 8
XA_GROUP_SIZE = 128
XA_GROUPS_PER_SECTOR = 18

def clamp16(v):
    if v > 32767: return 32767
    if v < -32768: return -32768
    return v

def decode_subblock(group, i, channel, hist):
    """Decode 28 samples for one (sub-block i, channel) per vgmstream's algorithm.
       Returns (samples, updated_hist).
       hist = [hist1, hist2] where hist1 is newer, hist2 is older."""
    # Header byte position per vgmstream: sp_pos = 0x04 + i*channels + channel  (channels=2 stereo)
    sp_pos = 0x04 + i * 2 + channel
    sp = group[sp_pos]
    index = (sp >> 4) & 0xF
    shift = sp & 0xF
    if shift > 12:
        shift = 9  # vgmstream's shift_limit clamp

    coef1 = K0[index] if index < 5 else 0
    coef2 = K1[index] if index < 5 else 0

    samples = []
    h1, h2 = hist
    for j in range(28):
        # Data byte: 0x10 + j*4 + i; nibble determined by channel
        su_pos = 0x10 + j * 4 + i
        su = group[su_pos]
        if channel == 1:
            nibble = (su >> 4) & 0xF
        else:
            nibble = su & 0xF

        # Sign-extend 4-bit nibble into 16-bit, then arithmetic-shift right.
        # vgmstream: sample = (int16_t)((su << 12) & 0xf000) >> shift
        raw = (nibble << 12) & 0xFFFF
        # Convert to signed int16
        if raw & 0x8000:
            raw -= 0x10000
        # Arithmetic right shift on signed value (Python's >> is arithmetic for negative)
        sample = raw >> shift

        # IIR filter with rounding (+32)
        sample += (coef1 * h1 + coef2 * h2 + 32) >> 6

        # Update history with UNCLAMPED value, output clamped
        h2 = h1
        h1 = sample
        samples.append(clamp16(sample))

    return samples, [h1, h2]

def decode_sector(sector_bytes, hist_l, hist_r):
    """Decode one sector. Returns (interleaved_samples, hist_l, hist_r)."""
    out = []  # interleaved L,R,L,R,...
    coding = sector_bytes[3]
    is_stereo = bool(coding & 1)
    bit_depth = (coding >> 4) & 1
    if bit_depth != 0:
        raise ValueError(f"8-bit not implemented (coding=0x{coding:02X})")
    if not is_stereo:
        # Mono not exercising here — implement if needed
        raise ValueError("Mono path not implemented in this reference")

    for g in range(XA_GROUPS_PER_SECTOR):
        group = sector_bytes[XA_PAYLOAD_OFFSET + g * XA_GROUP_SIZE :
                             XA_PAYLOAD_OFFSET + (g + 1) * XA_GROUP_SIZE]
        # vgmstream's per-channel pass: 4 sub-blocks (i=0..3), each producing 28 samples
        # We interleave by collecting both channels per (i, j)
        for i in range(4):
            l_samples, hist_l = decode_subblock(group, i, 0, hist_l)
            r_samples, hist_r = decode_subblock(group, i, 1, hist_r)
            for s in range(28):
                out.append(l_samples[s])
                out.append(r_samples[s])
    return out, hist_l, hist_r

def main():
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <xa_file> <start_sector> <num_sectors> <output.wav>")
        sys.exit(1)

    infile = Path(sys.argv[1])
    start = int(sys.argv[2])
    n_sec = int(sys.argv[3])
    outfile = sys.argv[4]

    data = infile.read_bytes()

    # Detect format from first sector
    first = data[start * XA_SECTOR_SIZE : start * XA_SECTOR_SIZE + 8]
    coding = first[3]
    is_stereo = bool(coding & 1)
    sr_code = (coding >> 2) & 3
    sample_rate = 37800 if sr_code == 0 else 18900
    print(f"Coding=0x{coding:02X}: stereo={is_stereo} sr={sample_rate} 4bit")

    # Determine (file, channel) filter from the start sector's subheader.
    filter_file    = first[0]
    filter_channel = first[1]
    print(f"Filter: file=0x{filter_file:02X} ch=0x{filter_channel:02X}")

    hist_l = [0, 0]
    hist_r = [0, 0]
    all_samples = []
    matched = 0
    cur = start
    scan_cap = n_sec * 32
    while matched < n_sec and scan_cap > 0:
        scan_cap -= 1
        sec = data[cur * XA_SECTOR_SIZE : (cur + 1) * XA_SECTOR_SIZE]
        cur += 1
        if len(sec) < XA_SECTOR_SIZE:
            print(f"EOF after {matched} matched sectors")
            break
        if sec[0] != filter_file or sec[1] != filter_channel:
            continue
        samples, hist_l, hist_r = decode_sector(sec, hist_l, hist_r)
        all_samples.extend(samples)
        matched += 1

    print(f"Decoded {len(all_samples)} ints ({len(all_samples)//2} stereo pairs, "
          f"{len(all_samples)/2/sample_rate:.3f} sec)")

    # Print first 16 stereo pairs for cross-check with C decoder
    print("First 16 stereo pairs (L,R):")
    for p in range(16):
        print(f"  ({all_samples[p*2]}, {all_samples[p*2+1]})", end="")
    print()

    # Write WAV
    with wave.open(outfile, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        # Pack samples as little-endian s16
        packed = struct.pack(f"<{len(all_samples)}h", *all_samples)
        w.writeframes(packed)
    print(f"Wrote {outfile}")

if __name__ == "__main__":
    main()
