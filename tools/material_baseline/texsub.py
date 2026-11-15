"""Reference for the "texture-subresources-v1" source identity (src/materials/texture_subresources_v1.hpp).

levels: list of (slice, mip, tightly packed bytes) in stream order.
"""
import hashlib
import struct

BLOCK = {71: 8, 72: 8, 80: 8, 81: 8, 74: 16, 75: 16, 77: 16, 78: 16, 83: 16, 84: 16, 98: 16, 99: 16}
TEXEL = {28: 4, 29: 4, 87: 4, 88: 4, 91: 4, 93: 4}


def digest(resource_format, view_format, width, height, first_mip, mip_count, first_array_slice, array_size, levels):
    h = hashlib.sha256(b"NA-TEXSUB-1\0")
    h.update(struct.pack("<8I", resource_format, view_format, width, height, first_mip, mip_count, first_array_slice, array_size))
    expected = [(first_array_slice+s, first_mip+m) for s in range(array_size) for m in range(mip_count)]
    if [(s, m) for s, m, _ in levels] != expected:
        raise ValueError("levels missing or out of order")
    for slice_, mip, data in levels:
        w, hh = max(1, width >> mip), max(1, height >> mip)
        if view_format in BLOCK:
            row_bytes, rows = (w+3)//4*BLOCK[view_format], (hh+3)//4
        elif view_format in TEXEL:
            row_bytes, rows = w*TEXEL[view_format], hh
        else:
            raise ValueError("format has no v1 identity")
        if len(data) != row_bytes*rows:
            raise ValueError("level bytes are not tightly packed")
        h.update(struct.pack("<6I", slice_, mip, w, hh, row_bytes, rows))
        h.update(data)
    return h.hexdigest()


if __name__ == "__main__":
    seq = lambda a, n: bytes(range(a, a+n))
    print("rgba8_2x2_two_mips  ", digest(28, 28, 2, 2, 0, 2, 0, 1, [(0, 0, seq(0, 16)), (0, 1, seq(16, 4))]))
    print("same_read_as_srgb   ", digest(28, 29, 2, 2, 0, 2, 0, 1, [(0, 0, seq(0, 16)), (0, 1, seq(16, 4))]))
    print("bc1_srgb_8x4_one_mip", digest(70, 72, 8, 4, 0, 1, 0, 1, [(0, 0, seq(0, 16))]))
    print("rgba8_4x4_mip1_only ", digest(28, 28, 4, 4, 1, 1, 0, 1, [(0, 1, seq(0, 16))]))
    print("rgba8_array_3_4     ", digest(28, 28, 2, 2, 0, 2, 3, 2, [(3, 0, seq(0, 16)), (3, 1, seq(16, 4)), (4, 0, seq(0, 16)), (4, 1, seq(16, 4))]))
    print("sha256_abc          ", hashlib.sha256(b"abc").hexdigest())
