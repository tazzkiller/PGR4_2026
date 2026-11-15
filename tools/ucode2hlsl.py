# Xenos pixel shader ucode (Xenia's disassembly, as pgr4_native_shader_dump_dir writes it:
# <hash>.ps.txt) to the HLSL body of a replacement shader (pgr4-native shader_replace.h):
# `float4 Shade(XeInput i)` computing what the game's shader computes, instruction by
# instruction, with the Xenos ALU's rules:
# - a vector op and its co-issued scalar op read their sources before either writes;
# - mul, mad and dp* are "legacy": 0 times anything (inf, NaN) is 0;
# - scalar ops set the previous scalar result (ps), which *_prev ops read; retain_prev keeps it;
# - fetch destinations take the fetched components their swizzle names (r2.w___: r2.x = .w).
# Hooks, defined by the including file before this body (default: the game's): XeEnv(tf, dir,
# r), the cube map fetch; XeFinish(color, r) the color the shader writes (r: the registers).
#   py ucode2hlsl.py <hash>.ps.txt <out.hlsl>
import re
import sys

VECTOR = {'add', 'mul', 'mad', 'max', 'min', 'dp3', 'dp4', 'dp2add', 'sgt', 'sge', 'seq', 'sne', 'cndeq',
          'cndgt', 'cndge', 'frc', 'trunc', 'floor', 'cube', 'max4'}
SCALAR = {'adds', 'adds_prev', 'muls', 'muls_prev', 'maxs', 'mins', 'seqs', 'sgts', 'sges',
          'snes', 'frcs', 'truncs', 'floors', 'exp', 'log', 'logc', 'rcp', 'rcpc', 'rcpf', 'rsq',
          'rsqc', 'rsqf', 'sqrt', 'sin', 'cos', 'subs', 'subs_prev', 'mulsc', 'addsc', 'subsc',
          'setp_eq', 'setp_ne', 'setp_gt', 'setp_ge', 'retain_prev', 'setp_inv', 'setp_pop'}
# Vector ops that set the predicate (Xenia's interpreter): p0 = a.w == 0 && b.w <cmp> 0, the
# result x (replicated) = a.x == 0 && b.x <cmp> 0 ? 0 : a.x + 1.
PUSH = {'setp_eq_push': '==', 'setp_ne_push': '!=', 'setp_gt_push': '>', 'setp_ge_push': '>='}
COMP = 'xyzw'


def operand(tok):
    """A source: (neg, abs, 'r'/'c', index, swizzle 4 letters)."""
    neg = tok.startswith('-')
    tok = tok.lstrip('-')
    m = re.match(r'r_abs\[(\d+)\](?:\.(\w+))?$', tok)
    if m:
        return neg, True, 'r', int(m.group(1)), (m.group(2) or 'xyzw')
    m = re.match(r'([rc])(\d+)(?:\.(\w+))?$', tok)
    if not m:
        raise ValueError('operand ' + tok)
    return neg, False, m.group(1), int(m.group(2)), (m.group(3) or 'xyzw')


def reg(kind, index):
    return f'r{index}' if kind == 'r' else f'c{index}'


def src4(op):
    """The operand as a float4 expression (swizzle padded with its last letter)."""
    neg, ab, kind, index, sw = op
    sw = (sw + sw[-1] * 4)[:4]
    e = f'{reg(kind, index)}.{sw}'
    if ab:
        e = f'abs({e})'
    return f'(-{e})' if neg else e


def src1(op, k=0):
    """Component k of the operand's swizzle as a scalar expression."""
    neg, ab, kind, index, sw = op
    e = f'{reg(kind, index)}.{sw[min(k, len(sw) - 1)]}'
    if ab:
        e = f'abs({e})'
    return f'(-{e})' if neg else e


def dest(tok):
    """(register expression, per-component target letters or '_' / '0' / '1')."""
    m = re.match(r'(oC\d|r\d+)(?:\.([\w_]+))?$', tok)
    if not m:
        raise ValueError('dest ' + tok)
    name = m.group(1)
    mask = m.group(2) or 'xyzw'
    if name.startswith('oC'):
        name = 'xe_out'
    if mask == '_':
        mask = '____'
    mask = (mask + '____')[:4]
    return name, mask


