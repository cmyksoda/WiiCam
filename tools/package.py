#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 cmyksoda
"""Package only the experimental runtime, installation note, and licenses."""

import argparse
import json
import shutil
from hashlib import sha256
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "dist/apps/wc04-camera-experimental"
FILES = [
    "boot.dol",
    "meta.xml",
    "INSTALL.txt",
    "licenses/ASSET-PROVENANCE.txt",
    "licenses/Vecteezy-License-Information.pdf",
    "licenses/UI-font.txt",
    "licenses/WC04-GPLv3.txt",
    "licenses/libogc.txt",
    "licenses/wiiuse.txt",
    "licenses/libdvm.txt",
    "licenses/FatFs.txt",
    "licenses/libjpeg-turbo.txt",
    "licenses/JPEG-README.ijg",
]


def validate_dol(data):
    if len(data) < 256:
        raise ValueError("DOL is shorter than its header")
    header = struct.unpack(">64I", data[:256])
    offsets = header[:18]
    addresses = header[18:36]
    sizes = header[36:54]
    entry = header[56]
    sections = []
    for offset, address, size in zip(offsets, addresses, sizes):
        if not size:
            continue
        if offset < 256 or offset + size > len(data):
            raise ValueError("DOL section exceeds file bounds")
        if not 0x80000000 <= address < address + size <= 0x81800000:
            raise ValueError("Unexpected DOL section memory range")
        sections.append((offset, offset + size))
    for previous, current in zip(sorted(sections), sorted(sections)[1:]):
        if previous[1] > current[0]:
            raise ValueError("Overlapping DOL file sections")
    if not any(a <= entry < a + s for a, s in zip(addresses[:7], sizes[:7])):
        raise ValueError("DOL entry is not in a text section")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--local-test', action='store_true')
    args = parser.parse_args()
    if not args.local_test:
        parser.error('font-derived asset redistribution is unverified; only --local-test is supported')
    font = json.loads((ROOT / 'build/ui-font.json').read_text())
    for name in ('ASSET-PROVENANCE.txt', 'Vecteezy-License-Information.pdf'):
        shutil.copyfile(ROOT / 'assets' / name, APP / 'licenses' / name)
    (APP / 'licenses/UI-font.txt').write_text(
        'PRIVATE LOCAL TEST ONLY\nFont: ' + ' '.join(font['name']) +
        '\nSource font SHA-256: ' + font['sha256'] +
        '\nThis DOL embeds rasterized glyphs from the selected local font.\n'
        'The full font file is not included. Redistribution rights are not established.\n'
        'Do not publish this binary without resolving font and artwork permissions.\n')
    data = (APP / "boot.dol").read_bytes()
    validate_dol(data)
    (APP / "licenses/WC04-GPLv3.txt").write_bytes((ROOT / "LICENSE").read_bytes())
    archive = ROOT / "dist/WC04-Camera-0.3.1-uvc-experimental-local-test.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
        for name in FILES:
            package.write(APP / name, f"apps/wc04-camera-experimental/{name}")
    with zipfile.ZipFile(archive) as package:
        assert package.testzip() is None
        assert package.namelist() == [f"apps/wc04-camera-experimental/{name}" for name in FILES]
        assert package.read("apps/wc04-camera-experimental/boot.dol") == data
    print(f"DOL: {len(data)} bytes; SHA-256 {sha256(data).hexdigest()}")
    print(f"ZIP: {archive.relative_to(ROOT)}; SHA-256 {sha256(archive.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
