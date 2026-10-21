# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 cmyksoda
"""Exercise the production microphone adapter with deterministic USB callbacks."""

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
start = main.index("typedef struct {\n    s32 handle;\n    UacMode mode;\n    MicPcm pcm;")
end = main.index("static s32 av_audio_finished(", start)
unit = (root / "tests/test_microphone.c").read_text()
fixture_start = unit.index("static Fixture microphone(void)")
fixture_end = unit.index("typedef struct {", fixture_start)

with tempfile.TemporaryDirectory(prefix="wiicam-mic-startup-") as directory:
    work = Path(directory)
    (work / "mic_startup_under_test.h").write_text(main[start:end])
    (work / "mic_fixture.h").write_text(unit[fixture_start:fixture_end])
    subprocess.run(
        [*shlex.split(args.cc), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-Isource", "-Itests", "-I" + directory, "source/iso_trace.c", "source/uvc.c",
         "source/uac.c", "source/microphone.c", "source/iso_queue.c", "source/storage.c",
         "tests/test_mic_startup.c", "-o", str(work / "test")],
        cwd=root, check=True,
    )
    subprocess.run([str(work / "test")], cwd=work, check=True)
