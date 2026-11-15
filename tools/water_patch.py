# The water shaders with finer waves (local change, pgr4 project): a water pixel shader translated
# by ucode2hlsl.py, patched so that XeWaterBegin (shaders/water_hooks.hlsli) sets the waves' slope
# at the pixel first, and every read of the height map (tf6) goes through XeWaterHeight.
#   py water_patch.py <hash> <generated .ps.hlsl> <output .ps.hlsl>
import re
import sys

name, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
begin = re.search(r'\n  float4 xe_out = [^\n]*\n', text)
assert begin, 'no xe_out'
text = text[:begin.end()] + '  XeWaterBegin(i);\n' + text[begin.end():]
fetch = re.compile(r'(TEX2D(?:_LEVEL)?\(6, float2\(([^()]*)\)[^;]*?\));')
count = 0


def wrap(m):
    global count
    count += 1
    return f'XeWaterHeight({m.group(1)}, float2({m.group(2)}));'


text = fetch.sub(wrap, text)
header = (f'// Water: variant {name} of the game\'s water shader (water_hooks.hlsli).\n'
          '#include "water_hooks.hlsli"\n')
open(dst, 'w').write(header + text)
print(name, 'lectures de la carte de hauteurs', count)
