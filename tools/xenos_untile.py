# A texture of the .stream2 (its base level as in Xbox 360 memory: blocks in Xenos 2D tiling,
# 16-bit words big-endian) to the linear little-endian BC data the PC reads (local tool, pgr4
# project). The tiling is Xenia's XGAddress2DTiledOffset (texture_address.cc).
#   untile(data, width, height, block_bytes) -> bytes
import numpy as np


def tiled_offsets(width_blocks, height_blocks, bpb_log2):
    """Byte offset of each block (row-major, linear order) in the tiled data."""
    pitch = (width_blocks + 31) & ~31
    y, x = np.mgrid[0:height_blocks, 0:width_blocks].astype(np.int64)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def untile(data, width, height, block_bytes):
    bw, bh = max(1, width // 4), max(1, height // 4)
    bpb_log2 = 3 if block_bytes == 8 else 4
    offs = tiled_offsets(bw, bh, bpb_log2).reshape(-1)
    src = np.frombuffer(data, np.uint8)
    idx = offs[:, None] + np.arange(block_bytes)[None, :]
    idx = np.minimum(idx, len(src) - 1)
    out = src[idx].reshape(-1)
    # Big-endian 16-bit words (k_8in16) to the PC's little-endian.
    return out.reshape(-1, 2)[:, ::-1].reshape(-1).tobytes()
