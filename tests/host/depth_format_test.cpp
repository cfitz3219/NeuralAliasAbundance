// Argon audit #2 (2026-10): which game depth formats may be copied (plane 0)
// into the R32_FLOAT textures FG / NR / live motion use. No device needed.
#include "test_support.hpp"
#include "host_dx12/depth_format.hpp"

#include <initializer_list>

int main()
{
    using na::depth::PlaneZeroCopiesToR32F;
    using na::depth::DepthKind;
    for (DXGI_FORMAT f : {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                          DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS})
        CHECK(PlaneZeroCopiesToR32F(f));
    for (DXGI_FORMAT f : {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                          DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM,
                          DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM})
        CHECK(!PlaneZeroCopiesToR32F(f));
    CHECK(std::strcmp(DepthKind(DXGI_FORMAT_D24_UNORM_S8_UINT), "24-bit") == 0);
    CHECK(std::strcmp(DepthKind(DXGI_FORMAT_R16_TYPELESS), "16-bit") == 0);
    CHECK(std::strcmp(DepthKind(DXGI_FORMAT_D32_FLOAT), "32-bit") == 0);
    CHECK(std::strcmp(DepthKind(DXGI_FORMAT_R8G8B8A8_UNORM), "unrecognised") == 0);
    return test::finish("depth_format_test");
}
