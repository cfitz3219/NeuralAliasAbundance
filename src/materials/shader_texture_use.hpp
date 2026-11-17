// How a compiled Direct3D pixel shader reads each of its textures, from the shader's disassembly
// text (the listing D3DDisassemble prints). Owner: Fable. Portable C++17; no hooks, no files.
//
// Why: a prepared map larger than the game's texture is the same picture only to a shader that
// reads it by plain filtered sampling, where the sampler turns 0..1 coordinates and screen
// derivatives into texels and levels. A shader that addresses texels (ld), asks the size
// (resinfo), names a level itself (sample_l), offsets by whole texels, or gathers, would read a
// larger map differently. Those slots keep the game's own texture, and the mnemonic is the reason.
//
// Not seen from here: a texture's size handed to the shader in a constant buffer. That is a known
// limit of this check, stated in docs/VARIANTS.md with the variant that uses it.
#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace na::materials {

struct ShaderTextureUse {
    static constexpr uint32_t kSlots = 128; // D3D11's shader resource slots per stage

    // False when the text is not a shader model 4.0 to 5.0 pixel shader listing: nothing is known,
    // and nothing is safe.
    bool known = false;
    std::bitset<kSlots> declared, sampled_plainly, read_otherwise;
    struct Other {
        uint32_t slot;
        std::string op; // the first instruction that reads the slot some other way
    };
    std::vector<Other> others;

    // May a larger map stand in for the texture in this slot? A declared slot no instruction
    // reads is safe: nothing looks at it.
    bool SafeForLarger(uint32_t slot) const { return known && slot < kSlots && !read_otherwise[slot]; }
    const char* WhyNot(uint32_t slot) const
    {
        if (!known) return "the shader's listing is not one this check reads";
        if (slot >= kSlots) return "slot out of range";
        for (const Other& o : others)
            if (o.slot == slot) return o.op.c_str();
        return nullptr;
    }
};

namespace detail {
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsWord(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// Texture registers named in the operands: "t3" standing alone as a word, as in "t3.xyzw".
inline void TextureOperands(std::string_view operands, std::vector<uint32_t>& out)
{
    for (size_t i = 0; i + 1 < operands.size(); ++i) {
        if (operands[i] != 't' || !IsDigit(operands[i + 1]) || (i && IsWord(operands[i - 1]))) continue;
        size_t j = i + 1;
        uint32_t n = 0;
        while (j < operands.size() && IsDigit(operands[j]) && n < 100000) n = n * 10 + uint32_t(operands[j++] - '0');
        if (j < operands.size() && IsWord(operands[j])) continue;
        out.push_back(n);
        i = j;
    }
}
} // namespace detail

inline ShaderTextureUse ReadShaderTextureUse(std::string_view listing)
{
    ShaderTextureUse use;
    bool versioned = false, sound = true;
    std::vector<uint32_t> slots;
    while (!listing.empty()) {
        const size_t end = listing.find('\n');
        std::string_view line = listing.substr(0, end);
        listing = end == std::string_view::npos ? std::string_view() : listing.substr(end + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty() || line.substr(0, 2) == "//") continue;

        size_t m = 0;
        while (m < line.size() && detail::IsWord(line[m])) ++m;
        const std::string_view op = line.substr(0, m);
        if (!versioned) { // the first statement names the shader kind and model
            versioned = true;
            if (op != "ps_4_0" && op != "ps_4_1" && op != "ps_5_0") return use;
            continue;
        }
        slots.clear();
        // Operands start after the mnemonic's own bracketed parts, e.g.
        // "sample_indexable(texture2d)(float,float,float,float) r0.xyzw, v1.xyxx, t0.xyzw, s0".
        size_t o = m;
        while (o < line.size() && line[o] == '(') {
            const size_t close = line.find(')', o);
            if (close == std::string_view::npos) { sound = false; break; }
            o = close + 1;
            while (o < line.size() && detail::IsWord(line[o])) ++o; // "_uint" after resinfo's brackets
        }
        detail::TextureOperands(line.substr(o), slots);
        if (slots.empty()) continue;
        const bool declaration = op.substr(0, 4) == "dcl_";
        // "sample", "sample_b", "sample_d", with or without "_indexable"; a texel offset
        // ("_aoffimmi") or anything else is another kind of read.
        std::string base(op);
        if (const size_t at = base.find("_indexable"); at != std::string::npos) base.erase(at, 10);
        const bool plain = base == "sample" || base == "sample_b" || base == "sample_d";
        for (uint32_t slot : slots) {
            if (slot >= ShaderTextureUse::kSlots) { sound = false; continue; }
            if (declaration) use.declared[slot] = true;
            else if (plain) use.sampled_plainly[slot] = true;
            else if (!use.read_otherwise[slot]) {
                use.read_otherwise[slot] = true;
                use.others.push_back({slot, std::string(op)});
            }
        }
    }
    use.known = versioned && sound;
    return use;
}

} // namespace na::materials
