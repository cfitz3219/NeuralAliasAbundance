// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A21 (phase-1 frame
// capture): on a real device (hardware or WARP) the four DLSS planes sit in
// their documented call-time states (color/depth/motion NON_PIXEL_SHADER_RESOURCE,
// output UNORDERED_ACCESS) and the back buffer in PRESENT. One armed capture
// must write frame0-{color,depth,motion,dlssout,backbuffer}.raw byte-exact plus
// frame0-meta.json with the passed dimensions/jitter; a second capture on the
// SAME resources must succeed too, proving every state was restored.
// Usage: capture_test <output dir>
#include "test_support.hpp"
#include "host_dx12/capture/frame_capture.hpp"

#include <string>
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 32, kPitch = kW * 4; // RGBA8: 256-byte rows, no padding

void Fill(test::Device& d, ID3D12Resource* tex, uint8_t seed, D3D12_RESOURCE_STATES final_state)
{
    auto up = test::CreateBuffer(d.device.Get(), kPitch * kH, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr; D3D12_RANGE none{0, 0};
    up->Map(0, &none, reinterpret_cast<void**>(&p));
    for (uint32_t i = 0; i < kPitch * kH; ++i) p[i] = static_cast<uint8_t>(seed + i * 7u);
    up->Unmap(0, nullptr);
    test::List l; l.Create(d.device.Get());
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kPitch};
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, final_state);
    l.list->ResourceBarrier(1, &b);
    l.list->Close();
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
}

bool FileMatches(const std::wstring& path, uint8_t seed)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::vector<uint8_t> data(kPitch * kH + 1);
    const size_t n = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (n != kPitch * kH) { std::printf("  %ls: %zu bytes\n", path.c_str(), n); return false; }
    for (uint32_t i = 0; i < kPitch * kH; ++i)
        if (data[i] != static_cast<uint8_t>(seed + i * 7u)) { std::printf("  %ls: byte %u differs\n", path.c_str(), i); return false; }
    return true;
}

bool WaitWritten(uint32_t n)
{
    for (int i = 0; i < 100; ++i) { if (na::capture::GetStats().frames_written >= n) return true; Sleep(50); }
    return false;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { std::printf("usage: capture_test <output dir>\n"); return 2; }
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("capture_test");
    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto color = test::CreateTexture(d.device.Get(), kW, kH, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto depth = test::CreateTexture(d.device.Get(), kW, kH, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(d.device.Get(), kW, kH, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto output = test::CreateTexture(d.device.Get(), kW, kH, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto back = test::CreateTexture(d.device.Get(), kW, kH, fmt, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(color && depth && motion && output && back);
    Fill(d, color.Get(), 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Fill(d, depth.Get(), 2, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Fill(d, motion.Get(), 3, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Fill(d, output.Get(), 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Fill(d, back.Get(), 5, D3D12_RESOURCE_STATE_PRESENT);

    const std::wstring dir = argv[1];
    CreateDirectoryW(dir.c_str(), nullptr);
    na::capture::EvalInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get(); in.output = output.Get();
    in.render_w = in.out_w = kW; in.render_h = in.out_h = kH;
    in.jitter_x = 0.25f; in.jitter_y = -0.125f; in.mv_scale_x = in.mv_scale_y = 1.0f; in.reset = 1;

    // Not armed: OnEvaluate must decline and record nothing.
    {
        test::List l; REQUIRE(l.Create(d.device.Get()));
        CHECK(!na::capture::OnEvaluate(l.list.Get(), in));
        l.list->Close();
    }

    for (uint32_t round = 0; round < 2; ++round) {
        const std::wstring sub = dir + (round == 0 ? L"\\a" : L"\\b");
        na::capture::Request(1, sub.c_str());
        CHECK(na::capture::Pending());
        test::List l; REQUIRE(l.Create(d.device.Get()));
        CHECK(na::capture::OnEvaluate(l.list.Get(), in));
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        na::capture::OnPresent(d.queue.Get(), back.Get());
        CHECK(!na::capture::Pending()); // one frame requested, one delivered
        CHECK(WaitWritten(1)); // Request() resets the per-capture counters
        d.WaitIdle();
        const char* names[] = {"color", "depth", "motion", "dlssout", "backbuffer"};
        for (uint8_t i = 0; i < 5; ++i) {
            std::wstring p = sub + L"\\frame0-";
            for (const char* c = names[i]; *c; ++c) p += static_cast<wchar_t>(*c);
            p += L".raw";
            CHECK(FileMatches(p, static_cast<uint8_t>(i + 1)));
        }
        FILE* m = nullptr;
        CHECK(_wfopen_s(&m, (sub + L"\\frame0-meta.json").c_str(), L"rb") == 0 && m);
        if (m) {
            char buf[2048] = {};
            std::fread(buf, 1, sizeof(buf) - 1, m);
            std::fclose(m);
            CHECK(std::strstr(buf, "\"render\": [64, 32]") != nullptr);
            CHECK(std::strstr(buf, "\"jitter\": [0.250000, -0.125000]") != nullptr);
            CHECK(std::strstr(buf, "\"reset\": 1") != nullptr);
        }
    }
    const auto s = na::capture::GetStats();
    CHECK(s.errors == 0);
    na::capture::Shutdown();
    return test::finish("capture_test");
}
