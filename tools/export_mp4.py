#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create an Android-oriented MP4 copy of a WiiCam AVI using desktop FFmpeg."""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


def run(command):
    return subprocess.run(command, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True).stdout


def probe(path):
    return json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                           '-of', 'json', str(path)]))


def export(source, destination):
    source, destination = Path(source).resolve(), Path(destination).absolute()
    if not source.is_file() or source.stat().st_size > 128 * 1024 * 1024:
        raise ValueError('Input must be a local WiiCam AVI no larger than 128 MiB.')
    if destination.suffix.lower() != '.mp4':
        raise ValueError('Choose a new .mp4 output path.')
    if os.path.lexists(destination):
        raise ValueError('Output already exists; choose a new path.')
    metadata = probe(source)
    streams = metadata['streams']
    video = [s for s in streams if s['codec_type'] == 'video']
    audio = [s for s in streams if s['codec_type'] == 'audio']
    duration = float(metadata['format'].get('duration', 0))
    if (metadata['format']['format_name'] != 'avi' or len(video) != 1 or len(audio) > 1
            or video[0]['codec_name'] != 'mjpeg'
            or not 0 < video[0]['width'] <= 848 or not 0 < video[0]['height'] <= 480
            or not math.isfinite(duration) or not 0 < duration <= 60.1
            or (audio and not audio[0]['codec_name'].startswith('pcm_'))):
        raise ValueError('Expected one bounded MJPEG AVI video stream with optional PCM audio.')
    # Conversion and validation finish before the exclusive final-path creation.
    with tempfile.TemporaryDirectory(prefix='.wiicam-export-', dir=destination.parent) as directory:
        temporary = Path(directory) / 'video.mp4'
        run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(source),
             '-map', '0:v:0', '-map', '0:a:0?', '-c:v', 'libx264', '-profile:v', 'baseline',
             '-level:v', '3.1', '-pix_fmt', 'yuv420p', '-color_range', 'tv',
             '-vf', 'scale=iw:ih:out_range=tv,pad=ceil(iw/2)*2:ceil(ih/2)*2',
             '-preset', 'medium', '-crf', '20', '-c:a', 'aac', '-b:a', '128k',
             '-movflags', '+faststart', str(temporary)])
        output = probe(temporary)
        videos = [s for s in output['streams'] if s['codec_type'] == 'video']
        audios = [s for s in output['streams'] if s['codec_type'] == 'audio']
        if (len(videos) != 1 or videos[0]['codec_name'] != 'h264'
                or videos[0]['pix_fmt'] != 'yuv420p' or len(audios) != len(audio)
                or (audios and audios[0]['codec_name'] != 'aac')
                or abs(float(output['format']['duration']) - duration) > 0.25):
            raise ValueError('MP4 stream or duration validation failed.')
        run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(temporary),
             '-f', 'null', '-'])
        os.link(temporary, destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path, nargs='?')
    args = parser.parse_args()
    try:
        result = export(args.input, args.output or args.input.with_suffix('.mp4'))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Export failed: {error}\n')
    print(f'Saved MP4 copy: {result}')


if __name__ == '__main__':
    main()