class Gen:
    def __init__(self):
        self.out = []
        self.indent = 1
        self.cexec_open = False
        self.labels = []
        # Forward jumps (jmp, (!p0) jmp): xe_jump holds the label jumped to, 0 when running; the
        # instructions run only while it is 0, a label clears it when reached.
        self.label_ids = {}
        self.jumps = False
        self.exec_pred = None  # the predicate of a (p0) / (!p0) exec block
        self.tmp = 0

    def emit(self, s):
        self.out.append('  ' * self.indent + s)

    def new_tmp(self):
        self.tmp += 1
        return f't{self.tmp}'

    def close_cexec(self):
        if self.cexec_open:
            self.indent -= 1
            self.emit('}')
            self.cexec_open = False

    # The vector op's float4 result (before the write mask), from parsed operands.
    def vector_value(self, op, ops):
        a = [src4(o) for o in ops]
        if op == 'add':
            return f'({a[0]} + {a[1]})'
        if op == 'mul':
            return f'XeMul({a[0]}, {a[1]})'
        if op == 'mad':
            return f'(XeMul({a[0]}, {a[1]}) + {a[2]})'
        if op == 'max':
            return f'max({a[0]}, {a[1]})'
        if op == 'min':
            return f'min({a[0]}, {a[1]})'
        if op == 'dp3':
            return f'XeDp3({a[0]}, {a[1]}).xxxx'
        if op == 'dp4':
            return f'XeDp4({a[0]}, {a[1]}).xxxx'
        if op == 'dp2add':
            # a.x b.x + a.y b.y + c.x (the water's normals, from their slopes).
            return f'(XeMul({a[0]}, {a[1]}).x + XeMul({a[0]}, {a[1]}).y + {a[2]}.x).xxxx'
        if op == 'sgt':
            return f'float4({a[0]} > {a[1]})'
        if op == 'sge':
            return f'float4({a[0]} >= {a[1]})'
        if op == 'seq':
            return f'float4({a[0]} == {a[1]})'
        if op == 'sne':
            return f'float4({a[0]} != {a[1]})'
        if op == 'cndeq':
            return f'XeSelect({a[0]} == 0.0, {a[1]}, {a[2]})'
        if op == 'cndgt':
            return f'XeSelect({a[0]} > 0.0, {a[1]}, {a[2]})'
        if op == 'cndge':
            return f'XeSelect({a[0]} >= 0.0, {a[1]}, {a[2]})'
        if op == 'frc':
            return f'frac({a[0]})'
        if op == 'trunc':
            return f'trunc({a[0]})'
        if op == 'floor':
            return f'floor({a[0]})'
        if op == 'cube':
            # cube dst, v.zzxy, v.yxzz: the canonical form, of v = (src1.y, src1.x, src1.z).
            s1 = ops[1]
            v = f'float3({src1(s1, 1)}, {src1(s1, 0)}, {src1(s1, 2)})'
            return f'XeCube({v})'
        raise ValueError('vector op ' + op)

    def scalar_value(self, op, ops):
        base = op.replace('_sat', '')
        if base == 'retain_prev':
            return None
        if base in ('mulsc', 'addsc', 'subsc'):
            a, b = src1(ops[0]), src1(ops[1])
            return {'mulsc': f'XeMul1({a}, {b})', 'addsc': f'({a} + {b})',
                    'subsc': f'({a} - {b})'}[base]
        o = ops[0]
        a = src1(o, 0)
        b = src1(o, 1)
        if base == 'maxs':
            return f'max({a}, {b})'
        if base == 'mins':
            return f'min({a}, {b})'
        if base == 'adds':
            return f'({a} + {b})'
        if base == 'subs':
            return f'({a} - {b})'
        if base == 'muls':
            return f'XeMul1({a}, {b})'
        if base == 'adds_prev':
            return f'({a} + ps)'
        if base == 'subs_prev':
            return f'({a} - ps)'
        if base == 'muls_prev':
            return f'XeMul1({a}, ps)'
        if base == 'sgts':
            return f'float({a} > 0.0)'
        if base == 'sges':
            return f'float({a} >= 0.0)'
        if base == 'seqs':
            return f'float({a} == 0.0)'
        if base == 'snes':
            return f'float({a} != 0.0)'
        if base == 'frcs':
            return f'frac({a})'
        if base == 'truncs':
            return f'trunc({a})'
        if base == 'floors':
            return f'floor({a})'
        if base in ('exp',):
            return f'exp2({a})'
        if base in ('log', 'logc'):
            return f'XeLog({a})'
        if base in ('rcp', 'rcpc', 'rcpf'):
            return f'XeRcp({a})'
        if base in ('rsq', 'rsqc', 'rsqf'):
            return f'XeRsq({a})'
        if base == 'sqrt':
            return f'sqrt({a})'
        if base == 'sin':
            return f'sin({a})'
        if base == 'cos':
            return f'cos({a})'
        if base == 'setp_inv':
            return ('setp_inv', a)
        if base == 'setp_pop':
            return ('setp_pop', a)
        if base.startswith('setp_'):
            cmp = {'setp_eq': '==', 'setp_ne': '!=', 'setp_gt': '>', 'setp_ge': '>='}[base]
            return ('setp', f'({a} {cmp} 0.0)')
        raise ValueError('scalar op ' + op)

    def write(self, name, mask, value_expr, sat, scalar=False):
        lines = []
        for k, m in enumerate(mask):
            if m == '_':
                continue
            if m in '01':
                lines.append(f'{name}.{COMP[k]} = {m}.0;')
                continue
            comp = '' if scalar else f'.{COMP[k]}'
            v = f'{value_expr}{comp}'
            lines.append(f'{name}.{COMP[k]} = {"saturate(" + v + ")" if sat else v};')
        return lines

    def alu(self, vec_line, sca_line, pred):
        """One ALU instruction: the vector part (or None) and the scalar part (or None)."""
        writes = []
        pre = []
        if vec_line:
            w = vec_line.replace(',', ' ').split()
            op = w[0]
            sat = op.endswith('_sat')
            base = op.replace('_sat', '')
            if base in SCALAR:  # a scalar-only instruction
                sca_line, vec_line = vec_line, None
            elif base in PUSH:
                name, mask = dest(w[1])
                ops = [operand(t) for t in w[2:]]
                a, b = src4(ops[0]), src4(ops[1])
                cmp = PUSH[base]
                t = self.new_tmp()
                pre.append(f'float4 {t} = (({a}).x == 0.0 && ({b}).x {cmp} 0.0) ? 0.0 : '
                           f'({a}).x + 1.0;')
                writes += self.write(name, mask, t, sat)
                writes.append(f'p0 = ({a}).w == 0.0 && ({b}).w {cmp} 0.0;')
            else:
                name, mask = dest(w[1])
                ops = [operand(t) for t in w[2:]]
                t = self.new_tmp()
                pre.append(f'float4 {t} = {self.vector_value(base, ops)};')
                writes += self.write(name, mask, t, sat)
        set_ps = None
        if sca_line:
            w = sca_line.replace(',', ' ').split()
            op = w[0]
            sat = op.endswith('_sat')
            if op != 'retain_prev':
                name, mask = dest(w[1])
                ops = [operand(t) for t in w[2:]]
                v = self.scalar_value(op, ops)
                if isinstance(v, tuple) and v[0] == 'setp_pop':
                    # ps (and the dest) = a - 1, p0 = it <= 0 (then ps 0), as Xenia's translators.
                    t, r = self.new_tmp(), self.new_tmp()
                    pre.append(f'float {t} = {v[1]} - 1.0;')
                    pre.append(f'float {r} = {t} <= 0.0 ? 0.0 : {t};')
                    writes.append(f'p0 = {t} <= 0.0;')
                    writes += self.write(name, mask, r, False, scalar=True)
                    writes.append(f'ps = {r};')
                elif isinstance(v, tuple) and v[0] == 'setp_inv':
                    # p0 = a == 1; ps (and the dest) = p0 ? 0 : (a == 0 ? 1 : a).
                    t, r = self.new_tmp(), self.new_tmp()
                    pre.append(f'float {t} = {v[1]};')
                    pre.append(f'float {r} = {t} == 1.0 ? 0.0 : ({t} == 0.0 ? 1.0 : {t});')
                    writes.append(f'p0 = {t} == 1.0;')
                    writes += self.write(name, mask, r, False, scalar=True)
                    writes.append(f'ps = {r};')
                elif isinstance(v, tuple):  # setp: the predicate and ps (and the dest)
                    t, r = self.new_tmp(), self.new_tmp()
                    pre.append(f'bool {t} = {v[1]};')
                    pre.append(f'float {r} = {t} ? 0.0 : 1.0;')
                    writes.append(f'p0 = {t};')
                    writes += self.write(name, mask, r, False, scalar=True)
                    writes.append(f'ps = {r};')
                elif v is not None:
                    t = self.new_tmp()
                    pre.append(f'float {t} = {"saturate(" + v + ")" if sat else v};')
                    writes += self.write(name, mask, t, False, scalar=True)
                    set_ps = t
        if set_ps:
            writes.append(f'ps = {set_ps};')
        body = pre + writes
        if not body:
            return
        self.emit_guarded('{ ' + ' '.join(body) + ' }', pred)

    def guard(self, pred):
        conds = []
        if self.jumps:
            conds.append('xe_jump == 0')
        if self.exec_pred:
            conds.append(self.exec_pred)
        if pred:
            conds.append(pred)
        return ' && '.join(conds)

    def emit_guarded(self, stmt, pred):
        g = self.guard(pred)
        if g:
            self.emit(f'if ({g}) {{')
            self.indent += 1
        self.emit(stmt)
        if g:
            self.indent -= 1
            self.emit('}')

    def fetch(self, line, pred):
        w = [x for x in line.replace(',', ' ').split()]
        kind = w[0]
        name, mask = dest(w[1]) if '.' in w[1] else (w[1], 'xyzw')
        coord = operand(w[2])
        tf = int(w[3][2:])
        attrs = dict(a.split('=') for a in w[4:] if '=' in a)
        t = self.new_tmp()
        c = coord
        if kind == 'tfetch2D':
            uv = f'float2({src1(c, 0)}, {src1(c, 1)})'
            # The register's LOD is the level itself only without the computed one; with it (the
            # default), it is a bias over it (as Xenia's translators do): as a level, the textures
            # were read at their sharpest at any distance, and swarmed.
            if attrs.get('UseRegisterLOD') == 'true' and attrs.get('UseComputedLOD') == 'false':
                s = f'TEX2D_LEVEL({tf}, {uv}, xe_lod)'
            elif attrs.get('UseRegisterLOD') == 'true':
                s = f'TEX2D_BIAS({tf}, {uv}, xe_lod)'
            elif attrs.get('UseComputedLOD') == 'false':
                s = f'TEX2D_LEVEL({tf}, {uv}, 0.0)'
            else:
                s = f'TEX2D({tf}, {uv})'
        elif kind == 'tfetch1D':
            # A 1D texture is bound as a 2D one of height 1 (the water's Fresnel table).
            uv = f'float2({src1(c, 0)}, 0.5)'
            if attrs.get('UseComputedLOD') == 'false':
                s = f'TEX2D_LEVEL({tf}, {uv}, 0.0)'
            else:
                s = f'TEX2D({tf}, {uv})'
        elif kind == 'tfetch3D':
            uvw = f'float3({src1(c, 0)}, {src1(c, 1)}, {src1(c, 2)})'
            s = f'XeStacked{tf}({uvw})'
        elif kind == 'tfetchCube':
            stf = f'float3({src1(c, 0)}, {src1(c, 1)}, {src1(c, 2)})'
            s = f'XeEnv({tf}, XeCubeDirection({stf}))'
        else:
            raise ValueError(kind)
        body = [f'float4 {t} = {s};']
        if name == 'xe_out':
            raise ValueError('fetch to oC')
        for k, m in enumerate(mask):
            if m == '_':
                continue
            if m in '01':
                body.append(f'{name}.{COMP[k]} = {m}.0;')
            else:
                body.append(f'{name}.{COMP[k]} = {t}.{m};')
        self.emit_guarded('{ ' + ' '.join(body) + ' }', pred)

    def run(self, text):
        lines = []
        for raw in text.split('\n'):
            l = re.sub(r'/\*.*?\*/', '', raw)
            l = l.split('//')[0].strip()
            if l:
                lines.append(l)
        i = 0
        tfs = set()
        while i < len(lines):
            l = lines[i]
            w = l.split()
            op = w[0]
            if op in ('exec', 'exece', 'alloc', 'cnop', 'serialize'):
                if op in ('exec', 'exece'):
                    self.close_cexec()
                    self.exec_pred = None
                i += 1
                continue
            # A predicated exec block, and forward jumps.
            if op in ('(p0)', '(!p0)') and len(w) > 1 and w[1] in ('exec', 'exece'):
                self.close_cexec()
                self.exec_pred = 'p0' if op == '(p0)' else '!p0'
                i += 1
                continue
            if op == 'jmp' or (op in ('(p0)', '(!p0)') and len(w) > 1 and w[1] == 'jmp'):
                self.close_cexec()
                self.exec_pred = None
                label = w[-1]
                lid = self.label_ids.setdefault(label, len(self.label_ids) + 1)
                self.jumps = True
                cond = '' if op == 'jmp' else (' && p0' if op == '(p0)' else ' && !p0')
                self.emit(f'if (xe_jump == 0{cond}) xe_jump = {lid};  // jmp {label}')
                i += 1
                continue
            if op == 'label' and w[1] in self.label_ids:
                self.close_cexec()
                self.exec_pred = None
                self.emit(f'if (xe_jump == {self.label_ids[w[1]]}) xe_jump = 0;  // {w[1]}')
                i += 1
                continue
            if op in ('cexec', 'cexece'):
                self.close_cexec()
                b = w[1]
                neg = b.startswith('!')
                n = int(b.lstrip('!b'))
                self.emit(f'if ({"!" if neg else ""}XE_BOOL({n})) {{')
                self.indent += 1
                self.cexec_open = True
                i += 1
                continue
            if op == 'cjmp':
                self.close_cexec()
                b = w[1].rstrip(',')
                label = w[2]
                neg = b.startswith('!')
                n = int(b.lstrip('!b'))
                # Jumps forward to label when the condition holds: run what follows otherwise.
                self.emit(f'if ({"" if neg else "!"}XE_BOOL({n})) {{  // to {label}')
                self.indent += 1
                self.labels.append(label)
                i += 1
                continue
            if op == 'label':
                self.close_cexec()
                if self.labels and self.labels[-1] == w[1]:
                    self.labels.pop()
                    self.indent -= 1
                    self.emit('}')
                i += 1
                continue
            pred = None
            if op.startswith('('):
                pred = {'(p0)': 'p0', '(!p0)': '!p0'}[op]
                l = l[len(op):].strip()
                w = l.split()
                op = w[0]
            if op.startswith('tfetch'):
                tfs.add(int(re.search(r'tf(\d+)', l).group(1)))
                self.fetch(l, pred)
                i += 1
                continue
            if op == 'setTexLOD':
                self.emit_guarded(f'xe_lod = {src1(operand(w[1]))};', pred)
                i += 1
                continue
            sca = None
            if i + 1 < len(lines) and lines[i + 1].startswith('+'):
                sca = lines[i + 1][1:].strip()
                # Its predicate is the vector part's (one predicate per ALU instruction).
                sca = re.sub(r'^\(!?p0\)\s*', '', sca)
                i += 1
            self.alu(l, sca, pred)
            i += 1
        self.close_cexec()
        while self.labels:
            self.labels.pop()
            self.indent -= 1
            self.emit('}')
        return tfs


