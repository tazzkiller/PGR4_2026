# Which textures a frame draws and whether each has an HD version (local tool, pgr4 project): the
# textures of a draw log (<capture dir>\draws-f<n>.txt: "tf<n> <address> fmt=<xenos> <w>x<h>x<d>"),
# their base level read from the running game's memory (physical view at 2^33), hashed as
# TextureHash does (XXH3 of the tiled base level, ^ width << 48 | height << 32 | format << 24).
#   py view_textures.py <draws.txt> <hd dir>
import collections
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys

import xxhash

log, hd_dir = sys.argv[1], sys.argv[2]
hd = set(f[:16].upper() for f in os.listdir(hd_dir) if f.endswith('.dds'))
pid = int(subprocess.check_output(
    ['powershell', '-NoProfile', '-Command', '(Get-Process pgr4).Id']).split()[0])
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)


def read(addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    # Texture addresses are physical: the physical view is 2^32 after the virtual one
    # (rexglue Memory::Initialize).
    k32.ReadProcessMemory(h, ctypes.c_void_p((2 << 32) + addr), buf, size, ctypes.byref(got))
    return buf.raw[:got.value]


BPB = {18: 8, 20: 16, 49: 16}
seen = {}
for line in open(log, encoding='utf-8', errors='replace'):
    m = re.match(r'\s+tf(\d+) ([0-9A-F]{8}) fmt=(\d+) (\d+)x(\d+)x1 dim=1 tiled=1', line)
    if not m:
        continue
    addr, fmt, w, hh = int(m.group(2), 16), int(m.group(3)), int(m.group(4)), int(m.group(5))
    if fmt not in BPB or (addr, fmt, w, hh) in seen:
        continue
    # The tiled base level: blocks in 32x32 tiles (pitch and height aligned to 32 blocks).
    bw, bh = max(1, w // 4), max(1, hh // 4)
    size = ((bw + 31) // 32 * 32) * ((bh + 31) // 32 * 32) * BPB[fmt]
    data = read(addr, size)
    k = xxhash.xxh3_64_intdigest(data) ^ (w << 48 | hh << 32 | fmt << 24)
    seen[(addr, fmt, w, hh)] = ('%016X' % k, int(m.group(1)))
stats = collections.Counter()
for (addr, fmt, w, hh), (k, tf) in seen.items():
    big = min(w, hh) >= 64
    stats[('HD' if k in hd else 'sans HD', 'tf%d' % tf, '>=64' if big else '<64')] += 1
for key, v in sorted(stats.items(), key=lambda x: -x[1]):
    print(v, *key)
missing = [(k, w, hh, fmt, 'tf%d' % tf) for (a, fmt, w, hh), (k, tf) in seen.items()
           if k not in hd and min(w, hh) >= 64]
print('exemples sans HD:', missing[:10])
