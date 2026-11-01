// P4-0c increment 2: AMD FidelityFX optical flow through our dispatcher (na::ffx_flow::Flow) on
// a real D3D12 device (hardware first, then WARP). A textured rectangle moves +16 px between two
// frames over a flat background; the flow at its interior block must report 16 px horizontally
// and ~0 vertically. The sign is MEASURED and printed (recorded in ffx_flow.hpp as the output
// convention). SKIP (77) when the build had no DXC or the device lacks Shader Model 6.2.
// Every result is printed and recapped so a failure is readable from CI's last lines.
#include "test_support.hpp"

#include "host_dx12/ffx_flow/ffx_flow.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace ff = na::ffx_flow;

namespace {
constexpr uint32_t W = 128, H = 64;
int g_failures = 0;
std::vector<std::string> g_recap;
void recap(const char* fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("%s\n", b);
    g_recap.emplace_back(b);
}
#define CHECK(c) do { if (!(c)) { ++g_failures; recap("FAIL line %d: %s", __LINE__, #c); } } while (0)

// BGRA8 picture: flat mid-grey background, a bright randomly textured 48x32 rectangle at x0.
std::vector<uint32_t> picture(int x0)
{
    std::vector<uint32_t> px(W * H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t v = 40;
            if (static_cast<int>(x) >= x0 && static_cast<int>(x) < x0 + 48 && y >= 16 && y < 48) {
                const uint32_t a = static_cast<uint32_t>(static_cast<int>(x) - x0) / 2, b = y / 2;
                const uint32_t hsh = ((a * 73856093u) ^ (b * 19349663u)) * 2654435761u;
                v = 90 + ((hsh >> 16) & 127u);
            }
            px[y * W + x] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    return px;
}

void upload(test::Device& d, ID3D12Resource* tex, const std::vector<uint32_t>& px, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    const uint32_t pitch = W * 4; // 512: already 256-aligned
    ComPtr<ID3D12Resource> up = test::CreateBuffer(d.device.Get(), static_cast<uint64_t>(pitch) * H, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    std::memcpy(p, px.data(), px.size() * 4);
    up->Unmap(0, nullptr);
    test::List l;
    l.Create(d.device.Get());
    D3D12_RESOURCE_BARRIER b = test::Transition(tex, before, D3D12_RESOURCE_STATE_COPY_DEST);
    if (before != D3D12_RESOURCE_STATE_COPY_DEST) l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    s.pResource = up.Get();
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 1, pitch};
    t.pResource = tex;
    t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    l.list->ResourceBarrier(1, &b);
    l.list->Close();
    ID3D12CommandList* ls[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, ls);
    d.WaitIdle();
}

struct Result {
    std::vector<int16_t> flow; // out_w * out_h * 2
    uint32_t scd[3] = {};
};

// Records one frame into a fresh list, copies the output grid and the scene-change words back.
Result run_frame(test::Device& d, ff::Flow& flow, ID3D12Resource* color, bool reset, uint32_t slot)
{
    const uint32_t ow = flow.OutWidth(), oh = flow.OutHeight();
    const uint32_t pitch = (ow * 4 + 255) & ~255u;
    ComPtr<ID3D12Resource> rb = test::CreateBuffer(d.device.Get(), static_cast<uint64_t>(pitch) * oh + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;
    l.Create(d.device.Get());
    const bool ok = flow.Record(l.list.Get(), color, DXGI_FORMAT_B8G8R8A8_UNORM, reset, slot);
    CHECK(ok);
    D3D12_RESOURCE_BARRIER b[2] = {test::Transition(flow.Output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                   test::Transition(flow.SceneChange(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    l.list->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    s.pResource = flow.Output();
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    t.pResource = rb.Get();
    t.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    t.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16_SINT, ow, oh, 1, pitch};
    l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    s.pResource = flow.SceneChange();
    t.PlacedFootprint.Offset = static_cast<uint64_t>(pitch) * oh;
    t.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 3, 1, 1, 256};
    l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    b[0] = test::Transition(flow.Output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    b[1] = test::Transition(flow.SceneChange(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l.list->ResourceBarrier(2, b);
    l.list->Close();
    ID3D12CommandList* ls[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, ls);
    d.WaitIdle();
    Result r;
    r.flow.resize(static_cast<size_t>(ow) * oh * 2);
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (uint32_t y = 0; y < oh; ++y) std::memcpy(&r.flow[static_cast<size_t>(y) * ow * 2], p + static_cast<size_t>(y) * pitch, ow * 4);
    std::memcpy(r.scd, p + static_cast<size_t>(pitch) * oh, sizeof r.scd);
    rb->Unmap(0, nullptr);
    return r;
}
}  // namespace

int main()
{
    if (!ff::Built()) {
        std::printf("ffx_flow_test: SKIP (built without DXC, AMD optical flow is a stub)\n");
        return test::kSkip;
    }
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("ffx_flow_test");
    ff::Flow flow;
    char reason[200] = {};
    if (!flow.Init(d.device.Get(), W, H, 2, false, reason, sizeof reason)) {
        std::printf("ffx_flow_test: Init failed: %s\n", reason);
        return std::strstr(reason, "Shader Model") ? test::kSkip : 1;
    }
    recap("init: %s, output %ux%u, device %s", reason, flow.OutWidth(), flow.OutHeight(), d.warp ? "WARP" : "hardware");
    CHECK(flow.OutWidth() == W / 8 && flow.OutHeight() == H / 8);

    ComPtr<ID3D12Resource> color = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    // Frame A (reset), then frame B with the rectangle moved +16 px (two blocks).
    upload(d, color.Get(), picture(32), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Result a = run_frame(d, flow, color.Get(), true, 0);
    recap("frame A (reset): dispatches %u scd %u %u %u", flow.GetStats().dispatches, a.scd[0], a.scd[1], a.scd[2]);
    upload(d, color.Get(), picture(48), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Result b = run_frame(d, flow, color.Get(), false, 1);
    const uint32_t ow = flow.OutWidth();
    auto at = [&](uint32_t bx, uint32_t by, int& vx, int& vy) { vx = b.flow[(by * ow + bx) * 2]; vy = b.flow[(by * ow + bx) * 2 + 1]; };
    int vx = 0, vy = 0;
    // Rectangle now at x 48..96, y 16..48: blocks 6..11 x 2..5. Interior blocks 8 and 9, row 3.
    at(8, 3, vx, vy);
    recap("frame B: object block (8,3) = (%d, %d)   [+16 px move; |x| must be 16]", vx, vy);
    int vx2 = 0, vy2 = 0;
    at(9, 3, vx2, vy2);
    recap("frame B: object block (9,3) = (%d, %d)", vx2, vy2);
    int bx0 = 0, by0 = 0;
    at(1, 0, bx0, by0);
    recap("frame B: flat background block (1,0) = (%d, %d)  (informational)", bx0, by0);
    recap("frame B: dispatches %u scd %u %u %u", flow.GetStats().dispatches, b.scd[0], b.scd[1], b.scd[2]);
    CHECK(std::abs(std::abs(vx) - 16) <= 2 && std::abs(vy) <= 2);
    CHECK(std::abs(std::abs(vx2) - 16) <= 2 && std::abs(vy2) <= 2);
    CHECK(flow.GetStats().dispatches == 4 + 7 * 3 - 1);
    recap("sign: output is %s", vx < 0 ? "p_previous - p_current (negative for a +x move), the arbiter's convention"
                                      : "p_current - p_previous (positive for a +x move): J2 must negate");
    // Frame C: the same picture again, moved back (-16): the sign must flip, nothing else.
    upload(d, color.Get(), picture(32), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Result c = run_frame(d, flow, color.Get(), false, 0);
    int cx = c.flow[(3 * ow + 6) * 2], cy = c.flow[(3 * ow + 6) * 2 + 1]; // rectangle back at 32..80: block 6 interior
    recap("frame C: object block (6,3) = (%d, %d)   [-16 px move]", cx, cy);
    CHECK(std::abs(std::abs(cx) - 16) <= 2 && std::abs(cy) <= 2 && (cx > 0) != (vx > 0));

    std::printf("---- recap ----\n");
    for (const auto& s : g_recap) std::printf("recap: %s\n", s.c_str());
    std::printf("ffx_flow_test: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
