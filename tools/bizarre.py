# Bizarre Creations' LZSS (local tool, pgr4 project), after QuickBMS's old_bizarre (Luigi Auriemma):
# per 32-bit big-endian flags word, 30 items: a set top bit copies `amount` bytes from `back`
# behind (a 16-bit big-endian pair: back = t & (0x3FFF >> (flags & 3)) + 1, amount =
# t >> (14 - (flags & 3)) + 3), a clear one a literal byte. Optionally a 4-byte little-endian
# output size first.
import struct


def decompress(data, out_size=None, sized=True, max_out=1 << 26):
    i = 0
    if sized:
        out_size = struct.unpack_from('<I', data, 0)[0]
        i = 4
    limit = out_size if out_size else max_out
    out = bytearray()
    n = len(data)
    while i + 4 <= n and len(out) < limit:
        flags = struct.unpack_from('>i', data, i)[0]
        i += 4
        shift = 0xE - (flags & 3)
        mask = 0x3FFF >> (flags & 3)
        for _ in range(0x1E):
            if flags < 0:
                if i + 2 > n:
                    return bytes(out), i
                t = (data[i] << 8) | data[i + 1]
                i += 2
                back = (t & mask) + 1
                amount = (t >> shift) + 3
                if back > len(out) or len(out) + amount > limit:
                    return bytes(out), i
                for _ in range(amount):
                    out.append(out[-back])
            else:
                if i >= n or len(out) >= limit:
                    return bytes(out), i
                out.append(data[i])
                i += 1
            flags = (flags << 1) & 0xFFFFFFFF
            if flags & 0x80000000:
                flags -= 1 << 32
    return bytes(out), i
