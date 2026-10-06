#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 cmyksoda
"""Read Linux USB descriptors for 057e:030a without opening a video stream."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Write a new report; never overwrite")
    args = parser.parse_args()
    if not shutil.which("lsusb"):
        parser.error("lsusb is required (usually the usbutils package)")
    found = subprocess.run(
        ["lsusb", "-d", "057e:030a"], capture_output=True, text=True, check=False
    )
    if found.returncode != 0 or not found.stdout.strip():
        print("No WC04 (057e:030a) found. No camera was opened.", file=sys.stderr)
        return 3
    details = subprocess.run(
        ["lsusb", "-v", "-d", "057e:030a"], capture_output=True, text=True, check=False
    )
    report = (
        "WC04 Linux descriptor report - no video captured\n\n"
        + details.stdout
        + "\nlsusb diagnostics:\n"
        + details.stderr
    )
    if args.output:
        with args.output.open("x", encoding="utf-8") as output:
            output.write(report)
        print(f"Wrote {args.output}")
    else:
        print(report)
    return details.returncode


if __name__ == "__main__":
    raise SystemExit(main())
