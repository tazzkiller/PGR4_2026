# The passes of a native draw log (draws-f<frame>.txt, pgr4_native_draw_log_* / draw-debug.txt bit
# 16): consecutive draws on the same surface (RB_SURFACE_INFO, COLOR_INFO, DEPTH_INFO) grouped, with
# their clears and resolves, draw and primitive counts, depth/blend use and shader pairs.
#   py frame_passes.py draws-f<frame>.txt [--shaders]
import re
import sys
from collections import Counter, OrderedDict

COLOR_FORMATS = {0: '8_8_8_8', 1: '8_8_8_8_GAMMA', 2: '2_10_10_10', 3: '2_10_10_10_FLOAT',
                 4: '16_16', 5: '16_16_16_16', 6: '16_16_FLOAT', 7: '16_16_16_16_FLOAT',
                 10: '2_10_10_10_AS_10_10_10_10', 12: '2_10_10_10_FLOAT_AS_16_16_16_16',
                 14: '32_FLOAT', 15: '32_32_FLOAT'}
DEPTH_FORMATS = {0: 'D24S8', 1: 'D24FS8'}

draw_re = re.compile(r'^(\d+) fn=(\w+) prim=(\d+) n=(\d+) idx=\d vs=(\w+):\w+ ps=(\w+):\w+ '
                     r'rt=(\d+):(-?\d+),(-?\d+) dsv=(-?\d+) z=(\d+)/(\d+)/(\d+) .*?blend=(\d+):'
                     r'.*?samples=(\d+) vp=(\d+),(\d+),(\d+),(\d+).*?tex=(\d+) ci=(\w+) si=(\w+) '
                     r'di=(\w+)')


def surface(si, ci, di, nrt, dsv):
    si, ci, di = int(si, 16), int(ci, 16), int(di, 16)
    pitch = si & 0x3FFF
    msaa = [1, 2, 4, 4][(si >> 16) & 3]
    color = f'c{ci & 0xFFF}:{COLOR_FORMATS.get((ci >> 16) & 0xF, (ci >> 16) & 0xF)}' if nrt else '-'
    depth = f'd{di & 0xFFF}:{DEPTH_FORMATS.get((di >> 16) & 1)}' if dsv >= 0 else '-'
    return f'pitch {pitch} {msaa}x color {color} depth {depth}'


passes = []
cur = None
events = []
for line in open(sys.argv[1], encoding='latin-1'):
    m = draw_re.match(line)
    if m:
        (idx, fn, prim, n, vs, ps, nrt, rtv0, rtv1, dsv, ze, zw, zf, blend, samples, vx, vy, vw, vh,
         tex, ci, si, di) = m.groups()
        key = surface(si, ci, di, int(nrt), int(dsv))
        if cur is None or cur['key'] != key:
            cur = {'key': key, 'first': int(idx), 'draws': 0, 'prims': 0, 'zwrite': 0, 'blend': 0,
                   'pairs': Counter(), 'vp': set(), 'events': events, 'fns': Counter()}
            events = []
            passes.append(cur)
        cur['draws'] += 1
        cur['prims'] += int(n)
        cur['zwrite'] += int(zw)
        cur['blend'] += int(blend)
        cur['pairs'][(vs[:16], ps[:16], int(tex))] += 1
        cur['vp'].add(f'{vw}x{vh}')
        cur['fns'][fn] += 1
        cur['last'] = int(idx)
        continue
    if line.startswith('clear'):
        events.append('clear ' + ' '.join(t for t in line.split() if t.startswith(('si=', 'ci0=', 'di=', 'height='))))
    elif line.startswith('resolve'):
        events.append(line.strip()[:160])

show_shaders = '--shaders' in sys.argv
for i, p in enumerate(passes):
    vps = ' '.join(sorted(p['vp']))[:60]
    print(f"[{i:3d}] draws {p['first']}-{p['last']} ({p['draws']}) {p['key']}  vertices {p['prims']}  "
          f"zwrite {p['zwrite']} blend {p['blend']}  vp {vps}")
    for e in p['events'][:3]:
        print('       before:', e)
    if show_shaders:
        for (vs, ps, tex), c in p['pairs'].most_common(6):
            print(f'       {c:4d} x VS {vs} PS {ps} tex {tex}')
