#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
from PIL import Image, ImageDraw
names = ['menu', 'no-signal', 'timer', 'video', 'no-sd', 'review', 'write-error', 'no-signal-wide']
plate = Image.new('RGB', (1320, 2100), '#161616')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(names):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640,480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#ffffff')
    plate.paste(im, (x,y))
plate.save('build/WiiCam-runtime-host-QA.png')

refresh_names = ['refresh-busy', 'refresh-spin', 'refresh-found-320', 'refresh-found-640',
                 'refresh-missing', 'refresh-cancelling', 'refresh-cancelled', 'refresh-wide']
plate = Image.new('RGB', (1320, 2100), '#161616')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(refresh_names):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640, 480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#ffffff')
    plate.paste(im, (x,y))
plate.save('build/Refresh-runtime-host-QA.png')

extra = ['live', 'shutter', 'waiting', 'signal-recording', 'menu-no-sd', 'menu-timer-10',
         'live-640', 'menu-wide']
plate = Image.new('RGB', (1320, 2100), '#161616')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(extra):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640, 480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#ffffff')
    plate.paste(im, (x,y))
plate.save('build/WiiCam-extra-host-QA.png')

review = ['video-review', 'video-review-confirm', 'video-playing', 'video-deleted',
          'photo-review', 'photo-review-confirm', 'video-review-wide']
plate = Image.new('RGB', (1320, 2100), '#161616')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(review):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640, 480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#ffffff')
    plate.paste(im, (x,y))
plate.save('build/WiiCam-review-host-QA.png')
