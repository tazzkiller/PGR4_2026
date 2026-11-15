# The texture list of a .stream2 (local tool, pgr4 project): every resource decompressed by
# lzxprobe/stream2ms.exe, its header read before it (01, width, height, width, height, format code,
# mip count, then the sizes: decompressed, 0, decompressed, compressed, big-endian) and its
# TextureHash (XXH3 of the resource ^ width << 48 | height << 32 | Xenos format << 24), written as
# the tsv family.py reads (hash, width, height, DXGI format, mips, offset in hex, bytes).
#   py stream2_textures.py <file.stream2> <out dir>
import os
import struct
import subprocess
import sys

import xxhash

STREAM2MS = r'D:\Apps\PGR4\pgr4\tools\lzxprobe\stream2ms.exe'
# Header format code -> (Xenos format, DXGI format).
FORMATS = {4: (18, 71), 7: (20, 77), 10: (49, 83)}

src, out = sys.argv[1], sys.argv[2]
resm = os.path.join(out, 'resm')
if not os.path.exists(os.path.join(resm, 'list.txt')):
    os.makedirs(resm, exist_ok=True)
    subprocess.run([STREAM2MS, src, resm], check=True)
data = open(src, 'rb').read()
rows, skipped = [], 0
for line in open(os.path.join(resm, 'list.txt')).read().split('\n'):
    p = line.split()
    if len(p) < 4 or p[3] != 'ok':
        continue
    off, usize, csize = int(p[0], 16), int(p[1]), int(p[2])
    quad = struct.pack('>IIII', usize, 0, usize, csize)
    at = data.rfind(quad, max(0, off - 64), off + 16)
    if at < 35:
        skipped += 1
        continue
    hdr = data[at - 35:at]
    if hdr[0] != 1:
        skipped += 1
        continue
    w, h = struct.unpack('>II', hdr[1:9])
    code = struct.unpack('>H', hdr[17:19])[0]
    mips = struct.unpack('>I', hdr[19:23])[0]
    if code not in FORMATS or not w or not h:
        skipped += 1
        continue
    xenos, dxgi = FORMATS[code]
    res = open(os.path.join(resm, f'{off:010x}_{usize}.bin'), 'rb').read()
    key = xxhash.xxh3_64_intdigest(res) ^ (w << 48 | h << 32 | xenos << 24)
    rows.append(f'{key:016X}\t{w}\t{h}\t{dxgi}\t{mips}\t{off:x}\t{len(res)}')
open(os.path.join(out, 'textures.tsv'), 'w').write(
    'hash\twidth\theight\tdxgi\tmips\tstream2_offset\tbytes\n' + '\n'.join(rows) + '\n')
print(f'{len(rows)} textures, {skipped} ressources sans en-tete de texture')
