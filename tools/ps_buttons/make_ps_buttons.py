# PlayStation button icons over the game's own (texture CB9C371CA8213F81: 512x512 BC3, one level,
# a grid of 8x8 icons of 64x64; dumped by pgr4_native_texture_dump_dir), for a PlayStation
# controller (pgr4-native textures.cpp loads textures\playstation\<hash>.bin then):
#   A B X Y -> cross, circle, square, triangle (dark glossy buttons, the symbols in their colors)
#   LB RB LT RT -> L1 R1 L2 R2, the sticks' L and R -> L3 and R3 (the game's shapes, their
#   text redrawn)
#   BACK START -> SHARE OPTIONS
# Only the BC3 blocks of the changed icons are re-encoded, the others are the original's.
#   py make_ps_buttons.py original_CB9C371CA8213F81.bin ../../textures/playstation/CB9C371CA8213F81.bin preview.png
import io
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from bcdec import decode

SRC_BIN, OUT_BIN, PREVIEW = sys.argv[1:4]
W = H = 512
CELL = 64
INK = (46, 45, 46, 255)  # the game's outline and text
orig = open(SRC_BIN, 'rb').read()
base = Image.fromarray(decode(orig, W, H, 77), 'RGBA')
new = base.copy()
changed = np.zeros((H // 4, W // 4), bool)


def cell(r, c):
    return new.crop((c * CELL, r * CELL, (c + 1) * CELL, (r + 1) * CELL))


def put(r, c, img):
    new.paste(img, (c * CELL, r * CELL))
    changed[r * CELL // 4:(r + 1) * CELL // 4, c * CELL // 4:(c + 1) * CELL // 4] = True


def font(size, black=True):
    return ImageFont.truetype(r'C:\Windows\Fonts\ariblk.ttf' if black else r'C:\Windows\Fonts\arialbd.ttf', size)


# Face buttons: the game's are 44 px glossy balls (x and y 10-53) with a 2 px dark rim.
S = 8  # drawn at 8 times the size, then reduced


def face(symbol, color):
    big = Image.new('RGBA', (CELL * S, CELL * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    c = 31.5 * S
    r = 22 * S
    d.ellipse([c - r, c - r, c + r, c + r], fill=INK)
    # The body: near black, a little lighter toward its centre (a ball lit from the front).
    body = r - 2.2 * S
    for i in range(int(body), 0, -1):
        g = int(24 + 26 * (1 - i / body))
        d.ellipse([c - i, c - i, c + i, c + i], fill=(g, g, g + 3, 255))
    w = 3.6 * S  # symbol stroke
    k = 10.5 * S  # symbol half size
    if symbol == 'cross':
        d.line([c - k, c - k, c + k, c + k], fill=color, width=int(w))
        d.line([c - k, c + k, c + k, c - k], fill=color, width=int(w))
    elif symbol == 'circle':
        d.ellipse([c - k, c - k, c + k, c + k], outline=color, width=int(w))
    elif symbol == 'square':
        q = k * 0.86
        d.rectangle([c - q, c - q, c + q, c + q], outline=color, width=int(w))
    elif symbol == 'triangle':
        h = k * 1.05
        pts = [(c, c - h), (c + h * 1.0, c + h * 0.72), (c - h * 1.0, c + h * 0.72)]
        d.line(pts + [pts[0], pts[1]], fill=color, width=int(w), joint='curve')
    return big.resize((CELL, CELL), Image.LANCZOS)


put(7, 4, face('cross', (124, 178, 232, 255)))     # A
put(7, 5, face('circle', (255, 102, 102, 255)))    # B
put(7, 6, face('square', (229, 160, 208, 255)))    # X
put(7, 7, face('triangle', (64, 226, 160, 255)))   # Y


def flood_outside(alpha):
    """Pixels reachable from the border through transparency, then the dark rim next to them."""
    h, w = alpha.shape
    seen = np.zeros_like(alpha, bool)
    stack = [(y, x) for y in range(h) for x in (0, w - 1)] + [(y, x) for x in range(w) for y in (0, h - 1)]
    while stack:
        y, x = stack.pop()
        if y < 0 or x < 0 or y >= h or x >= w or seen[y, x] or alpha[y, x] > 40:
            continue
        seen[y, x] = True
        stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    return seen


def components(mask):
    """The 4-connected components of `mask`, as lists of (y, x)."""
    h, w = mask.shape
    seen = np.zeros_like(mask)
    out = []
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or seen[y0, x0]:
                continue
            comp, stack = [], [(y0, x0)]
            seen[y0, x0] = True
            while stack:
                y, x = stack.pop()
                comp.append((y, x))
                for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            out.append(comp)
    return out


def relabel(r, c, text, size, dy=0, letters=0):
    """The game's shape with its dark text erased (white where no outline reaches) and `text`.
    `letters`: only that many letter-shaped dark marks inside are the text (the sticks' other marks
    kept); 0: every one."""
    img = cell(r, c)
    a = np.array(img)
    alpha = a[..., 3]
    outside = flood_outside(alpha)
    lum = a[..., :3].mean(axis=2)
    dark = (alpha > 40) & (lum < 240)  # the text and its anti-aliased edge
    # The rim: dark pixels within 3 px of the outside.
    near = np.zeros_like(outside)
    for oy in range(-3, 4):
        for ox in range(-3, 4):
            near |= np.roll(np.roll(outside, oy, 0), ox, 1)
    text_px = dark & ~near
    if letters:
        keep = np.zeros_like(text_px)
        # A letter: narrow (at most 12 px), at least 8 px high, centred across the icon.
        def is_letter(comp):
            xs = [x for _, x in comp]
            ys = [y for y, _ in comp]
            return (max(xs) - min(xs) <= 12 and max(ys) - min(ys) >= 8 and
                    24 <= (min(xs) + max(xs)) / 2 <= 40)
        for comp in [k for k in components(text_px) if is_letter(k)][:letters]:
            for y, x in comp:
                keep[y, x] = True
        # Their anti-aliased edge too.
        grown = keep.copy()
        for oy in (-1, 0, 1):
            for ox in (-1, 0, 1):
                grown |= np.roll(np.roll(keep, oy, 0), ox, 1)
        text_px = grown & dark
    a[text_px] = (255, 255, 255, 255)
    img = Image.fromarray(a, 'RGBA')
    ys, xs = np.nonzero(text_px)
    cx = (xs.min() + xs.max() + 1) / 2 if len(xs) else 32
    cy = (ys.min() + ys.max() + 1) / 2 if len(ys) else 32
    big = img.resize((CELL * S, CELL * S), Image.LANCZOS)
    d = ImageDraw.Draw(big)
    f = font(size * S)
    b = d.textbbox((0, 0), text, font=f)
    d.text((cx * S - (b[0] + b[2]) / 2, (cy + dy) * S - (b[1] + b[3]) / 2), text, font=f, fill=INK)
    put(r, c, big.resize((CELL, CELL), Image.LANCZOS))


relabel(1, 0, 'L2', 15)  # LT
relabel(1, 1, 'R2', 15)  # RT
relabel(1, 2, 'L1', 15)  # LB
relabel(1, 3, 'R1', 15)  # RB
# The sticks (their L and R): L3 and R3.
relabel(1, 4, 'R3', 13, letters=1)  # right stick, pressed
relabel(1, 5, 'R3', 13, letters=1)  # right stick
relabel(1, 6, 'L3', 13, letters=1)  # left stick, pressed
relabel(1, 7, 'L3', 13, letters=1)  # left stick
relabel(6, 2, 'L3', 13, letters=1)  # left stick, round
relabel(6, 3, 'R3', 13, letters=1)  # right stick, round


def caption(r, c, text, size):
    """BACK/START: the arrow kept, the white caption under it redrawn."""
    img = cell(r, c)
    a = np.array(img)
    a[41:, :, :] = 0  # the caption's rows (the arrow's ball ends above)
    img = Image.fromarray(a, 'RGBA')
    big = img.resize((CELL * S, CELL * S), Image.LANCZOS)
    d = ImageDraw.Draw(big)
    f = font(size * S, black=False)
    b = d.textbbox((0, 0), text, font=f, stroke_width=int(1.6 * S))
    x = 32 * S - (b[0] + b[2]) / 2
    y = 47.5 * S - (b[1] + b[3]) / 2
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255), stroke_width=int(1.6 * S), stroke_fill=INK)
    put(r, c, big.resize((CELL, CELL), Image.LANCZOS))


caption(0, 2, 'SHARE', 9.5)    # BACK
caption(0, 3, 'OPTIONS', 8)    # START

# Encode, then keep the original blocks of the icons left as they were.
buf = io.BytesIO()
new.save(buf, format='DDS', pixel_format='DXT5')
dds = buf.getvalue()
assert dds[:4] == b'DDS ' and dds[84:88] == b'DXT5'
enc = dds[128:128 + (W // 4) * (H // 4) * 16]
o = np.frombuffer(orig[:len(enc)], np.uint8).reshape(H // 4, W // 4, 16).copy()
e = np.frombuffer(enc, np.uint8).reshape(H // 4, W // 4, 16)
o[changed] = e[changed]
out = o.tobytes()
assert len(out) == len(orig)
os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
open(OUT_BIN, 'wb').write(out)
# Preview: as the game decodes it, twice the size, on the game's dark blue.
res = Image.fromarray(decode(out, W, H, 77), 'RGBA')
bg = Image.new('RGBA', (W, H), (60, 60, 90, 255))
bg.alpha_composite(res)
bg.resize((W * 2, H * 2), Image.NEAREST).convert('RGB').save(PREVIEW)
print('ok', int(changed.sum()), 'blocks re-encoded')
