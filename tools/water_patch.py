# The water shaders with finer waves (local change, pgr4 project): a water pixel shader translated
# by ucode2hlsl.py, patched so that XeWaterBegin (shaders/water_hooks.hlsli) sets the waves' slope
# at the pixel first, and every read of the height map (tf6) goes through XeWaterHeight.
# Local change (17/11/2026): its body becomes XeWaterShade, with the sun's light (c7) times xe_sun,
# and a new Shade puts the sun on the fine waves or on the game's smooth water (kSunWaves).
# A file this already patched (its header) only gets the steps it lacks: it can be its own output.
#   py water_patch.py <hash> <generated .ps.hlsl> <output .ps.hlsl>
import re
import sys

name, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src).read()
header = (f'// Water: variant {name} of the game\'s water shader (water_hooks.hlsli).\n'
          '#include "water_hooks.hlsli"\n')
count = 0
if '#include "water_hooks.hlsli"' not in text:
    begin = re.search(r'\n  float4 xe_out = [^\n]*\n', text)
    assert begin, 'no xe_out'
    text = text[:begin.end()] + '  XeWaterBegin(i);\n' + text[begin.end():]
    fetch = re.compile(r'(TEX2D(?:_LEVEL)?\(6, float2\(([^()]*)\)[^;]*?\));')

    def wrap(m):
        global count
        count += 1
        return f'XeWaterHeight({m.group(1)}, float2({m.group(2)}));'

    text = header + fetch.sub(wrap, text)

if 'XeWaterShade' not in text:
    start = text.find('float4 Shade(XeInput i) {')
    assert start >= 0 and text.count('float4 Shade(XeInput i) {') == 1, 'no single Shade'
    end = text.find('\n}', start)
    body = text[start:end]
    assert body.count('return ') == 1 and 'return XeFinish(xe_out);' in body, 'Shade returns elsewhere'
    body = body.replace('float4 Shade(XeInput i) {', 'float4 XeWaterShade(XeInput i) {', 1)
    body = body.replace('  XeWaterBegin(i);\n', '  XeWaterBegin(i);\n  const float4 xe_c7 = c7 * xe_sun;\n', 1)
    body = re.sub(r'\bc7\.', 'xe_c7.', body)
    shade = '''
// Local change (17/11/2026): the sun on the fine waves or on the game's smooth water
// (water_hooks.hlsli kSunWaves). The output is affine in the sun's light (c7): what the sun adds is
// the body with it less the body without it, the scenery's reflection left out of both (it cancels
// out). kSunWaves 1: the body once, as before.
float4 Shade(XeInput i) {
  xe_sun = 1.0; xe_waves = 1.0; xe_scene = true;
  if (kSunWaves >= 1.0) return XeWaterShade(i);
  xe_sun = 0.0;
  float4 color = XeWaterShade(i);
  xe_scene = false;
  if (kSunWaves > 0.0) {
    xe_sun = 1.0;
    const float4 lit = XeWaterShade(i);
    xe_sun = 0.0;
    color.rgb += kSunWaves * (lit.rgb - XeWaterShade(i).rgb);
  }
  xe_waves = 0.0;
  xe_sun = 1.0;
  const float4 smooth_lit = XeWaterShade(i);
  xe_sun = 0.0;
  color.rgb += (1.0 - kSunWaves) * (smooth_lit.rgb - XeWaterShade(i).rgb);
  return color;
}'''
    text = text[:start] + body + '\n}' + shade + text[end + 2:]

# Local change (17/11/2026): the game's fog amount over the water (its distance and height fog,
# c11.y's line, before the exp2 of the fog's transmission) times kWaterFog (water_hooks.hlsli).
if 'kWaterFog' not in text:
    fog = re.compile(r'\{ float4 (t\d+) = \((XeMul\(\(-r\d\.[xyzw]{4}\), c11\.yyyy\) \+ r\d\.xxxx)\);')
    assert len(fog.findall(text)) == 1, 'no single fog amount'
    text = fog.sub(r'{ float4 \1 = (\2) * kWaterFog;', text)

open(dst, 'w').write(text)
print(name, 'lectures de la carte de hauteurs', count, 'soleil', 'XeWaterShade' in text)
