#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independently inspect and decode the C writer's host regression clips."""
import json
from pathlib import Path
import subprocess
for width in (320, 640):
    path = Path(f'build/test-video-{width}.avi')
    data = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)]))
    assert len(data['streams']) == 1
    stream = data['streams'][0]
    assert stream['codec_name'] == 'mjpeg'
    assert stream['width'] == width and stream['height'] == width*3//4
    assert int(stream['nb_frames']) == 10
    assert abs(float(data['format']['duration'])-2) < .001
    assert stream['avg_frame_rate'] == '5/1'
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'], check=True)
    # Exercise the index with a seek as well as sequential decoding.
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-ss', '1', '-i', str(path),
                    '-frames:v', '1', '-f', 'null', '-'], check=True)
    print(f'PASS: {width}x{width*3//4} AVI, 10 frames / 2 s, full decode and seek, no audio')
