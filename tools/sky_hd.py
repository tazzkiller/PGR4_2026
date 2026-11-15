# HD skies (local tool, pgr4 project): the sky panoramas the game is drawing now (psSky, psSkyFog:
# DXT1, 2048x512 or 1024x512), read from its memory at the addresses of a draw log, hashed as
# TextureHash does (XXH3 of the tiled base level ^ width << 48 | height << 32 | Xenos format
# << 24), enlarged x4 by Real-ESRGAN (wrapped around horizontally first: a panorama has no left
# and right edges), written as <hash>.dds (DXT1, every level) to the HD textures' folder.
#   py sky_hd.py <draws log> [model] [--write]   (the game running; skies already done skipped;
#   without --write, the PNGs only, in work\sky_hd)
import os
import re
import subprocess
import sys
import ctypes
import ctypes.wintypes as wt

import numpy as np
import xxhash
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from hdtex import load_bc, write_dds  # noqa: E402
from xenos_untile import untile  # noqa: E402

HD = r'D:\Apps\PGR4\pgr4\out\build\win-amd64-release\textures_hd'
SRC = r'D:\Apps\PGR4\pgr4\textures_hd'
WORK = r'D:\Apps\PGR4\work\sky_hd'
ESRGAN = r'D:\Apps\PGR4\tools\realesrgan\realesrgan-ncnn-vulkan.exe'
SKY = ('25E95607', 'AA1B910E', '529E0E9E', 'A0120EC7', 'A7B4D6C2')
PAD = 64  # columns wrapped around on each side for the enlargement

log = sys.argv[1]
args = [a for a in sys.argv[2:] if not a.startswith('--')]
model = args[0] if args else 'realesrgan-x4plus'
os.makedirs(WORK, exist_ok=True)

pid = int(subprocess.check_output(
    ['powershell', '-NoProfile', '-Command', '(Get-Process pgr4).Id']).split()[0])
k32 = ctypes.WinDLL('kernel32')
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)


def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p((2 << 32) + addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value]


# The skies' textures in the log: the tf lines after their draws.
lines = open(log, encoding='utf-8', errors='replace').read().split('\n')
found = set()
for i, line in enumerate(lines):
    if not re.match(r'\d+ fn=', line) or not any('ps=' + s in line for s in SKY):
        continue
    for t in lines[i + 1:i + 8]:
        m = re.match(r'\s+tf0 ([0-9A-F]{8}) fmt=(\d+) (\d+)x(\d+)x1 dim=1 tiled=1', t)
        if m and m.group(2) == '18':
            found.add((int(m.group(1), 16), int(m.group(3)), int(m.group(4))))

for addr, w, hh in sorted(found):
    bw, bh = w // 4, hh // 4
    data = read(addr, ((bw + 31) // 32 * 32) * ((bh + 31) // 32 * 32) * 8)
    key = '%016X' % (xxhash.xxh3_64_intdigest(data) ^ (w << 48 | hh << 32 | 18 << 24))
    out = os.path.join(HD, key + '.dds')
    if os.path.exists(out):
        print(key, 'déjà en HD')
        continue
    lin = untile(data, w, hh, 8)
    raw = os.path.join(WORK, key + '.bin')
    open(raw, 'wb').write(lin)
    img = np.asarray(load_bc(raw, w, hh).convert('RGB'))
    padded = np.concatenate([img[:, -PAD:], img, img[:, :PAD]], axis=1)
    src_png = os.path.join(WORK, key + '_src.png')
    big_png = os.path.join(WORK, key + '_x4.png')
    Image.fromarray(padded).save(src_png)
    subprocess.run([ESRGAN, '-i', src_png, '-o', big_png, '-n', model, '-s', '4'], check=True,
                   capture_output=True)
    big = np.asarray(Image.open(big_png).convert('RGB'))[:, PAD * 4:PAD * 4 + w * 4]
    Image.fromarray(big).save(os.path.join(WORK, key + '_hd.png'))
    if '--write' in sys.argv:
        write_dds(Image.fromarray(big), out)
    print(key, '%dx%d' % (w, hh), '->', big.shape[1], 'x', big.shape[0])
