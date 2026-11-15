# The HD textures packed into one compressed file (local tool, pgr4 project): every
# <hash>.dds, <hash>_n.dds and areas.tsv of a textures_hd folder, each compressed on its own with
# Windows' Compression API (Cabinet.dll, buffer mode) so that the game reads and decompresses
# only the ones it binds (textures.cpp: HdRead). Layout (little-endian): "PGR4HDP1", the entry
# count (u32), 0 (u32), per entry its hash (u64), offset (u64), compressed and file sizes (u32),
# kind (u32: 0 a texture, 1 its normal map, 2 areas.tsv) and algorithm (u32: 0 stored, 3 XPRESS,
# 4 XPRESS_HUFF, 5 LZMS), then the entries' data. An entry the algorithm does not shrink is stored.
#   py hd_pack.py <textures_hd dir> <out .pak> [xpress_huff|xpress|lzms]
import ctypes
import os
import struct
import sys
from ctypes import wintypes

ALGOS = {'xpress': 3, 'xpress_huff': 4, 'lzms': 5}
cab = ctypes.WinDLL('cabinet')
cab.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cab.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                         ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
cab.CloseCompressor.argtypes = [ctypes.c_void_p]


def compress(handle, data):
    out = ctypes.create_string_buffer(len(data) + len(data) // 8 + 65536)
    got = ctypes.c_size_t(0)
    if not cab.Compress(handle, data, len(data), out, len(out), ctypes.byref(got)):
        raise OSError(ctypes.get_last_error(), 'Compress')
    return out.raw[:got.value]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    algo = ALGOS[sys.argv[3] if len(sys.argv) > 3 else 'xpress_huff']
    names = []
    for name in sorted(os.listdir(src)):
        stem, ext = os.path.splitext(name)
        if name == 'areas.tsv':
            names.append((0, 2, name))
        elif ext == '.dds' and len(stem) in (16, 18) and (len(stem) == 16 or stem.endswith('_n')):
            names.append((int(stem[:16], 16), 1 if stem.endswith('_n') else 0, name))
    handle = ctypes.c_void_p()
    if not cab.CreateCompressor(algo, None, ctypes.byref(handle)):
        raise OSError(ctypes.get_last_error(), 'CreateCompressor')
    head = 16 + 32 * len(names)
    index, raw_total, packed_total = [], 0, 0
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as out:
        out.write(b'\0' * head)
        for i, (h, kind, name) in enumerate(names):
            data = open(os.path.join(src, name), 'rb').read()
            packed = compress(handle, data)
            entry_algo = algo
            if len(packed) >= len(data):
                packed, entry_algo = data, 0
            index.append(struct.pack('<QQIIII', h, out.tell(), len(packed), len(data), kind,
                                     entry_algo))
            out.write(packed)
            raw_total += len(data)
            packed_total += len(packed)
            if (i + 1) % 200 == 0 or i + 1 == len(names):
                print(f'{i + 1} / {len(names)}  {raw_total >> 20} Mo -> {packed_total >> 20} Mo',
                      flush=True)
        out.seek(0)
        out.write(b'PGR4HDP1' + struct.pack('<II', len(names), 0) + b''.join(index))
    cab.CloseCompressor(handle)
    os.replace(tmp, dst)
    print('ok', dst, f'{packed_total / max(raw_total, 1):.3f}')


if __name__ == '__main__':
    main()
