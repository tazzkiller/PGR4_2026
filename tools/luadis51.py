# A Lua 5.1 bytecode disassembler for the game's UI scripts (dumped by pgr4_script_dump_dir):
# every function's constants and instructions, constants and upvalue names resolved.
#   py luadis51.py <script.lua>
import struct
import sys

OPS = ['MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL', 'GETTABLE', 'SETGLOBAL',
       'SETUPVAL', 'SETTABLE', 'NEWTABLE', 'SELF', 'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'UNM',
       'NOT', 'LEN', 'CONCAT', 'JMP', 'EQ', 'LT', 'LE', 'TEST', 'TESTSET', 'CALL', 'TAILCALL',
       'RETURN', 'FORLOOP', 'FORPREP', 'TFORLOOP', 'SETLIST', 'CLOSE', 'CLOSURE', 'VARARG']


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0
        assert data[:4] == b'\x1bLua' and data[4] == 0x51, 'not Lua 5.1 bytecode'
        self.e = '<' if data[6] == 1 else '>'
        self.int_size, self.size_t, self.ins_size, self.num_size = data[7:11]
        self.p = 12

    def byte(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def int(self):
        fmt = self.e + ('i' if self.int_size == 4 else 'q')
        v = struct.unpack_from(fmt, self.d, self.p)[0]
        self.p += self.int_size
        return v

    def sizet(self):
        fmt = self.e + ('I' if self.size_t == 4 else 'Q')
        v = struct.unpack_from(fmt, self.d, self.p)[0]
        self.p += self.size_t
        return v

    def string(self):
        n = self.sizet()
        if n == 0:
            return None
        s = self.d[self.p:self.p + n - 1].decode('latin-1')
        self.p += n
        return s

    def number(self):
        v = struct.unpack_from(self.e + 'd', self.d, self.p)[0]
        self.p += 8
        return v

    def function(self):
        f = {'source': self.string(), 'line': self.int(), 'last': self.int()}
        f['nups'], f['params'], f['vararg'], f['stack'] = self.byte(), self.byte(), self.byte(), self.byte()
        f['code'] = [struct.unpack_from(self.e + 'I', self.d, self.p + 4 * i)[0] for i in range(self.int())]
        self.p += 4 * len(f['code'])
        consts = []
        for _ in range(self.int()):
            t = self.byte()
            if t == 0:
                consts.append(None)
            elif t == 1:
                consts.append(bool(self.byte()))
            elif t == 3:
                consts.append(self.number())
            elif t == 4:
                consts.append(self.string())
            else:
                raise ValueError('constant type %d' % t)
        f['consts'] = consts
        f['protos'] = [self.function() for _ in range(self.int())]
        lines = self.int()
        self.p += lines * self.int_size
        locs = []
        for _ in range(self.int()):
            locs.append(self.string())
            self.int()
            self.int()
        f['locals'] = locs
        f['upvals'] = [self.string() for _ in range(self.int())]
        return f


def rk(f, v):
    if v >= 256:
        c = f['consts'][v - 256]
        return repr(c)
    return 'r%d' % v


def dump(f, name, depth=0):
    pad = '  ' * depth
    print(f'{pad}function {name} (line {f["line"]}, params {f["params"]}, upvalues {f["upvals"]})')
    for pc, i in enumerate(f['code']):
        op = i & 0x3F
        a = (i >> 6) & 0xFF
        c = (i >> 14) & 0x1FF
        b = (i >> 23) & 0x1FF
        bx = i >> 14
        sbx = bx - 131071
        o = OPS[op] if op < len(OPS) else '?%d' % op
        if o in ('LOADK',):
            arg = f'r{a} = {f["consts"][bx]!r}'
        elif o in ('GETGLOBAL', 'SETGLOBAL'):
            arg = f'r{a} {f["consts"][bx]}'
        elif o in ('GETTABLE', 'SELF'):
            arg = f'r{a} = r{b}[{rk(f, c)}]'
        elif o == 'SETTABLE':
            arg = f'r{a}[{rk(f, b)}] = {rk(f, c)}'
        elif o in ('ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'EQ', 'LT', 'LE'):
            arg = f'{a} {rk(f, b)} {rk(f, c)}'
        elif o in ('JMP', 'FORLOOP', 'FORPREP'):
            arg = f'r{a} -> {pc + 1 + sbx}'
        elif o in ('GETUPVAL', 'SETUPVAL'):
            up = f['upvals'][b] if b < len(f['upvals']) else b
            arg = f'r{a} upval {up}'
        elif o == 'CLOSURE':
            arg = f'r{a} = proto {bx}'
        else:
            arg = f'{a} {b} {c}'
        print(f'{pad}  {pc:4d} {o:9s} {arg}')
    for k, p in enumerate(f['protos']):
        dump(p, f'{name}/{k}', depth + 1)


r = Reader(open(sys.argv[1], 'rb').read())
dump(r.function(), 'main')
