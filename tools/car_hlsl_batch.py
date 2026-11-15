# The cars' pixel shaders still without our HLSL, made from their dumped ucode (local tool, pgr4 project), by the
# rules the existing ones follow (14/11/2026):
#   - paints (psCarPaint*): car_paint_hooks.hlsli; a read of the rain's drops (XeStacked13) goes through XeDrops13
#     with PGR4_DROPS_RUN;
#   - the other parts reading the surroundings' cube map (glass, chrome, alloys, badges...): car_reflect_hooks.hlsli;
#     a glass's read of the drops (XeStacked7) through XeDrops7 with PGR4_DROPS_GLIDE; a windscreen's wiper
#     (PGR4_WIPER by day, PGR4_WIPER_NIGHT at night);
#   - a shader reading neither the cube map nor the drops (the 320x180 pass's small paints, the carbon fibre) is left
#     to the game: nothing of ours to add.
#   py car_hlsl_batch.py <shader_names.tsv> <dump dir> <shaders dir> [--write]
import os
import re
import subprocess
import sys

FAMILIES = re.compile(r'psCar(Paint|Glass(Windscreen|Window|Exterior)|Chrome|Alloy|Carbon|Badge)')


def main():
    names, dump, out = sys.argv[1], sys.argv[2], sys.argv[3]
    write = '--write' in sys.argv
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ucode2hlsl.py')
    tmp = os.path.join(os.environ.get('TEMP', '.'), 'car_hlsl_batch.hlsl')
    made, skipped = [], []
    for line in open(names, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 3 or f[1] != 'ps' or not FAMILIES.match(f[2]):
            continue
        h, name = f[0], f[2]
        if os.path.exists(os.path.join(out, h + '.ps.hlsl')):
            continue
        src = os.path.join(dump, h + '.ps.txt')
        if not os.path.exists(src):
            skipped.append((h, name, 'no dump'))
            continue
        subprocess.run([sys.executable, tool, src, tmp], check=True, capture_output=True)
        body = open(tmp, encoding='utf-8').read()
        code = body[body.find('float4 Shade('):]
        # Ours to add where it reads the surroundings' cube map or the rain's drops (the night bonnets read no cube
        # map but the drops).
        drops = re.search(r'= XeStacked(13|7)\(float3\(', code)
        if 'XeEnv(' not in code and not drops:
            skipped.append((h, name, 'neither cube map nor drops'))
            continue
        defines = []
        if name.startswith('psCarPaint'):
            header = 'car_paint_hooks.hlsli'
            what = "Car body paint: variant %s of the game's body shader (car_paint_hooks.hlsli)." % h
            if re.search(r'= XeStacked13\(float3\(', code):
                defines.append('PGR4_DROPS_RUN')
                body = re.sub(r'= XeStacked13\(float3\(', '= XeDrops13(i, float3(', body)
        else:
            header = 'car_reflect_hooks.hlsli'
            what = "Car part reading the surroundings' cube map: variant %s of the game's shader (car_reflect_hooks.hlsli)." % h
            if name.startswith('psCarGlass') and re.search(r'= XeStacked7\(float3\(', code):
                defines.append('PGR4_DROPS_GLIDE')
                body = re.sub(r'= XeStacked7\(float3\(', '= XeDrops7(i, float3(', body)
                if name.startswith('psCarGlassWindscreen'):
                    defines.append('PGR4_WIPER_NIGHT' if 'Night' in name else 'PGR4_WIPER')
        left = sorted(set(re.findall(r'= (XeStacked\d+)\(', body[body.find('float4 Shade('):])))
        text = '// ' + what + '\n// ' + name + ', made by tools/car_hlsl_batch.py (14/11/2026).\n'
        text += ''.join('#define %s\n' % d for d in defines) + '#include "%s"\n' % header + body
        made.append((h, name, ' '.join(defines) or '-', ' '.join(left) or '-'))
        if write:
            open(os.path.join(out, h + '.ps.hlsl'), 'w', encoding='utf-8', newline='\n').write(text)
    for m in made:
        print('MADE   %s %-36s %-30s other stacked: %s' % m)
    for s in skipped:
        print('LEFT   %s %-36s %s' % s)
    print('%d made, %d left%s' % (len(made), len(skipped), '' if write else ' (dry run)'))


if __name__ == '__main__':
    main()
