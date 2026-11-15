# HD textures for pgr4-native (local tool, pgr4 project): the game's textures as the engine dumps
# them (<hash>_<w>x<h>_<DXGI>_<mips>.bin, the host's BC blocks, every level packed), enlarged by
# Real-ESRGAN, then written as <hash>.dds (the game's block format, all levels) for
# pgr4_native_texture_hd_dir.
#   py hdtex.py select <dump dir> <png dir> [max]   # BC1 and BC3 textures of 64 or more
#   py hdtex.py encode <png dir> <upscaled dir> <out dir> [scale]
# select writes <hash>_<w>x<h>.png (opaque BC1), <hash>_<w>x<h>_a.png (BC1 with punch-through
# alpha) or <hash>_<w>x<h>_d5.png (BC3): the colors only, for Real-ESRGAN; the alpha of the last
# two goes beside as <hash>_<w>x<h>_<kind>.alpha.png, enlarged by encode itself (Lanczos).
import io
import os
import re
import struct
import sys

import numpy as np
from PIL import Image


def dds_header(w, h, fourcc, mips, data_len):
    return struct.pack('<4s7I44s2I4s5I5I', b'DDS ', 124, 0x1007 | 0x20000 | 0x80000, h, w,
                       data_len, 0, mips, b'\0' * 44, 32, 0x4, fourcc, 0, 0, 0, 0, 0,
                       0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)


def load_bc(path, w, h, fourcc=b'DXT1'):
    bs = 8 if fourcc == b'DXT1' else 16
    data = open(path, 'rb').read()[:max(1, w // 4) * max(1, h // 4) * bs]
    return Image.open(io.BytesIO(dds_header(w, h, fourcc, 1, len(data)) + data)).convert('RGBA')


def load_bc1(path, w, h):
    return load_bc(path, w, h, b'DXT1')


def to_blocks(a, c):
    h, w = a.shape[:2]
    return a.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return (c[:, 0] >> 3) << 11 | (c[:, 1] >> 2) << 5 | (c[:, 2] >> 3)


def from565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], 1).astype(
        np.float32)


def color_blocks(rgb, transparent=None):
    """BC1 color blocks: endpoints along each block's principal axis (of its opaque pixels),
    indices by nearest palette color. transparent: (blocks, 16) bools, or None (4-color mode
    everywhere, as BC3's color blocks need). A block with transparent pixels uses the 3-color
    mode (c0 <= c1), index 3 for them."""
    px = to_blocks(rgb, 3).astype(np.float32)
    n = len(px)
    if transparent is None:
        transparent = np.zeros((n, 16), bool)
    opaque = ~transparent
    weight = opaque.astype(np.float32)[..., None]
    count = np.maximum(weight.sum(1), 1.0)
    mean = (px * weight).sum(1, keepdims=True) / count[:, None]
    cen = (px - mean) * weight
    cov = np.einsum('bni,bnj->bij', cen, cen)
    axis = np.ones((n, 3), np.float32)
    for _ in range(8):
        axis = np.einsum('bij,bj->bi', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    proj = np.einsum('bni,bi->bn', px - mean, axis)
    big = np.where(opaque, proj, -1e9).max(1, keepdims=True)
    small = np.where(opaque, proj, 1e9).min(1, keepdims=True)
    big = np.where(opaque.any(1, keepdims=True), big, 0)
    small = np.where(opaque.any(1, keepdims=True), small, 0)
    hi = mean[:, 0] + axis * big
    lo = mean[:, 0] + axis * small
    a, b = to565(hi), to565(lo)
    three = transparent.any(1)
    # 4-color mode: c0 > c1; 3-color mode: c0 <= c1.
    c0 = np.where(three, np.minimum(a, b), np.maximum(a, b))
    c1 = np.where(three, np.maximum(a, b), np.minimum(a, b))
    p0, p1 = from565(c0), from565(c1)
    pal4 = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)
    pal3 = np.stack([p0, p1, (p0 + p1) / 2, np.full_like(p0, 1e9)], 1)
    pal = np.where(three[:, None, None], pal3, pal4)
    dist = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    idx = dist.argmin(-1).astype(np.uint32)
    idx = np.where(transparent, 3, idx)
    idx = np.where(((c0 == c1) & ~three)[:, None], 0, idx)
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0] = c0 & 255
    out[:, 1] = c0 >> 8
    out[:, 2] = c1 & 255
    out[:, 3] = c1 >> 8
    for k in range(4):
        out[:, 4 + k] = (bits >> (8 * k)) & 255
    return out


