#pragma once
// What a compiled pixel shader reads, taken from the program itself: the texture slots it declares (a
// compiled shader declares only the textures it really uses) and whether it reads its pixel position.
// The shader's description data is not needed, so this also works on shaders whose description was
// stripped before the game shipped.
//
// Pure logic, no graphics calls: tests/cpu/dxbc_inputs_test.cpp.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace na::dxbc {

struct PixelInputs {
    uint64_t slots[2] = {}; // bit n: texture slot n is read
    bool position = false;  // x or y of the pixel position is read
};

// False: not a compiled shader this reader understands (nothing is known about it).
inline bool ReadPixelInputs(const void* code, size_t size, PixelInputs& out)
{
    out = {};
    const auto* p = static_cast<const uint8_t*>(code);
    if (!p || size < 32 || std::memcmp(p, "DXBC", 4) != 0) return false;
    const auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, p + at, 4); return v; };
    const size_t chunks = u32(28);
    if (chunks > 256 || size < 32 + chunks * 4) return false;
    bool program = false;
    for (size_t i = 0; i < chunks; ++i) {
        const size_t at = u32(32 + i * 4);
        if (at > size - 8) continue;
        const size_t body = at + 8, length = u32(at + 4);
        if (length > size - body) continue;
        const bool isgn = !std::memcmp(p + at, "ISGN", 4), isg1 = !std::memcmp(p + at, "ISG1", 4);
        if (isgn || isg1) {
            // The inputs: name, index, system value, type, register, mask, mask of what is read.
            if (length < 8) continue;
            const size_t count = u32(body), each = isg1 ? 32 : 24, first = body + 8 + (isg1 ? 4 : 0);
            for (size_t k = 0; k < count && first + k * each + 24 <= body + length; ++k) {
                const size_t e = first + k * each;
                if (u32(e + 8) == 1 /* the pixel position */ && (p[e + 21] & 3)) out.position = true;
            }
        } else if (!std::memcmp(p + at, "SHEX", 4) || !std::memcmp(p + at, "SHDR", 4)) {
            // The program: a version word, its length in words, then one instruction after another.
            if (length < 8) continue;
            const size_t words = (std::min)(size_t(u32(body + 4)), length / 4);
            const auto word = [&](size_t w) { return u32(body + w * 4); };
            program = true;
            for (size_t w = 2; w < words;) {
                const uint32_t t = word(w);
                const uint32_t op = t & 0x7ffu;
                size_t len = (t >> 24) & 0x7fu;
                if (op == 53) { // a data block carries its own length
                    if (w + 1 >= words) break;
                    len = word(w + 1);
                }
                if (!len || len > words - w) break;
                if (op == 88) { // a texture is declared: the operand names its slot
                    const size_t end = w + len;
                    size_t o = w + 1;
                    for (uint32_t more = t; (more & 0x80000000u) && o < end; ++o) more = word(o);
                    if (o < end) {
                        const uint32_t operand = word(o++);
                        for (uint32_t more = operand; (more & 0x80000000u) && o < end; ++o) more = word(o);
                        const uint32_t kind = (operand >> 12) & 0xffu, dims = (operand >> 20) & 3u, how = (operand >> 22) & 7u;
                        if (kind == 7 && dims >= 1 && how == 0 && o < end) {
                            const uint32_t slot = word(o);
                            if (slot < 128) out.slots[slot >> 6] |= uint64_t{1} << (slot & 63);
                        }
                    }
                }
                w += len;
            }
        }
    }
    return program;
}

} // namespace na::dxbc
