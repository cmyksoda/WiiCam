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
globals_start = main.rfind("enum {", 0, main.index("AV_AUDIO_ONLY"))
globals_end = main.index("static bool av_audio_start(", globals_start)
prepare_start = main.index("static void release_microphone(")
prepare_end = main.index("typedef struct {", prepare_start)
prime_start = main.index("static IsoStop av_audio_prime(")
prime_end = main.index("static bool save_photo(", prime_start)
wait_start = main.index("static bool av_audio_wait_rate(")
wait_end = main.index("static bool av_audio_queue_start(", wait_start)
settle = next(line for line in main.splitlines() if line.startswith("#define AV_RATE_SETTLE_MS "))
code = settle + "\n" + main[globals_start:globals_end]
code += "static bool av_audio_start(s32 handle, const UvcMode *mode);\n"
code += "static void av_audio_close(void);\n"
code += "static void av_audio_stop(void);\n"
code += "static bool av_audio_queue_start(MicRateResult rate, unsigned phase, uint64_t configured);\n"
queue_end = main.index("static bool av_audio_start(s32 handle, const UvcMode *mode)\n{", wait_end)
consume_start = main.index("static bool av_audio_consume(")
consume_end = main.index("static void av_audio_stopping(", consume_start)
log_start = main.index("static void av_audio_log_rejections(")
log_end = main.index("static void av_audio_close(void)\n{", log_start)
cancel_start = main.index("static void av_audio_cancel(")
cancel_end = main.index("static void *av_audio_worker(", cancel_start)
drain_start = main.index("static bool drain_audio_queue(")
drain_end = main.index("static void mic_cancel(", drain_start)
code += main[consume_start:consume_end] + main[log_start:log_end]
code += main[drain_start:drain_end] + main[cancel_start:cancel_end]
code += main[prepare_start:prepare_end] + main[wait_start:queue_end] + main[prime_start:prime_end]
with tempfile.TemporaryDirectory(prefix="wiicam-av-startup-") as directory:
    work = Path(directory)
    (work / "av_startup_under_test.h").write_text(code)
    subprocess.run(
        [*shlex.split(args.cc), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-Isource", "-I" + directory, "source/iso_trace.c", "source/audio_stream.c", "source/iso_queue.c", "source/uvc.c",
         "tests/test_av_startup.c", "-o", str(work / "test")],
        cwd=root, check=True,
    )
    subprocess.run([str(work / "test")], cwd=root, check=True)
