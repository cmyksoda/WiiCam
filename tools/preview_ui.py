#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
from PIL import Image, ImageDraw
names = ['menu', 'no-signal', 'timer', 'video', 'no-sd', 'review', 'write-error', 'no-signal-wide']
plate = Image.new('RGB', (1320, 2100), '#edf3f5')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(names):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640,480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#253f4c')
    plate.paste(im, (x,y))
plate.save('build/Pearl-runtime-host-QA.png')

refresh_names = ['refresh-busy', 'refresh-spin', 'refresh-found-320', 'refresh-found-640',
                 'refresh-missing', 'refresh-cancelling', 'refresh-cancelled', 'refresh-wide']
plate = Image.new('RGB', (1320, 2100), '#edf3f5')
draw = ImageDraw.Draw(plate)
for n, name in enumerate(refresh_names):
    im = Image.open(Path('build')/f'ui-{name}.ppm')
    im.save(Path('build')/f'ui-{name}.png')
    im.thumbnail((640, 480))
    x, y = 10+(n%2)*660, 28+(n//2)*525
    draw.text((x,y-20), name, fill='#253f4c')
    plate.paste(im, (x,y))
plate.save('build/Refresh-runtime-host-QA.png')
