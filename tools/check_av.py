#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode both AVI streams and verify sample timing, audio bytes, and seeking."""
import json
from pathlib import Path
import struct
import subprocess

expected = b''.join(struct.pack('<H', (n * 37) & 65535) for n in range(32000))
for width, height in [(320, 240), (640, 480), (640, 400), (848, 480),
                      (352, 288), (176, 144), (160, 120)]:
    path = Path(f'build/test-av-{width}x{height}.avi')
    result = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-count_frames', '-show_streams', '-show_format',
        '-show_packets', '-of', 'json', str(path)]))
    video, audio = result['streams']
    assert video['codec_name'] == 'mjpeg' and video['avg_frame_rate'] == '10/1'
    assert (video['width'], video['height']) == (width, height)
    assert int(video['nb_read_frames']) == 20 and float(video['duration']) == 2
    assert audio['codec_name'] == 'pcm_s16le' and audio['sample_rate'] == '16000'
    assert audio['channels'] == 1 and int(audio['nb_frames']) == 32000
    assert float(result['format']['duration']) == 2
    video_packets = [p for p in result['packets'] if p['codec_type'] == 'video']
    assert len(video_packets) == 20
    for n, packet in enumerate(video_packets):
        assert abs(float(packet['pts_time']) - n / 10) < 1e-6
        assert abs(float(packet['duration_time']) - .1) < 1e-6
    samples = 0
    for packet in [p for p in result['packets'] if p['codec_type'] == 'audio']:
        assert int(packet['pts']) == samples
        assert int(packet['duration']) * 2 == int(packet['size'])
        samples += int(packet['duration'])
    assert samples == 32000
    pcm = subprocess.check_output(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i',
                                   str(path), '-map', '0:a:0', '-f', 's16le', '-'])
    assert pcm == expected
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(path),
                    '-f', 'null', '-'], check=True)
    pcm = subprocess.check_output(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-ss', '1',
                                   '-i', str(path), '-map', '0:a:0', '-f', 's16le', '-'])
    assert pcm == expected[32000:]
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-ss', '1', '-i', str(path),
                    '-frames:v', '1', '-f', 'null', '-'], check=True)
    print(f'PASS: {width}x{height}: 20 MJPEG frames, 32000 exact PCM samples, 2-second matched timelines and indexed seek')
