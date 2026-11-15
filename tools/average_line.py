# A line away from the barriers (local tool, pgr4 project): the mean of the laps of every AI car of a
# record (src/pgr4_autopilot.cpp's `record`), place by place along the first's. One car's line touches
# the edges; their mean stays nearer the road's middle.
#   py average_line.py <record> <out line> <lap length, m> [car ...]
import sys
from collections import defaultdict

import numpy as np

src, dst, length = sys.argv[1], sys.argv[2], float(sys.argv[3])
wanted = set(sys.argv[4:])
cars = defaultdict(list)
for l in open(src):
    p = l.split()
    if len(p) == 6 and p[0] != 't':
        cars[p[1]].append((float(p[0]), float(p[2]), float(p[3]), float(p[4]), float(p[5])))


def clean(rows):
    r = np.array(rows)
    return r[(r[:, 2] > 50) & (r[:, 4] < 120) & (r[:, 4] > 8)]


def one_lap(r):
    """One clean lap: the first stretch of about a lap with no jump and no crash."""
    xz = r[:, [1, 3]]
    seg = np.hypot(*np.diff(xz, axis=0).T)
    d = np.concatenate(([0], np.cumsum(np.minimum(seg, 30))))
    dv = np.abs(np.diff(r[:, 4]) / np.maximum(np.diff(r[:, 0]), 1e-3))
    for s in range(0, len(r) - 100, 10):
        e = np.searchsorted(d, d[s] + length)
        if e >= len(r):
            break
        # Back where it began, smoothly all the way.
        if np.hypot(*(xz[e] - xz[s])) < 25 and seg[s:e].max() < 25 and dv[s:e].max() < 25:
            return r[s:e + 1]
    return None


laps = {}
for c, rows in cars.items():
    if wanted and c not in wanted or len(rows) < 500:
        continue
    r = clean(rows)
    if len(r) < 500:
        continue
    lap = one_lap(r)
    print(c, 'tour' if lap is not None else 'aucun tour', None if lap is None else round(lap[-1, 0] - lap[0, 0], 1))
    if lap is not None:
        laps[c] = lap
if not laps:
    sys.exit('aucun tour')
ref = max(laps.values(), key=lambda a: a[-1, 0] - a[0, 0] > 0)
dr = np.concatenate(([0], np.cumsum(np.hypot(*np.diff(ref[:, [1, 3]], axis=0).T))))
n = int(dr[-1] / 2)
s = np.linspace(0, dr[-1], n, endpoint=False)
base = np.stack([np.interp(s, dr, ref[:, k]) for k in (1, 2, 3, 4)], axis=1)
sum_xyz = base[:, :3].copy()
count = np.ones(n)
speed = base[:, 3].copy()
for c, lap in laps.items():
    if lap is ref:
        continue
    d = np.concatenate(([0], np.cumsum(np.hypot(*np.diff(lap[:, [1, 3]], axis=0).T))))
    t = np.arange(0, d[-1], 2.0)
    other = np.stack([np.interp(t, d, lap[:, k]) for k in (1, 2, 3, 4)], axis=1)
    # Nearest points, the way the line goes (a street may be driven twice).
    dirs = np.gradient(base[:, [0, 2]], axis=0)
    odirs = np.gradient(other[:, [0, 2]], axis=0)
    for i in range(n):
        dist = np.hypot(other[:, 0] - base[i, 0], other[:, 2] - base[i, 2])
        same = (odirs[:, 0] * dirs[i, 0] + odirs[:, 1] * dirs[i, 1]) > 0
        dist[~same] = 1e9
        j = dist.argmin()
        if dist[j] < 15:
            sum_xyz[i] += other[j, :3]
            speed[i] = min(speed[i], other[j, 3])
            count[i] += 1
mean = sum_xyz / count[:, None]
line = np.column_stack((mean, speed))
size = 15
ker = np.hanning(size + 2)[1:-1]
ker /= ker.sum()
for k in range(4):
    w = np.concatenate((line[-size:, k], line[:, k], line[:size, k]))
    line[:, k] = np.convolve(w, ker, mode='same')[size:-size]
# A point every metre, as the driver expects.
d = np.concatenate(([0], np.cumsum(np.hypot(*np.diff(line[:, [0, 2]], axis=0).T))))
m = int(d[-1])
s2 = np.linspace(0, d[-1], m, endpoint=False)
out = np.stack([np.interp(s2, d, line[:, k]) for k in range(4)], axis=1)
np.savetxt(dst, out, fmt='%.3f')
shift = np.hypot(line[:, 0] - base[:, 0], line[:, 2] - base[:, 2])
print(len(laps), 'tours,', m, 'm, voitures par point', round(count.mean(), 1), 'ecart a la ligne d\'une seule', round(shift.mean(), 1), 'm en moyenne,', round(shift.max(), 1), 'au plus')
print('vitesses', round(out[:, 3].min() * 3.6), 'a', round(out[:, 3].max() * 3.6), 'km/h')
