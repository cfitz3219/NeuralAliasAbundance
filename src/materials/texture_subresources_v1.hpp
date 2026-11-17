// Source identity "texture-subresources-v1": the SHA-256 of a texture's observed subresources in a
// fixed byte order, for `Source.content_sha256` when `identity_domain` is "texture-subresources-v1"
// (record.hpp). Agreed in COMMS 9b962d6f / fc0b110f. Owner of this serializer: Fable; the preparer
// (Astra) checks against the same test vectors (tests/cpu/texture_subresources_v1_test.cpp).
//
// The stream, every integer unsigned 32-bit little-endian:
//   "NA-TEXSUB-1" and one zero byte (twelve bytes)
//   resource_format, view_format, width, height (mip 0 extent), first_mip, mip_count,
//   first_array_slice, array_size
//   then per subresource, slices ascending and within a slice mips ascending from first_mip:
//     slice, mip, level_width, level_height, row_bytes, rows, then rows * row_bytes bytes,
//     top row first, tightly packed (a mapped row pitch's padding is never hashed).
//   uncompressed: row_bytes = level_width * bytes per texel, rows = level_height
//   block-compressed: row_bytes = ceil(level_width / 4) * block bytes, rows = ceil(level_height / 4)
// The typed view format decides the layout, so the same texels read as sRGB and as UNORM differ.
// Block data is hashed as stored, never decoded. No pointer, name, slot or time is in the stream.
//
// Portable C++17, no dependencies.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace na::materials {

// FIPS 180-4 SHA-256.
class Sha256 {
public:
    void Update(const void* data, size_t n)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        total_ += n;
        while (n) {
            const size_t take = (64 - fill_) < n ? (64 - fill_) : n;
            std::memcpy(block_ + fill_, p, take);
            fill_ += take;
            p += take;
            n -= take;
            if (fill_ == 64) {
                Compress(block_);
                fill_ = 0;
            }
        }
    }
    std::array<uint8_t, 32> Final()
    {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (fill_ != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        Update(len, 8);
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) out[static_cast<size_t>(i) * 4 + b] = static_cast<uint8_t>(h_[i] >> (24 - 8 * b));
        return out;
    }

private:
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Compress(const uint8_t* b)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) | (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block_[64] = {};
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

struct TexSubHeader {
    uint32_t resource_format = 0, view_format = 0;
    uint32_t width = 0, height = 0; // the resource's mip 0 extent
    uint32_t first_mip = 0, mip_count = 0;
    uint32_t first_array_slice = 0, array_size = 1;
};

// Layout of a typed DXGI format this identity covers: bytes per 4x4 block, or bytes per texel.
// False for every other format (the texture then has no v1 identity).
inline bool TexSubLayout(uint32_t view_format, uint32_t& block_bytes, uint32_t& texel_bytes)
{
    block_bytes = texel_bytes = 0;
    switch (view_format) {
    case 28: case 29: case 87: case 88: case 91: case 93: texel_bytes = 4; return true; // RGBA8 / BGRA8 / BGRX8, UNORM and sRGB
    case 61: texel_bytes = 1; return true;                                              // R8_UNORM
    case 71: case 72: case 80: case 81: block_bytes = 8; return true;                   // BC1, BC4
    case 74: case 75: case 77: case 78: case 83: case 84: case 98: case 99: block_bytes = 16; return true; // BC2, BC3, BC5, BC7
    default: return false;
    }
}

// Feed the levels in the stream's order; anything else makes the digest invalid (ok() false).
class TextureSubresourcesV1 {
public:
    explicit TextureSubresourcesV1(const TexSubHeader& h) : h_(h)
    {
        // The range must exist: levels below the 1x1 level, or a slice range past 32 bits, have no identity.
        const uint32_t longest = h.width > h.height ? h.width : h.height;
        uint32_t full = 0; // levels of the full chain
        for (uint32_t v = longest; v; v >>= 1) ++full;
        ok_ = TexSubLayout(h.view_format, block_, texel_) && h.width && h.height && h.mip_count && h.array_size &&
              h.first_mip < full && h.mip_count <= full - h.first_mip &&
              uint64_t(h.first_array_slice) + h.array_size <= 0x100000000ull &&
              uint64_t(h.width) * (block_ ? block_ : texel_) <= 0xffffffffull;
        sha_.Update("NA-TEXSUB-1", 12); // the literal's own terminating zero is the twelfth byte
        const uint32_t fields[8] = {h.resource_format, h.view_format, h.width, h.height,
                                    h.first_mip, h.mip_count, h.first_array_slice, h.array_size};
        for (uint32_t v : fields) Put(v);
    }
    // `data` is the level's first row; rows are `source_row_pitch` bytes apart (>= the tight row).
    bool AddLevel(uint32_t slice, uint32_t mip, const void* data, size_t source_row_pitch)
    {
        // Nothing is computed from the header before it is known to be valid (mip_count is then
        // nonzero, and the quotient below is under array_size, so both sums fit 32 bits).
        if (!ok_ || !data || next_ >= uint64_t(h_.mip_count) * h_.array_size) return ok_ = false;
        const uint32_t want_slice = h_.first_array_slice + static_cast<uint32_t>(next_ / h_.mip_count);
        const uint32_t want_mip = h_.first_mip + static_cast<uint32_t>(next_ % h_.mip_count);
        if (slice != want_slice || mip != want_mip) return ok_ = false;
        const uint32_t w = (h_.width >> mip) ? (h_.width >> mip) : 1, h = (h_.height >> mip) ? (h_.height >> mip) : 1;
        const uint32_t row_bytes = block_ ? ((w + 3) / 4) * block_ : w * texel_;
        const uint32_t rows = block_ ? (h + 3) / 4 : h;
        if (source_row_pitch < row_bytes) return ok_ = false;
        const uint32_t fields[6] = {slice, mip, w, h, row_bytes, rows};
        for (uint32_t v : fields) Put(v);
        const auto* p = static_cast<const uint8_t*>(data);
        for (uint32_t r = 0; r < rows; ++r) sha_.Update(p + size_t(r) * source_row_pitch, row_bytes);
        ++next_;
        return true;
    }
    bool ok() const { return ok_; }
    // Lowercase hex, or empty when a level was missing, out of order or of an uncovered format.
    std::string Finish()
    {
        if (!ok_ || next_ != uint64_t(h_.mip_count) * h_.array_size) return {};
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (uint8_t b : sha_.Final()) {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 15]);
        }
        return out;
    }

private:
    void Put(uint32_t v)
    {
        const uint8_t le[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        sha_.Update(le, 4);
    }
    TexSubHeader h_;
    Sha256 sha_;
    uint32_t block_ = 0, texel_ = 0;
    uint64_t next_ = 0;
    bool ok_ = false;
};

} // namespace na::materials
