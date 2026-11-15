# The game's shaders by their original names (local tool, pgr4 project): the compiled shader
# objects in the decrypted executable (magic 0x102A1100 vertex / 0x102A1101 pixel; the header's
# second and third words: the data's offset and size from the magic; a debug path naming the
# source, e.g. c:\PGR4\...\psCarPaint0.updb) matched against the ucode the engine dumps
# (--pgr4_native_shader_dump_dir: <hash>.ps.ucode / .vs.ucode). The runtime ucode is the object's
# with words patched at load (constant and fetch slots), so a shader goes to the object sharing
# most of its 16-byte windows.
#   py xpu_names.py <basefile.exe> <dump dir> <out.tsv>
import collections
import os
import re
import struct
import sys

exe, dump, out = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(exe, 'rb').read()
objects = []
for m in re.finditer(rb'\x10\x2a\x11[\x00\x01]', d):
    s = m.start()
    magic, off, size = struct.unpack_from('>3I', d, s)
    if not 0 < size < 0x40000 or s + off + size > len(d):
        continue
    t = re.search(rb'c:[ -~]{5,300}?\x00', d[s:s + off])
    name = t.group(0)[:-1].decode().replace('\\', '/').split('/')[-1] if t else ''
    name = re.sub(r'\.updb$', '', name)
    objects.append((s, 'ps' if magic & 1 else 'vs', name, d[s + off:s + off + size]))

index = collections.defaultdict(set)
for n, (s, kind, name, data) in enumerate(objects):
    for k in range(0, len(data) - 16 + 1, 4):
        index[data[k:k + 16]].add(n)

rows = []
for f in sorted(os.listdir(dump)):
    m = re.match(r'([0-9A-F]{16})\.(ps|vs)\.ucode$', f)
    if not m:
        continue
    u = open(os.path.join(dump, f), 'rb').read()
    votes = collections.Counter()
    windows = 0
    for k in range(0, len(u) - 16 + 1, 4):
        windows += 1
        hits = index.get(u[k:k + 16])
        if hits and len(hits) < 8:  # common runs (shared code) vote for nothing
            for n in hits:
                votes[n] += 1
    best = votes.most_common(1)
    if best and best[0][1] * 3 >= windows:
        n, v = best[0]
        rows.append((m.group(1), m.group(2), objects[n][2], f'{v}/{windows}', hex(objects[n][0])))
    else:
        rows.append((m.group(1), m.group(2), '', f'{best[0][1] if best else 0}/{windows}', ''))

with open(out, 'w', encoding='utf-8') as o:
    o.write('hash\tstage\tname\twindows\tobject\n')
    for r in rows:
        o.write('\t'.join(r) + '\n')
named = sum(1 for r in rows if r[2])
print(f'{len(objects)} objects, {len(rows)} dumped shaders, {named} named')