def encode_bc1(rgb, alpha=None):
    """rgb: (h, w, 3) uint8, h and w multiples of 4; alpha: (h, w) uint8 or None (opaque). BC1
    with punch-through alpha: transparent below 128, as the game's alpha test."""
    transparent = None
    if alpha is not None:
        transparent = to_blocks(alpha[..., None], 1)[..., 0] < 128
    return color_blocks(rgb, transparent).tobytes()


def alpha_blocks(alpha):
    """BC3 alpha blocks: 8-level mode, endpoints the block's extremes, indices by nearest."""
    a = to_blocks(alpha[..., None], 1)[..., 0].astype(np.float32)
    a0 = a.max(1)
    a1 = a.min(1)
    flat = a0 == a1
    a1 = np.where(flat & (a0 > 0), a0 - 1, a1)
    a0 = np.where(flat & (a0 == 0), 1, a0)
    lv = [a0, a1] + [((7 - k) * a0 + k * a1) / 7 for k in range(1, 7)]
    pal = np.stack(lv, 1)
    idx = np.abs(a[:, :, None] - pal[:, None, :]).argmin(-1).astype(np.uint64)
    bits = (idx << (3 * np.arange(16, dtype=np.uint64))).sum(1).astype(np.uint64)
    out = np.zeros((len(a), 8), np.uint8)
    out[:, 0] = a0.astype(np.uint8)
    out[:, 1] = a1.astype(np.uint8)
    for k in range(6):
        out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def encode_bc3(rgb, alpha):
    return np.concatenate([alpha_blocks(alpha), color_blocks(rgb)], 1).tobytes()


