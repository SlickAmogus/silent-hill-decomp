#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack an Essential QoL mod and bring its catalog entry up to date.

    qol_mod_pack.py <mod folder> <essential-mods.json> <id> [--out DIR] [--no-verify]

The folder is zipped flat (modinfo.txt, readme.txt, load/...), the way the
launcher's Mod Manager and the in-game installer both expect, and written as
the entry's "file". The entry's size, sha256 and files are then rewritten, and
"verify" is added: one "crc32 size path" line per file under load/. The game
and the launcher compare those lines with the copy a player has, so a changed
mod is found by its content, whatever their install records say.

"verify" is left out for a pack over 16 MB (every title screen of a new build
would read the whole pack) or with --no-verify; those fall back on the sha256
recorded at install time.

To publish, upload the zip and then the catalog to the qol-mods release:
    gh release upload qol-mods <zip> --clobber
    gh release upload qol-mods essential-mods.json --clobber
"""
import argparse
import hashlib
import io
import json
import os
import sys
import time
import zipfile
import zlib
from collections import OrderedDict

VERIFY_LIMIT = 16 * 1024 * 1024


def dump(cat):
    return json.dumps(cat, indent=2, ensure_ascii=False)


def read_catalog(path):
    raw = io.open(path, "r", encoding="utf-8", newline="").read()
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    cat = json.loads(text, object_pairs_hook=OrderedDict)
    tail = "\n" if text.endswith("\n") else ""
    if dump(cat) + tail != text:
        sys.exit("refusing to rewrite %s: it is not in the layout this tool writes" % path)
    return cat, crlf, tail


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("catalog")
    ap.add_argument("id")
    ap.add_argument("--out", default=None, help="where the zip goes (default: beside the catalog)")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    cat, crlf, tail = read_catalog(args.catalog)
    entry = next((m for m in cat["mods"] if m.get("id") == args.id), None)
    if entry is None:
        sys.exit("no catalog entry with id %s" % args.id)

    files = []
    for root, dirs, names in os.walk(args.folder):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(root, name)
            rel = os.path.relpath(full, args.folder).replace(os.sep, "/")
            files.append((rel, full))
    files.sort()
    load = [(rel, full) for rel, full in files if rel.lower().startswith("load/")]
    if not load:
        sys.exit("%s has no load/ folder: this tool packs load mods" % args.folder)

    out_dir = args.out or os.path.dirname(os.path.abspath(args.catalog))
    zip_path = os.path.join(out_dir, entry["file"])
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel, full in files:
            info = zipfile.ZipInfo(rel, time.localtime(os.path.getmtime(full))[:6])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())

    blob = open(zip_path, "rb").read()
    entry["size"] = len(blob)
    entry["sha256"] = hashlib.sha256(blob).hexdigest()
    entry["files"] = "\n".join(rel for rel, _ in load)

    lines, total = [], 0
    for rel, full in load:
        data = open(full, "rb").read()
        total += len(data)
        lines.append("%08x %d %s" % (zlib.crc32(data) & 0xFFFFFFFF, len(data), rel))
    if args.no_verify or total > VERIFY_LIMIT:
        entry.pop("verify", None)
    else:
        entry["verify"] = "\n".join(lines)

    text = dump(cat) + tail
    if crlf:
        text = text.replace("\n", "\r\n")
    io.open(args.catalog, "w", encoding="utf-8", newline="").write(text)

    print("%s  %d bytes  sha256 %s" % (zip_path, entry["size"], entry["sha256"]))
    for line in lines:
        print("  " + line)
    print("verify: %s" % ("written" if "verify" in entry else "left out"))


if __name__ == "__main__":
    main()
