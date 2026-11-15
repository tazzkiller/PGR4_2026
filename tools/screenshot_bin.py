# A back buffer dumped by pgr4-native (frame_dump.h: <capture dir>\screenshot.txt makes
# screenshot_<frame>.bin beside the executable) to a PNG, optionally cropped and scaled.
#   py screenshot_bin.py <in.bin> <out.png> [--crop x0,y0,x1,y1] [--width N]
import struct
import sys

import numpy as np
from PIL import Image

args = sys.argv[1:]
src, dst = args[0], args[1]
crop = None
width = None
for i, a in enumerate(args):
    if a == '--crop':
        crop = tuple(int(v) for v in args[i + 1].split(','))
    if a == '--width':
        width = int(args[i + 1])

data = open(src, 'rb').read()
w, h, pitch, fmt = struct.unpack_from('<4I', data, 0)
rows = np.frombuffer(data, dtype=np.uint8, offset=16, count=pitch * h).reshape(h, pitch)
if fmt in (24, 25):  # R10G10B10A2_UNORM / _UINT
    px = rows[:, :w * 4].copy().view('<u4').reshape(h, w)
    rgb = np.stack([(px >> s) & 1023 for s in (0, 10, 20)], -1).astype(np.float32) / 1023.0
elif fmt in (27, 28, 29):  # R8G8B8A8
    rgb = rows[:, :w * 4].reshape(h, w, 4)[..., :3].astype(np.float32) / 255.0
elif fmt in (87, 88, 90, 91):  # B8G8R8A8 / B8G8R8X8
    rgb = rows[:, :w * 4].reshape(h, w, 4)[..., 2::-1].astype(np.float32) / 255.0
elif fmt == 10:  # R16G16B16A16_FLOAT, scRGB: to sRGB
    lin = rows[:, :w * 8].copy().view('<f2').reshape(h, w, 4)[..., :3].astype(np.float32)
    lin = np.clip(lin, 0.0, 1.0)
    rgb = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
else:
    sys.exit(f'format {fmt} not handled')
img = Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8))
if crop:
    img = img.crop(crop)
if width and img.width > width:
    img = img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)
img.save(dst)
print(f'{w}x{h} format {fmt} -> {dst}')
