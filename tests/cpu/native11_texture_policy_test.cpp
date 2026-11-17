// Native DX11 material textures: which textures are candidates for a prepared replacement, and
// the worded reason for each one that is not.
#include "test_support.hpp"
#include "host_dx12/native11/texture_policy.hpp"

using namespace na::native11;

namespace {
MaterialTextureFacts Material(uint32_t view_format = 99, uint32_t side = 1024, uint32_t mips = 11)
{
    MaterialTextureFacts t;
    t.texture2d = true;
    t.width = t.height = side;
    t.mips = t.view_mips = mips;
    t.format = t.view_format = view_format;
    t.bind_flags = 0x8; // shader resource
    t.usage = 1;        // immutable
    return t;
}
} // namespace

int main()
{
    return test::run({
        {"a mipmapped block-compressed colour texture is a candidate", [] {
            CHECK(Judge(Material()) == TextureVerdict::Candidate);
            CHECK(ClassOf(99) == TextureClass::ColourSrgb);
            CHECK(ClassOf(83) == TextureClass::TwoChannel);
            CHECK(ClassOf(80) == TextureClass::OneChannel);
            CHECK(ClassOf(77) == TextureClass::FourChannel);
        }},
        {"what the game draws into or rewrites keeps its own content", [] {
            auto t = Material(28);
            t.bind_flags |= kBindRenderTarget;
            CHECK(Judge(t) == TextureVerdict::RenderedTo);
            t = Material(28);
            t.usage = 2;
            CHECK(Judge(t) == TextureVerdict::Rewritten);
        }},
        {"lookup tables, screen data and float data are not surfaces", [] {
            CHECK(Judge(Material(99, 32, 6)) == TextureVerdict::TooSmall);
            CHECK(Judge(Material(99, 1024, 1)) == TextureVerdict::NoMips);
            CHECK(Judge(Material(10)) == TextureVerdict::DataFormat);  // R16G16B16A16_FLOAT
            CHECK(Judge(Material(61)) == TextureVerdict::DataFormat);  // R8_UNORM
            CHECK(Judge(Material(42)) == TextureVerdict::DataFormat);  // R32_UINT (id maps)
        }},
        {"cubes, arrays, multisampled and partial views are named, not guessed", [] {
            auto t = Material();
            t.cube = true;
            CHECK(Judge(t) == TextureVerdict::Cube);
            t = Material();
            t.array_size = 4;
            CHECK(Judge(t) == TextureVerdict::Array);
            t = Material();
            t.samples = 4;
            CHECK(Judge(t) == TextureVerdict::Multisampled);
            t = Material();
            t.view_first_mip = 2;
            t.view_mips = 9;
            CHECK(Judge(t) == TextureVerdict::PartialView);
            t = Material();
            t.texture2d = false;
            CHECK(Judge(t) == TextureVerdict::NotTexture2D);
        }},
        {"memory of a mip chain counts blocks, and doubling a side is four times the texels", [] {
            CHECK(ChainBytes(4, 4, 1, 99) == 16);
            CHECK(ChainBytes(4, 4, 1, 71) == 8);
            CHECK(ChainBytes(2, 2, 1, 28) == 16);
            const uint64_t a = ChainBytes(1024, 1024, 11, 99), b = ChainBytes(2048, 2048, 12, 99);
            CHECK(b > 3 * a && b < 5 * a);
            CHECK(ChainBytes(4096, 4096, 13, 99) > 22000000 && ChainBytes(4096, 4096, 13, 99) < 23000000);
        }},
    });
}
