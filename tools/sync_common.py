#!/usr/bin/env python3
"""
sync_common.py -- copy common/ into each Arduino sketch folder.

The Arduino IDE compiles every source file that sits NEXT TO the .ino and has
no include path for a sibling directory, so shared code cannot simply live in
common/ and be referenced with "../common/...". The usual workaround is to
duplicate the files by hand, which guarantees the four nodes drift apart.

This script makes common/ the single source of truth instead: edit there, run
this, and all four sketches are provably identical.

The copies ARE committed, so the repository's sketch folders can be opened and
compiled straight from a clone without running anything first -- which matters
for a deliverable someone else has to build. Never edit a copy: the next sync
overwrites it. `--check` fails the build if any copy has drifted.

Usage:
    python tools/sync_common.py          # sync
    python tools/sync_common.py --clean  # remove the copies again
    python tools/sync_common.py --check  # exit 1 if anything is out of date
"""

import argparse
import filecmp
import hashlib
import os
import shutil
import stat
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMMON = ROOT / "common"
FIRMWARE = ROOT / "firmware"

SKETCHES = [
    "Node1_Sensor",
    "Node2_SensorRelay",
    "Node3_ClusterHead",
    "Node4_Sink",
]

BANNER = "/* AUTO-COPIED from common/ by tools/sync_common.py -- DO NOT EDIT HERE. */\n"


def source_files():
    files = sorted(
        p for p in COMMON.iterdir()
        if p.is_file() and p.suffix in (".h", ".cpp", ".c")
    )
    if not files:
        sys.exit(f"error: no source files found in {COMMON}")
    return files


def make_writable(path: Path):
    if path.exists():
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sync(check_only=False):
    files = source_files()
    stale, copied = [], 0

    for sketch in SKETCHES:
        dest_dir = FIRMWARE / sketch
        if not dest_dir.is_dir():
            sys.exit(f"error: missing sketch folder {dest_dir}")

        for src in files:
            dest = dest_dir / src.name

            if check_only:
                if not dest.exists() or digest(dest) != digest(src):
                    stale.append(f"{sketch}/{src.name}")
                continue

            if dest.exists() and filecmp.cmp(src, dest, shallow=False):
                continue

            make_writable(dest)
            shutil.copy2(src, dest)
            copied += 1

    if check_only:
        if stale:
            print("Out of date:")
            for s in stale:
                print("   ", s)
            print("\nRun:  python tools/sync_common.py")
            return 1
        print(f"Up to date: {len(files)} modules x {len(SKETCHES)} sketches.")
        return 0

    print(f"Synced {len(files)} modules into {len(SKETCHES)} sketches "
          f"({copied} file(s) updated).")
    for f in files:
        print("   ", f.name)
    return 0


def clean():
    files = source_files()
    removed = 0
    for sketch in SKETCHES:
        for src in files:
            dest = FIRMWARE / sketch / src.name
            if dest.exists():
                make_writable(dest)
                dest.unlink()
                removed += 1
    print(f"Removed {removed} copied file(s).")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean", action="store_true", help="remove the copies")
    ap.add_argument("--check", action="store_true",
                    help="report whether the copies are current (exit 1 if not)")
    args = ap.parse_args()

    if args.clean:
        return clean()
    return sync(check_only=args.check)


if __name__ == "__main__":
    sys.exit(main())
