# The cars' pixel shaders from a run.log of the vehicles menu (local tool, pgr4 project): the car shown is the one
# whose VehiclePvsData\<car>_PvsCells.dat the game asked for last (it does as it shows a car); each car shader first
# drawn in the 2 s windows of pgr4-native's log (draw.cpp, with a capture dir) goes to it. A shader drawn within
# 0.5 s of a change of car is left out (the previous car's last frames).
#   py car_shaders.py <run.log> <from HH:MM:SS> <to HH:MM:SS> <out.tsv>
import re
import sys
from collections import defaultdict

LINE = re.compile(r'^\[\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d)\.(\d{3})\]')
CAR = re.compile(r"VehiclePvsData\\([^']+?)_PvsCells\.dat")
SHADER = re.compile(r'PGR4 car shader (?:first drawn|without HLSL): ([0-9A-F]{16}) (\S+)(?: (ours))?')


def seconds(m):
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0


def main():
    log, start, end, out = sys.argv[1:5]
    to_s = lambda s: sum(int(x) * k for x, k in zip(s.split(':'), (3600, 60, 1)))
    lo, hi = to_s(start), to_s(end) + 1
    car, since = None, 0.0
    shaders = defaultdict(set)
    names = {}
    for line in open(log, encoding='utf-8', errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        t = seconds(m)
        if t < lo or t > hi:
            continue
        c = CAR.search(line)
        if c:
            if c.group(1) != car:
                car, since = c.group(1), t
            continue
        s = SHADER.search(line)
        if s and car and t - since >= 0.5:
            shaders[car].add(s.group(1))
            names[s.group(1)] = (s.group(2), 'ours' if s.group(3) or 'first drawn' in line and 'ours' in line else
                                 ('ours' if 'first drawn' in line else 'game'))
    with open(out, 'w', encoding='utf-8') as f:
        f.write('car\thash\tname\twho\n')
        for c in sorted(shaders):
            for h in sorted(shaders[c], key=lambda h: names[h][0]):
                f.write('%s\t%s\t%s\t%s\n' % (c, h, names[h][0], names[h][1]))
    by_shader = defaultdict(set)
    for c, hs in shaders.items():
        for h in hs:
            by_shader[h].add(c)
    print('%d cars, %d shaders' % (len(shaders), len(by_shader)))
    for h in sorted(by_shader, key=lambda h: names[h]):
        if names[h][1] == 'game':
            print('game\'s  %s %-36s %s' % (h, names[h][0], ' '.join(sorted(by_shader[h]))))


if __name__ == '__main__':
    main()
