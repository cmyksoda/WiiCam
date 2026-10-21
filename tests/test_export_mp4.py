# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('export_mp4', ROOT / 'tools/export_mp4.py')
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class ExportTests(unittest.TestCase):
    def test_capture_copies(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            for width in [320, 640]:
                source = ROOT / f'build/test-video-{width}.avi'
                before = hashlib.sha256(source.read_bytes()).hexdigest()
                target = Path(directory) / f'{width}.mp4'
                exporter.export(source, target)
                result = exporter.probe(target)
                self.assertEqual(result['streams'][0]['codec_name'], 'h264')
                self.assertEqual(result['streams'][0]['width'], width)
                self.assertEqual(result['streams'][0]['height'], width * 3 // 4)
                self.assertEqual(result['streams'][0]['pix_fmt'], 'yuv420p')
                self.assertEqual(len(result['streams']), 1)
                data = target.read_bytes()
                boxes, at = [], 0
                while at < len(data):
                    size, name = struct.unpack_from('>I4s', data, at)
                    self.assertGreaterEqual(size, 8)
                    self.assertLessEqual(at + size, len(data))
                    boxes.append(name)
                    at += size
                self.assertEqual(at, len(data))
                self.assertLess(boxes.index(b'moov'), boxes.index(b'mdat'))
                with self.assertRaises(ValueError):
                    exporter.export(source, target)
                self.assertEqual(data, target.read_bytes())
                self.assertEqual(before, hashlib.sha256(source.read_bytes()).hexdigest())
                alias = Path(directory) / f'alias-{width}.mp4'
                alias.symlink_to(source)
                with self.assertRaises(ValueError):
                    exporter.export(source, alias)
                self.assertEqual(before, hashlib.sha256(source.read_bytes()).hexdigest())

    def test_pcm_audio_copy(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            directory = Path(directory)
            video = ROOT / 'build/test-video-320.avi'
            duration = float(exporter.probe(video)['format']['duration'])
            wav = directory / 'pcm.wav'
            with wave.open(str(wav), 'wb') as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(16000)
                audio.writeframes(b'\x00\x00' * round(duration * 16000))
            source = directory / 'pcm.avi'
            subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(video), '-i', str(wav),
                            '-c:v', 'copy', '-c:a', 'pcm_s16le', '-shortest', str(source)],
                           check=True)
            target = directory / 'audio.mp4'
            exporter.export(source, target)
            metadata = exporter.probe(target)
            self.assertEqual([s['codec_name'] for s in metadata['streams']], ['h264', 'aac'])
            self.assertEqual(metadata['streams'][1]['channels'], 1)
            self.assertEqual(int(metadata['streams'][1]['sample_rate']), 16000)
            self.assertLess(abs(float(metadata['format']['duration']) - duration), .1)

    def test_invalid_input(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            source = Path(directory) / 'broken.avi'
            source.write_bytes(b'RIFFbroken AVI')
            target = Path(directory) / 'broken.mp4'
            with self.assertRaises(subprocess.CalledProcessError):
                exporter.export(source, target)
            self.assertFalse(target.exists())
            with self.assertRaises(ValueError):
                exporter.export(source, source)


if __name__ == '__main__':
    unittest.main()
