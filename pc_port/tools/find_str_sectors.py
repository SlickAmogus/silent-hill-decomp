#!/usr/bin/env python3
"""find_str_sectors.py — locate STR files by scanning sector subheaders + chunk headers."""
import sys, struct

BIN_SECTOR_SIZE = 2352
SYNC_SKIP       = 16            # 12 sync + 4 header
SUBHEADER_OFS   = SYNC_SKIP

def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1
    path = sys.argv[1]
    with open(path, "rb") as f:
        f.seek(0, 2)
        total_bytes = f.tell()
        f.seek(0)
        total_sectors = total_bytes // BIN_SECTOR_SIZE

        print(f"# scanning {total_sectors} sectors of {path}", file=sys.stderr)

        video_runs = []  # list of (start_sector, length) of contiguous video sector runs
        run_start = None
        last_frame_num = None
        last_chunks_in_frame = None

        for s in range(total_sectors):
            f.seek(s * BIN_SECTOR_SIZE)
            sector = f.read(BIN_SECTOR_SIZE)
            if len(sector) < BIN_SECTOR_SIZE:
                break

            submode = sector[SUBHEADER_OFS + 2]
            is_video = (submode & 0x0E) == 0x02

            if is_video:
                # Read chunk header at offset SYNC_SKIP+8
                co = SYNC_SKIP + 8
                chunk_num,       = struct.unpack_from("<H", sector, co + 0x00)
                chunks_in_frame, = struct.unpack_from("<H", sector, co + 0x02)
                frame_num,       = struct.unpack_from("<I", sector, co + 0x04)
                w,               = struct.unpack_from("<H", sector, co + 0x0C)
                h,               = struct.unpack_from("<H", sector, co + 0x0E)
                magic,           = struct.unpack_from("<H", sector, co + 0x10)
                qs,              = struct.unpack_from("<H", sector, co + 0x12)
                ver,             = struct.unpack_from("<H", sector, co + 0x14)
                if run_start is None:
                    run_start = s
                    print(f"VIDEO @ {s:8d}  chunk={chunk_num}/{chunks_in_frame}  "
                          f"frame={frame_num}  size={w}x{h}  magic={magic:04x}  qs={qs}  v={ver}")
                last_frame_num = frame_num
                last_chunks_in_frame = chunks_in_frame
            else:
                if run_start is not None:
                    video_runs.append((run_start, s - run_start))
                    run_start = None

        if run_start is not None:
            video_runs.append((run_start, total_sectors - run_start))

        print(f"# found {len(video_runs)} contiguous video runs", file=sys.stderr)
        for start, length in video_runs[:50]:
            print(f"# run: start_sector={start} length={length}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