HEADER = '''// Generated by pgr4/tools/ucode2hlsl.py from the game's pixel shader {name} (Xenos ucode):
// the same computation in HLSL, for shader_replace.h. Hooks (define before including this):
// XeEnv(tf, dir), XeFinish(color, r0..r7 at the output).
#ifndef XE_UCODE_HELPERS
#define XE_UCODE_HELPERS
float4 XeMul(float4 a, float4 b) {{ return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }}
float XeMul1(float a, float b) {{ return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }}
float XeDp3(float4 a, float4 b) {{ float4 m = XeMul(a, b); return m.x + m.y + m.z; }}
float XeDp4(float4 a, float4 b) {{ float4 m = XeMul(a, b); return m.x + m.y + m.z + m.w; }}
float4 XeSelect(bool4 c, float4 a, float4 b) {{ return c ? a : b; }}
float XeRcp(float a) {{ return a == 1.0 ? 1.0 : 1.0 / a; }}
float XeRsq(float a) {{ return a == 1.0 ? 1.0 : rsqrt(a); }}
float XeLog(float a) {{ return a == 1.0 ? 0.0 : log2(a); }}
// cube: (tc, sc, 2 ma, face) of v, as Direct3D's cube faces.
float4 XeCube(float3 v) {{
  float3 a = abs(v);
  if (a.z >= a.x && a.z >= a.y) return float4(-v.y, v.z < 0.0 ? -v.x : v.x, 2.0 * v.z, v.z < 0.0 ? 5.0 : 4.0);
  if (a.y >= a.x) return float4(v.y < 0.0 ? -v.z : v.z, v.x, 2.0 * v.y, v.y < 0.0 ? 3.0 : 2.0);
  return float4(-v.y, v.x < 0.0 ? v.z : -v.z, 2.0 * v.x, v.x < 0.0 ? 1.0 : 0.0);
}}
// tfetchCube's (s, t, face), s and t in [1, 2] (cube's sc and tc / |2 ma| + 1.5): the direction.
float3 XeCubeDirection(float3 stf) {{
  float sc = (stf.x - 1.5) * 2.0, tc = (stf.y - 1.5) * 2.0;
  int f = int(stf.z + 0.5);
  if (f == 0) return float3(1.0, -tc, -sc);
  if (f == 1) return float3(-1.0, -tc, sc);
  if (f == 2) return float3(sc, 1.0, tc);
  if (f == 3) return float3(sc, -1.0, -tc);
  if (f == 4) return float3(sc, -tc, 1.0);
  return float3(-sc, -tc, -1.0);
}}
#endif
#ifndef XeEnv
#define XeEnv(tf, dir) TEXCUBE(tf, dir)
#endif
#ifndef XeFinish
#define XeFinish(color) (color)
#endif
'''


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding='latin-1').read()
    name = re.search(r'([0-9A-F]{16})', src).group(1)
    g = Gen()
    tfs = g.run(text)
    used_r = sorted(set(int(x) for x in re.findall(r'\br(\d+)\b', '\n'.join(g.out))))
    used_c = sorted(set(int(x) for x in re.findall(r'\bc(\d+)\b', '\n'.join(g.out))))
    out = [HEADER.format(name=name)]
    for c in used_c:
        out.append(f'#ifndef c{c}\n#define c{c} float4(0.0, 0.0, 0.0, 0.0)\n#endif')
    for tf in sorted(tfs):
        out.append(f'''#ifdef XE_HAS_STACKED{tf}
float4 XeStacked{tf}(float3 uvw) {{
#ifdef XeStackedUv
  uvw = XeStackedUv({tf}, uvw);  // a hook's change of the coordinates
#endif
  // A 3D texture (the fetch constant's dimension), or a stacked one's layers by w, filtered
  // between the two nearest.
  if (XE_IS_3D_TF{tf}) return XE_TEXTURE3D_TF{tf}.Sample(XE_SAMPLER_TF{tf}, uvw);
  float width, height, layers;
  XE_STACKED_TF{tf}.GetDimensions(width, height, layers);
  float l = clamp(uvw.z * layers - 0.5, 0.0, layers - 1.0);
  float l0 = floor(l);
  float4 a = XE_STACKED_TF{tf}.Sample(XE_SAMPLER_TF{tf}, float3(uvw.xy, l0));
  float4 b = XE_STACKED_TF{tf}.Sample(XE_SAMPLER_TF{tf}, float3(uvw.xy, min(l0 + 1.0, layers - 1.0)));
  return lerp(a, b, l - l0);
}}
#else
float4 XeStacked{tf}(float3 uvw) {{ return 0.0; }}
#endif''')
    out.append('float4 Shade(XeInput i) {')
    out.append('  float ps = 0.0, xe_lod = 0.0;')
    out.append('  bool p0 = false;')
    out.append('  int xe_jump = 0;')
    out.append('  float4 xe_out = float4(0.0, 0.0, 0.0, 0.0);')
    for r in used_r:
        if r < 16:
            out.append(f'  float4 r{r} = 0.0;')
            out.append(f'#if XE_INTERPOLATORS > {r}\n  r{r} = i.r{r};\n#endif')
        else:
            out.append(f'  float4 r{r} = 0.0;')
    out += g.out
    out.append('  return XeFinish(xe_out);')
    out.append('}')
    open(dst, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print(f'{name}: {len(g.out)} lines, textures {sorted(tfs)}')


main()