def write_dds(img, path, kind='', alpha=None, cover=None):
    """img: RGB PIL image, power-of-two size; alpha: L PIL image of its size (kinds 'a', 'd5').
    Every level down to 1x1, in BC1 ('' and 'a') or BC3 ('d5'). cover: the cut-out's share (alpha
    128 or more) of each level by its width, as the game's own mips (tools/diffusion/
    mip_coverage.py: thicker the smaller); a level listed is cut where its box-filtered alpha gives
    that share ('a'), or its alpha shifted to reach it, its edges kept smooth ('d5')."""
    w, h = img.size
    levels = []
    cur, cur_a = img.convert('RGB'), alpha
    while True:
        cw, ch = cur.size
        pw, ph = max(4, cw), max(4, ch)
        rgb = np.asarray(cur.resize((pw, ph), Image.NEAREST) if (pw, ph) != (cw, ch) else cur)
        a = None
        if cur_a is not None:
            a = np.asarray(cur_a.resize((pw, ph), Image.NEAREST) if (pw, ph) != (cw, ch) else cur_a)
            if cover and cw in cover:
                share = min(max(cover[cw], 0.0), 1.0)
                t = np.quantile(a, 1.0 - share) if 0.0 < share < 1.0 else (0 if share >= 1 else 256)
                if kind == 'a':
                    a = np.where(a >= max(t, 1), 255, 0).astype(np.uint8)
                else:
                    # The threshold brought to 128, what is transparent left so: a shift of all the
                    # alpha lifted the background of the small levels (06/11/2026: the alpha to mask's
                    # grid showed it as dots around the trees' leaves).
                    v = a.astype(np.float32)
                    tf = float(min(max(t, 1), 254))
                    a = np.clip(np.rint(np.where(v < tf, v * 128.0 / tf,
                                                 128.0 + (v - tf) * 127.0 / (255.0 - tf))),
                                0, 255).astype(np.uint8)
        levels.append(encode_bc3(rgb, a) if kind == 'd5' else encode_bc1(rgb, a))
        if cw == 1 and ch == 1:
            break
        size = (max(1, cw // 2), max(1, ch // 2))
        cur = cur.resize(size, Image.BOX)
        if cur_a is not None:
            cur_a = cur_a.resize(size, Image.BOX)
    with open(path, 'wb') as f:
        f.write(dds_header(w, h, b'DXT5' if kind == 'd5' else b'DXT1', len(levels),
                           len(levels[0])))
        f.write(b''.join(levels))


def is_data(rgba):
    """Textures that are data, not pictures: the scenery's lightmaps (light and shadows packed
    channel by channel: mostly vivid green patches), masks (magenta and cyan), lookup bars.
    Real-ESRGAN invents detail in them (shadows moved and hardened). Calibrated on New York."""
    a = rgba[..., :3].astype(np.int16)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    green = ((g > r + 40) & (g > b + 40)).mean()
    magenta = ((r > g + 60) & (b > g + 60)).mean()
    return green > 0.15 or magenta > 0.2


def select(dump, png, limit):
    n = 0
    for f in sorted(os.listdir(dump)):
        m = re.match(r'([0-9A-F]{16})_(\d+)x(\d+)_(71|77)_(\d+)\.bin$', f)
        if not m:
            continue
        w, h = int(m.group(2)), int(m.group(3))
        # The scenery is mostly tiled from small textures (64x128, 256x128...): all of 64 or more.
        if min(w, h) < 64 or os.path.exists(os.path.join(png, f'{m.group(1)}_{w}x{h}.png')):
            continue
        dxt5 = m.group(4) == '77'
        im = load_bc(os.path.join(dump, f), w, h, b'DXT5' if dxt5 else b'DXT1')
        if is_data(np.asarray(im)):
            continue
        alpha = np.asarray(im)[..., 3]
        kind = 'd5' if dxt5 else ('a' if alpha.min() < 255 else '')
        stem = f'{m.group(1)}_{w}x{h}' + (f'_{kind}' if kind else '')
        im.convert('RGB').save(os.path.join(png, stem + '.png'))
        if kind:
            Image.fromarray(alpha).save(os.path.join(png, stem + '.alpha.png'))
        n += 1
        if n >= limit:
            break
    print(n, 'textures selected')


def encode_one(job):
    png, up, out, scale, f = job
    m = re.match(r'([0-9A-F]{16})_(\d+)x(\d+)(?:_(a|d5))?\.png$', f)
    w, h, kind = int(m.group(2)), int(m.group(3)), m.group(4) or ''
    size = (w * scale, h * scale)
    big = Image.open(os.path.join(up, f)).convert('RGB').resize(size, Image.LANCZOS)
    alpha = None
    if kind:
        alpha = Image.open(os.path.join(png, f[:-4] + '.alpha.png')).convert('L').resize(
            size, Image.LANCZOS)
    # Written under a temporary name, so that a stopped run leaves no partial .dds.
    path = os.path.join(out, m.group(1) + '.dds')
    write_dds(big, path + '.tmp', kind, alpha)
    os.replace(path + '.tmp', path)
    return 1


def encode(png, up, out, scale):
    """Every texture of `png` enlarged in `up` whose .dds is not in `out` yet, on every core."""
    import multiprocessing
    os.makedirs(out, exist_ok=True)
    jobs = []
    for f in sorted(os.listdir(png)):
        m = re.match(r'([0-9A-F]{16})_(\d+)x(\d+)(?:_(a|d5))?\.png$', f)
        if not m or not os.path.exists(os.path.join(up, f)):
            continue
        if os.path.exists(os.path.join(out, m.group(1) + '.dds')):
            continue
        jobs.append((png, up, out, scale, f))
    with multiprocessing.Pool() as pool:
        n = sum(pool.imap_unordered(encode_one, jobs, chunksize=4))
    print(n, 'HD textures written')


if __name__ == '__main__':
    if sys.argv[1] == 'select':
        select(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 1 << 30)
    else:
        encode(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 2)
