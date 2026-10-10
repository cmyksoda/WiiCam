# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 cmyksoda
"""Exercise the Wii startup functions with deterministic USB/time substitutes."""

import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default="cc")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
main = (root / "source/main.c").read_text()
globals_start = main.index("enum { AV_AUDIO_ONLY,")
globals_end = main.index("static bool av_audio_start(", globals_start)
prepare_start = main.index("static bool prepare_camera(")
prepare_end = main.index("typedef struct {", prepare_start)
prime_start = main.index("static IsoStop av_audio_prime(")
prime_end = main.index("static bool save_photo(", prime_start)
code = main[globals_start:globals_end]
code += "static bool av_audio_start(s32 handle, const UvcMode *mode);\n"
code += "static void av_audio_close(void);\n"
code += main[prepare_start:prepare_end] + main[prime_start:prime_end]
with tempfile.TemporaryDirectory(prefix="wiicam-av-startup-") as directory:
    work = Path(directory)
    (work / "av_startup_under_test.h").write_text(code)
    subprocess.run(
        [*shlex.split(args.cc), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-Isource", "-I" + directory, "source/audio_stream.c",
         "tests/test_av_startup.c", "-o", str(work / "test")],
        cwd=root, check=True,
    )
    subprocess.run([str(work / "test")], cwd=root, check=True)
