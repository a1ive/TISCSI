# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 A1ive
"""Run the native VHDX fault sweep, then recover each copy with qemu-img."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

import vhdx_tool as vx


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, help="compiled vhdx_faults.exe")
    parser.add_argument("--qemu-img", default=os.environ.get(
        "TISCSI_QEMU_IMG", r"C:\Program Files\qemu\qemu-img.exe"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tiscsi-vhdx-faults-") as tmp:
        root = Path(tmp)
        base = root / "base.vhdx"
        cases = root / "cases"
        vx.create(base)
        subprocess.run([str(Path(args.exe).resolve()), str(base), str(cases)], check=True)
        old = bytes(16*vx.MiB)
        new = old[:2*vx.MiB] + bytes([0xD7])*512 + old[2*vx.MiB+512:]
        count = 0
        for path in sorted(cases.glob("case-*.vhdx")):
            for command in ([args.qemu_img, "check", "-r", "all", str(path)],
                            [args.qemu_img, "convert", "-f", "vhdx", "-O", "raw", str(path), str(root / "out.raw")]):
                result = subprocess.run(command, capture_output=True, timeout=120)
                if result.returncode:
                    raise RuntimeError(str(path) + ": " + (result.stdout + result.stderr).decode("utf-8", "replace"))
            if (root / "out.raw").read_bytes() not in (old, new):
                raise AssertionError("Incorrect disk contents after recovery: " + str(path))
            count += 1
        if not count:
            raise AssertionError("No failure snapshots were produced")
        print(f"PASS qemu recovery and complete-disk content for {count} interrupted-write snapshots")


if __name__ == "__main__":
    main()
