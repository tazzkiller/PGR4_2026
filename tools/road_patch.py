# The road shaders with relief (local change, pgr4 project): a road pixel shader translated by
# ucode2hlsl.py, patched to call XeRoadBump (shaders/road_hooks.hlsli) on the normal r0 just
# for the game's sun term dot(r0, c6) only, with the gravel's coordinates (the tf0 fetch) and the
# eye-to-pixel vector (r5 as interpolated).
#   py road_patch.py <generated .ps.hlsl> <output .ps.hlsl>
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
lines = open(src).read().split('\n')
out = []
pos_done = uv_done = bump_done = False
for line in lines:
    # The eye-to-pixel vector, before the shader overwrites r5.
    if not pos_done and re.match(r'\s+\{ float4 t1 = ', line):
        out.append('  float3 xe_pos = r5.xyz;')
        out.append('  float2 xe_gravel_uv = 0.0;')
        pos_done = True
    if not bump_done and 'XeDp3(r0.zxyy, c6.zxyy)' in line:
        # Only the sun's diffuse term gets the relief: the game's specular with a bumpy normal
        # sparkled on every stone (specular aliasing), so the normal is put back after it.
        out.append('  float3 xe_n0 = r0.xyz;')
        out.append('  r0.xyz = XeRoadBump(r0.xyz, xe_gravel_uv, xe_pos);')
        out.append(line)
        out.append('  r0.xyz = xe_n0;')
        bump_done = True
        continue
    out.append(line)
    m = re.search(r'TEX2D(?:_LEVEL)?\(0, float2\(([^)]*)\)', line)
    if m and not uv_done:
        # Saved after the fetch's line: its operands are read before any of its writes.
        out.insert(len(out) - 1, f'  xe_gravel_uv = float2({m.group(1)});')
        uv_done = True
text = '\n'.join(out)
header = '#include "road_hooks.hlsli"\n'
open(dst, 'w').write(header + text)
print(src, 'position', pos_done, 'gravier', uv_done, 'soleil', bump_done)
