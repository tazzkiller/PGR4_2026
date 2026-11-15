# The test driver's line (local tool, pgr4 project): from src/pgr4_autopilot.cpp's record of every
# car, the cleanest fast lap of one of them, resampled every 2 m and smoothed.
#   py racing_line.py <record file> <line file> [car, hex]
# Laps: the car back within 15 m of a point it passed over 300 m of driving before. Closed circuits
# only. Prints each car's laps, then the one taken: the quickest, not counting those with a stop
# or a sudden loss of speed (a crash).
import math
import sys

import numpy as np


def laps_of(rows):
    """rows: t, x, z, y, speed. Pairs of indexes, a lap each."""
    p = rows[:, 1:3]
    start = 0
    out = []
    seg = np.hypot(*np.diff(p, axis=0).T)
    driven = np.concatenate(([0.0], np.cumsum(seg)))
    while start < len(p) - 50:
        later = np.nonzero(driven - driven[start] > 300.0)[0]
        later = later[later > start]
        if not len(later):
            break
        d = np.hypot(*(p[later] - p[start]).T)
        close = np.nonzero(d < 15.0)[0]
        if not len(close):
            start += 20
            continue
        first = close[0]
        # The closest point of this pass.
        run = first
        while run + 1 < len(d) and d[run + 1] <= d[run]:
            run += 1
        end = later[run]
        out.append((start, end))
        start = end
    return out


def clean(rows):
    v = rows[:, 4]
    if v.min() < 8.0:
        return False
    dv = np.diff(v) / np.maximum(np.diff(rows[:, 0]), 1e-3)
    return dv.min() > -22.0


def main():
    src, dst = sys.argv[1:3]
    want = sys.argv[3].lower() if len(sys.argv) > 3 else None
    cars = {}
    for line in open(src):
        p = line.split()
        if len(p) != 6 or p[0] == 't':
            continue
        cars.setdefault(p[1], []).append((float(p[0]), float(p[2]), float(p[4]), float(p[3]), float(p[5])))
    best = None
    for car, rows in cars.items():
        rows = np.array(rows)
        for a, b in laps_of(rows):
            lap = rows[a:b + 1]
            t = lap[-1, 0] - lap[0, 0]
            ok = clean(lap)
            print(car, f'tour {t:.1f} s, {len(lap)} points, {"propre" if ok else "accroc"}')
            if ok and (want is None or car == want) and (best is None or t < best[0]):
                best = (t, car, lap)
    if not best:
        sys.exit('aucun tour propre')
    t, car, lap = best
    print('retenu', car, f'{t:.1f} s')
    xz = lap[:, 1:3]
    d = np.concatenate(([0.0], np.cumsum(np.hypot(*np.diff(xz, axis=0).T))))
    keep = np.concatenate(([True], np.diff(d) > 1e-3))
    d, lap = d[keep], lap[keep]
    n = int(d[-1] / 2.0)
    s = np.linspace(0.0, d[-1], n, endpoint=False)
    out = np.stack([np.interp(s, d, lap[:, k]) for k in (1, 3, 2, 4)], axis=1)  # x y z speed
    # Smoothed around the loop (the 10 a second points cut the bends' corners a little).
    size = 5
    kernel = np.hanning(size + 2)[1:-1]
    kernel /= kernel.sum()
    for k in range(4):
        wrapped = np.concatenate((out[-size:, k], out[:, k], out[:size, k]))
        out[:, k] = np.convolve(wrapped, kernel, mode='same')[size:-size]
    np.savetxt(dst, out, fmt='%.3f')
    length = d[-1]
    print(f'{n} points, {length:.0f} m, {out[:, 3].max() * 3.6:.0f} km/h au plus, '
          f'{out[:, 3].min() * 3.6:.0f} km/h au moins, moyenne {length / t * 3.6:.0f} km/h')
    if math.isnan(out.sum()):
        sys.exit('valeurs invalides')


if __name__ == '__main__':
    main()
