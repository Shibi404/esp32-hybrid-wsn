#!/usr/bin/env python3
"""
build_all.py -- verify that all four sketches still compile.

Runs sync_common.py --check first, so a build can never pass while a sketch
folder holds a stale copy of a shared module, then compiles each node with
warnings enabled.

It locates arduino-cli automatically: on PATH, or inside an Arduino IDE 2.x
installation, which is where it lives on a typical lab machine.

Usage:
    python tools/build_all.py
    python tools/build_all.py --warnings none    # faster
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FQBN = "esp32:esp32:esp32:PartitionScheme=huge_app"

SKETCHES = [
    "Node1_Sensor",
    "Node2_SensorRelay",
    "Node3_ClusterHead",
    "Node4_Sink",
]

IDE_PATHS = [
    Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Arduino IDE"
    / "resources" / "app" / "lib" / "backend" / "resources" / "arduino-cli.exe",
    Path("C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"),
    Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"),
]


def find_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    for p in IDE_PATHS:
        if p.exists():
            return str(p)
    sys.exit(
        "arduino-cli not found.\n"
        "Install it, or install Arduino IDE 2.x (which bundles it), or compile\n"
        "the four sketches from the IDE with Partition Scheme = Huge APP."
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--warnings", default="all", choices=["none", "default", "more", "all"])
    args = ap.parse_args()

    print("Checking that the sketch folders hold current copies of common/ ...")
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "sync_common.py"), "--check"])
    if r.returncode != 0:
        return 1

    cli = find_cli()
    print(f"\nUsing {cli}")
    print(f"FQBN  {FQBN}\n")

    failures, rows = 0, []
    for sketch in SKETCHES:
        print(f"  building {sketch} ...", end=" ", flush=True)
        proc = subprocess.run(
            [cli, "compile", "--warnings", args.warnings, "--fqbn", FQBN,
             str(ROOT / "firmware" / sketch)],
            capture_output=True, text=True)
        out = proc.stdout + proc.stderr

        if proc.returncode != 0:
            failures += 1
            print("FAILED")
            for line in out.splitlines():
                if "error" in line.lower():
                    print("      " + line.strip())
            continue

        flash = re.search(r"Sketch uses (\d+) bytes \((\d+)%\)", out)
        ram = re.search(r"Global variables use (\d+) bytes \((\d+)%\)", out)
        warns = [l for l in out.splitlines()
                 if "warning:" in l and ("wsn_" in l or ".ino" in l)]
        rows.append((sketch,
                     f"{int(flash.group(1))/1024:.0f} kB ({flash.group(2)}%)" if flash else "?",
                     f"{int(ram.group(1))/1024:.0f} kB ({ram.group(2)}%)" if ram else "?",
                     len(warns)))
        print("ok" + (f"  ({len(warns)} warnings)" if warns else ""))
        for w in warns:
            print("      " + w.strip())

    if rows:
        print(f"\n  {'sketch':<20} {'flash':<18} {'ram':<16} warnings")
        print("  " + "-" * 62)
        for name, fl, rm, nw in rows:
            print(f"  {name:<20} {fl:<18} {rm:<16} {nw}")

    if failures:
        print(f"\n{failures} sketch(es) FAILED to build.")
        return 1

    print("\nAll four sketches build.")
    print("Remember: Partition Scheme must be 'Huge APP' in the IDE too -- the")
    print("default 1.2 MB app partition is too small for these binaries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
