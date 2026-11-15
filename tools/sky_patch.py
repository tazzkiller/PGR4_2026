# The sky (local change, pgr4 project): the sky's pixel shaders (psSky,
# psSkyFog) translated from the shader dump (work\shaders\<hash>.ps.txt) by ucode2hlsl.py, each
# read of their panorama (tf0) through XeSkyFetch (shaders/sky_hooks.hlsli), written to
# pgr4\shaders\<hash>.ps.hlsl.
#   py sky_patch.py
import os
import re
import subprocess
import sys

ROOT = r'D:\Apps\PGR4'
DUMP = os.path.join(ROOT, r'work\shaders')
OUT = os.path.join(ROOT, r'pgr4\shaders')
GEN = os.path.join(ROOT, r'pgr4\tools\ucode2hlsl.py')
SKY = {'25E956077772CC47': 'psSky', 'AA1B910E0D596BC3': 'psSky', '529E0E9E179149DB': 'psSkyFog',
       'A0120EC7D19963FD': 'psSkyFog', 'A7B4D6C2F3180089': 'psSkyFog'}
FETCH = re.compile(r'TEX2D(?:_LEVEL|_BIAS)?\(0, float2\(([^()]*)\)[^;]*?\)(?=;)')


def main():
    for h, n in SKY.items():
        tmp = os.path.join(OUT, h + '.gen.tmp')
        r = subprocess.run([sys.executable, GEN, os.path.join(DUMP, h + '.ps.txt'), tmp],
                           capture_output=True, text=True)
        if r.returncode:
            print(h, 'non traduit :', r.stderr.strip().split('\n')[-1])
            continue
        text = open(tmp).read()
        os.remove(tmp)
        text, count = FETCH.subn(lambda m: f'XeSkyFetch(float2({m.group(1)}))', text)
        if not count:
            print(h, 'pas de lecture du panorama')
            continue
        header = (f'// Sky: variant {h} of the game\'s {n} (sky_hooks.hlsli).\n'
                  '#include "sky_hooks.hlsli"\n')
        open(os.path.join(OUT, h + '.ps.hlsl'), 'w').write(header + text)
        print(h, n, 'lectures du panorama', count)


if __name__ == '__main__':
    main()
