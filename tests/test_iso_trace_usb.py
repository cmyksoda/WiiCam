# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 cmyksoda
"""Compile the actual V5 ISO builder/callback with deterministic IOS substitutes."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
usb = (root / 'vendor/libogc/usb.c').read_text()
parts = []
for start, end in [('struct _usb_msg {', 'static s32 __usbv5_devicechangeCB('),
                   ('static s32 __usbv5_messageCB(', 'static s32 __usbv0_messageCB('),
                   ('static s32 __usb_isochronous_message(', 'static s32 __usb_control_message(')]:
    first = usb.index(start)
    parts.append(usb[first:usb.index(end, first)])
with tempfile.TemporaryDirectory(prefix='wiicam-iso-trace-') as directory:
    work = Path(directory)
    (work / 'usb_under_test.h').write_text('\n'.join(parts))
    subprocess.run([*shlex.split(args.cc), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-pointer-to-int-cast',
                    '-Isource', '-I' + directory, 'source/iso_trace.c',
                    'tests/test_iso_trace_usb.c', '-o', str(work / 'test')], cwd=root, check=True)
    subprocess.run([str(work / 'test')], cwd=root, check=True)
