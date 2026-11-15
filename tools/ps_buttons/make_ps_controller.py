# The controls screen's controller picture for a PlayStation controller: the game's texture
# 8912A2FD634C6447 (512x256 BC3, one level: the Xbox 360 controller at x 13-351, y 7-246, and a
# shadow at x 416-478) with a DualSense photo (on white) in place of the Xbox controller.
#   py make_ps_controller.py original_8912A2FD634C6447.bin photo.jpeg ../../textures/playstation/8912A2FD634C6447.bin preview.png
# The photo is cut out of its white background (flood fill from the border), its ground shadow
# dropped, and fitted to the Xbox controller's box. Pgr4-native textures.cpp loads it for a
# PlayStation controller. A product photo: for one's own build, not for publishing.
import io
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageFilter
from bcdec import decode

SRC_BIN, PHOTO, OUT_BIN, PREVIEW = sys.argv[1:5]
W, H = 512, 256
BOX = (13, 7, 351, 246)  # the Xbox controller's (x0, y0, x1, y1), inclusive
orig = open(SRC_BIN, 'rb').read()
base = Image.fromarray(decode(orig, W, H, 77), 'RGBA')

photo = Image.open(PHOTO).convert('RGB')
# Working at a quarter of 4K is plenty for a 340x240 result.
photo = photo.resize((photo.width // 4, photo.height // 4), Image.LANCZOS)
rgb = np.array(photo).astype(np.int16)
lo = rgb.min(axis=2)
sat = rgb.max(axis=2) - lo
# Background: near white and grey, reached from the border; the ground shadow below the grips is
# light grey and unsaturated, the controller's white has darker seams around it.
bg_like = (lo >= 236) & (sat <= 6)
h, w = lo.shape
outside = np.zeros((h, w), bool)
stack = [(y, x) for y in range(h) for x in (0, w - 1)] + [(y, x) for x in range(w) for y in (0, h - 1)]
# And from between the grips, which the ground shadow closes off from the border.
stack += [(int(h * 0.70), w // 2), (int(h * 0.62), w // 2)]
while stack:
    y, x = stack.pop()
    if y < 0 or x < 0 or y >= h or x >= w or outside[y, x] or not bg_like[y, x]:
        continue
    outside[y, x] = True
    stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
# The ground shadow: light unsaturated pixels below the grips' lowest dark seam, joined to the
# outside; dropped with a second, looser fill.
shadow_like = (lo >= 150) & (lo < 236) & (sat <= 10)  # greyer than the plastic's white
grow = outside.copy()
stack = list(zip(*np.nonzero(outside)))
while stack:
    y, x = stack.pop()
    for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
        if 0 <= ny < h and 0 <= nx < w and not grow[ny, nx] and shadow_like[ny, nx] and ny > h * 0.86:
            grow[ny, nx] = True
            stack.append((ny, nx))
mask = Image.fromarray(np.where(grow, 0, 255).astype(np.uint8))
# Soft edge: shrink by a pixel, then blur.
mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.8))
cut = photo.convert('RGBA')
cut.putalpha(mask)
bbox = mask.point(lambda v: 255 if v > 24 else 0).getbbox()
cut = cut.crop(bbox)

# Fitted into the Xbox controller's box, centred, aspect kept.
bw, bh = BOX[2] - BOX[0] + 1, BOX[3] - BOX[1] + 1
s = min(bw / cut.width, bh / cut.height)
fit = cut.resize((round(cut.width * s), round(cut.height * s)), Image.LANCZOS)
x = BOX[0] + (bw - fit.width) // 2
y = BOX[1] + (bh - fit.height) // 2
new = base.copy()
a = np.array(new)
a[:, :380] = 0  # the Xbox controller gone (the shadow at the right kept)
new = Image.fromarray(a, 'RGBA')
new.alpha_composite(fit, (x, y))

buf = io.BytesIO()
new.save(buf, format='DDS', pixel_format='DXT5')
dds = buf.getvalue()
assert dds[:4] == b'DDS ' and dds[84:88] == b'DXT5'
enc = dds[128:128 + (W // 4) * (H // 4) * 16]
o = np.frombuffer(orig[:len(enc)], np.uint8).reshape(H // 4, W // 4, 16).copy()
e = np.frombuffer(enc, np.uint8).reshape(H // 4, W // 4, 16)
o[:, :380 // 4] = e[:, :380 // 4]
out = o.tobytes()
assert len(out) == len(orig)
os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
open(OUT_BIN, 'wb').write(out)
res = Image.fromarray(decode(out, W, H, 77), 'RGBA')
bg = Image.new('RGBA', (W, H), (60, 60, 90, 255))
bg.alpha_composite(res)
bg.resize((W * 2, H * 2), Image.LANCZOS).convert('RGB').save(PREVIEW)
print('ok', fit.size, (x, y))
