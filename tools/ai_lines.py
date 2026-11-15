# The racing lines of the game's AI, for the test driver (local tool, pgr4 project). ai.pak: a KAP
# (u32 at 0x10 the directory; u32 count, then per entry a name, u32 position in 2048-byte sectors,
# size, parent, a hash, padding to 36 bytes after the name's 0; a file: u32 0, u32 packed size, zlib).
# <city>/[puddles/]<clockwise|anticlockwise>/<city><route>.aid, little-endian: u32 1, u32 name length,
# the name (Route01_Main_AI_SplineShape), u32 1, u32 count, then per metre 16 floats: the position
# (0-2), the room to each side (4-7), two other points of the road (10-15).
# Writes <out>\<city>_<way>_<route>[_puddles].txt (x y z speed per metre, the speed from the bend's
# radius and the car's grip) and <out>\index.tsv (name, closed or not, the 12 bytes of the position
# of point 100 as the game has them in memory, big-endian, to tell which route is loaded).
#   py ai_lines.py <out dir> [side grip m/s2] [acceleration m/s2]
import os
import struct
import sys
import zlib

import numpy as np

PAK = r'D:\Games\Xenia\Games\Project Gotham Racing 4\ai.pak'
TOP_SPEED = 95.0


def entries(d):
    o = struct.unpack('>I', d[0x10:0x14])[0]
    n = struct.unpack('>I', d[o:o + 4])[0]
    o += 4
    out = []
    for _ in range(n):
        e = d.index(b'\0', o)
        pos, size, parent = struct.unpack('>3I', d[e + 1:e + 13])
        out.append((d[o:e].decode('latin1'), pos, size, parent))
        o = e + 37
    return out


def path(es, i):
    p = []
    c = i
    for _ in range(8):
        p.append(es[c][0])
        c = es[c][3]
        if c >= len(es) or es[c][0] == '\\':
            break
    return list(reversed(p))


def speeds(p, closed, grip, accel):
    n = len(p)
    xz = p[:, [0, 2]]
    span = 8
    a = np.roll(xz, span, axis=0) if closed else xz[np.clip(np.arange(n) - span, 0, n - 1)]
    b = np.roll(xz, -span, axis=0) if closed else xz[np.clip(np.arange(n) + span, 0, n - 1)]
    u, v = xz - a, b - xz
    cross = np.abs(u[:, 0] * v[:, 1] - u[:, 1] * v[:, 0])
    du, dv, dw = np.linalg.norm(u, axis=1), np.linalg.norm(v, axis=1), np.linalg.norm(b - a, axis=1)
    curve = 2.0 * cross / np.maximum(du * dv * dw, 1e-6)  # 1 / radius, through the three points
    v_max = np.minimum(np.sqrt(grip / np.maximum(curve, 1e-5)), TOP_SPEED)
    out = v_max.copy()
    # Up to speed out of a bend, around the loop twice when it closes.
    for _ in range(2 if closed else 1):
        for i in range(n):
            j = (i - 1) % n if closed or i else 0
            out[i] = min(out[i], np.sqrt(out[j] ** 2 + 2.0 * accel))
    return out, curve


def main():
    out_dir = sys.argv[1]
    grip = float(sys.argv[2]) if len(sys.argv) > 2 else 9.0
    accel = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
    os.makedirs(out_dir, exist_ok=True)
    d = open(PAK, 'rb').read()
    es = entries(d)
    index = []
    for i, e in enumerate(es):
        if not e[0].endswith('.aid'):
            continue
        q = e[1] * 2048
        packed = struct.unpack('>I', d[q + 4:q + 8])[0]
        b = zlib.decompress(d[q + 8:q + 8 + packed])
        length = struct.unpack('<I', b[4:8])[0]
        at = 8 + length
        shapes, n = struct.unpack('<II', b[at:at + 8])
        at += 8
        if len(b) < at + n * 64 or n < 150:
            print('passe', '/'.join(path(es, i)), 'points', n)
            continue
        a = np.frombuffer(b, '<f4', count=n * 16, offset=at).reshape(n, 16)
        p = a[:, :3]
        closed = np.linalg.norm(p[0] - p[-1]) < 10.0
        v, curve = speeds(p, closed, grip, accel)
        parts = path(es, i)
        way = 'puddles_' if 'puddles' in parts else ''
        # The route's number and what follows it (st petersburg13_ac01: 13_ac01), each its own file.
        stem = e[0][:-4]
        route = stem[len(parts[0]):] if stem.lower().startswith(parts[0].lower()) else stem
        name = f"{parts[0].replace(' ', '_')}_{way}{parts[-2]}_{route}"
        np.savetxt(os.path.join(out_dir, name + '.txt'), np.column_stack((p, v)), fmt='%.3f')
        key = struct.pack('>3f', *p[100]).hex()
        index.append((name, int(closed), key, n))
    with open(os.path.join(out_dir, 'index.tsv'), 'w') as f:
        for r in index:
            f.write('\t'.join(map(str, r)) + '\n')
    print(len(index), 'trajectoires')
    for r in index[:4]:
        print(r)
    closed = sum(r[1] for r in index)
    print(closed, 'fermees', len(index) - closed, 'ouvertes')


if __name__ == '__main__':
    main()
