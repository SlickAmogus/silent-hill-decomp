"""Histogram of submode bytes across the entire BIN."""
import sys
from collections import Counter

path = sys.argv[1]
counter = Counter()
N = 2352
with open(path, "rb") as f:
    f.seek(0, 2); total = f.tell() // N; f.seek(0)
    print(f"# scanning {total} sectors", file=sys.stderr)
    while True:
        d = f.read(N)
        if len(d) < N: break
        counter[d[18]] += 1  # submode byte (offset 16+2)

print("submode  count")
for k, v in sorted(counter.items()):
    print(f"  0x{k:02x}     {v}")
