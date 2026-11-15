// Source identity "texture-subresources-v1": the reference vectors shared with the preparer.
// The expected digests come from the independent Python reference, tools/material_baseline/texsub.py.
#include "test_support.hpp"
#include "materials/texture_subresources_v1.hpp"

#include <vector>

using namespace na::materials;

namespace {
std::vector<uint8_t> Seq(uint8_t first, size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(first + i);
    return v;
}
TexSubHeader Header(uint32_t resource, uint32_t view, uint32_t w, uint32_t h, uint32_t first_mip, uint32_t mips)
{
    TexSubHeader t;
    t.resource_format = resource;
    t.view_format = view;
    t.width = w;
    t.height = h;
    t.first_mip = first_mip;
    t.mip_count = mips;
    return t;
}
std::string TwoMips(uint32_t view)
{
    const auto a = Seq(0, 16), b = Seq(16, 4);
    TextureSubresourcesV1 d(Header(28, view, 2, 2, 0, 2));
    d.AddLevel(0, 0, a.data(), 8);
    d.AddLevel(0, 1, b.data(), 4);
    return d.Finish();
}
} // namespace

int main()
{
    return test::run({
        {"the hash itself matches the published SHA-256 of \"abc\"", [] {
            Sha256 s;
            s.Update("abc", 3);
            const auto out = s.Final();
            CHECK(out[0] == 0xba && out[1] == 0x78 && out[2] == 0x16 && out[3] == 0xbf && out[31] == 0xad);
        }},
        {"uncompressed colour with two mips", [] {
            CHECK(TwoMips(28) == "881dba1cbcd2dae281ca92e0a9c73a8991d0659c5ce5eada825a461706aa4ec1");
        }},
        {"the same texels read as sRGB are a different source", [] {
            CHECK(TwoMips(29) == "f2daa7e114d47150a6d52aaf820d1af4a2aa4bf939bc5acb773c60fe435b82bc");
        }},
        {"block-compressed data is hashed as stored, in block rows", [] {
            const auto a = Seq(0, 16);
            TextureSubresourcesV1 d(Header(70, 72, 8, 4, 0, 1));
            CHECK(d.AddLevel(0, 0, a.data(), 16));
            CHECK(d.Finish() == "ddb30f684c9a1659f56c259f882ebb8356f44e8951206a6cd1ec848bf2bfa62d");
        }},
        {"a view that starts at mip 1 hashes that level at its own size", [] {
            const auto a = Seq(0, 16);
            TextureSubresourcesV1 d(Header(28, 28, 4, 4, 1, 1));
            CHECK(d.AddLevel(0, 1, a.data(), 8));
            CHECK(d.Finish() == "d5a0fbf12418f0569b27f987551ea6883054b8d8eb3570211af6f34ef3cf456f");
        }},
        {"a mapped row pitch's padding is never hashed", [] {
            // The two-mip texture again, mip 0 laid out 2 rows of 8 texel bytes at a pitch of 32.
            std::vector<uint8_t> padded(64, 0xEE);
            for (uint8_t i = 0; i < 16; ++i) padded[(i / 8) * 32 + i % 8] = i;
            const auto b = Seq(16, 4);
            TextureSubresourcesV1 d(Header(28, 28, 2, 2, 0, 2));
            CHECK(d.AddLevel(0, 0, padded.data(), 32));
            CHECK(d.AddLevel(0, 1, b.data(), 256));
            CHECK(d.Finish() == "881dba1cbcd2dae281ca92e0a9c73a8991d0659c5ce5eada825a461706aa4ec1");
        }},
        {"a missing, repeated or out-of-order level gives no identity", [] {
            const auto a = Seq(0, 16), b = Seq(16, 4);
            TextureSubresourcesV1 missing(Header(28, 28, 2, 2, 0, 2));
            CHECK(missing.AddLevel(0, 0, a.data(), 8));
            CHECK(missing.Finish().empty());
            TextureSubresourcesV1 swapped(Header(28, 28, 2, 2, 0, 2));
            CHECK(!swapped.AddLevel(0, 1, b.data(), 4));
            CHECK(!swapped.AddLevel(0, 0, a.data(), 8)); // stays invalid
            CHECK(swapped.Finish().empty());
            TextureSubresourcesV1 extra(Header(28, 28, 4, 4, 1, 1));
            CHECK(extra.AddLevel(0, 1, a.data(), 8));
            CHECK(!extra.AddLevel(0, 2, b.data(), 4));
            CHECK(extra.Finish().empty());
            TextureSubresourcesV1 narrow(Header(28, 28, 2, 2, 0, 2));
            CHECK(!narrow.AddLevel(0, 0, a.data(), 4)); // pitch below the tight row
        }},
        {"typeless, float and unknown formats have no v1 identity", [] {
            const auto a = Seq(0, 64);
            for (uint32_t format : {0u, 27u, 70u, 10u, 2u, 95u}) {
                TextureSubresourcesV1 d(Header(format, format, 2, 2, 0, 1));
                CHECK(!d.ok());
                CHECK(!d.AddLevel(0, 0, a.data(), 32));
                CHECK(d.Finish().empty());
            }
        }},
        {"array slices run in ascending order, mips within each", [] {
            const auto a = Seq(0, 16), b = Seq(16, 4);
            auto h = Header(28, 28, 2, 2, 0, 2);
            h.first_array_slice = 3;
            h.array_size = 2;
            TextureSubresourcesV1 d(h);
            CHECK(d.AddLevel(3, 0, a.data(), 8));
            CHECK(d.AddLevel(3, 1, b.data(), 4));
            CHECK(d.AddLevel(4, 0, a.data(), 8));
            CHECK(d.AddLevel(4, 1, b.data(), 4));
            CHECK(d.Finish() == "e856d2d87ffba8add0686923abc740517f5145be7b477a31f8a7420e44cdf7d2");
        }},
    });
}
