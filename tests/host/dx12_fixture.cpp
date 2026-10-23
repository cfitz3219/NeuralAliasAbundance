// Reconstructed 2026-10 (C3). The controlled DX12 compute fixture from
// milestone 1: renders shaders/reference_scene.hlsl (clean scene, depth,
// motion, HUD-composited output) for N frames at two extents with known
// object motion, jitter and a camera cut, reads everything back OFFLINE and
// checks the known truths pixel by pixel:
//   - moving object: motion = previousOffset - offset in pixels, exactly
//   - stationary background: motion = 0; camera cut frame: motion = 0 everywhere
//   - depth: object 0.25, background 0.9 (normalized forward Z)
//   - the white HUD band appears ONLY in the composite, never in the clean scene
// Writes PPM previews + result.txt under --output <dir>. CPU waits/readbacks
// are deliberate offline measurement, not a live-path proposal. Also drives
// the abundance_core frame contract with the fixture's metadata.
#include "test_support.hpp"
#include "core/frame_contract.hpp"

#include <d3dcompiler.h>
#include <cmath>
#include <string>
#include <vector>

#ifndef ABUNDANCE_SHADER_PATH
#define ABUNDANCE_SHADER_PATH L"shaders/reference_scene.hlsl"
#endif

namespace {
struct Constants { uint32_t width, height, frameIndex, cameraCut; float jitterX, jitterY, previousOffset, offset; };
struct Plane { test::ComPtr<ID3D12Resource> tex, readback; DXGI_FORMAT fmt; uint32_t bpp; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint64_t bytes = 0; };

bool make_plane(ID3D12Device* dev, Plane& p, uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t bpp)
{
    p.fmt = fmt; p.bpp = bpp;
    p.tex = test::CreateTexture(dev, w, h, fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!p.tex) return false;
    D3D12_RESOURCE_DESC d = p.tex->GetDesc();
    dev->GetCopyableFootprints(&d, 0, 1, 0, &p.fp, nullptr, nullptr, &p.bytes);
    p.readback = test::CreateBuffer(dev, p.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    return p.readback != nullptr;
}
void write_ppm(const std::wstring& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb)
{
    FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"wb"); if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h); std::fwrite(rgb.data(), 1, rgb.size(), f); std::fclose(f);
}
float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff; float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24); else if (e == 31) v = m ? NAN : INFINITY; else v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring out = L"fixture-out";
    for (int i = 1; i + 1 < argc; ++i) if (!lstrcmpW(argv[i], L"--output")) out = argv[i + 1];
    CreateDirectoryW(out.c_str(), nullptr);

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("dx12_fixture");

    // Compile the reference scene.
    test::ComPtr<ID3DBlob> cs, err;
    HRESULT hr = D3DCompileFromFile(ABUNDANCE_SHADER_PATH, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) { std::printf("shader compile failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return 1; }

    // Root signature: b0 as root constants, u0..u3 as a descriptor table.
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 2; rs.pParameters = params;
    test::ComPtr<ID3DBlob> rsb; REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)));
    test::ComPtr<ID3D12RootSignature> root; REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso; REQUIRE(SUCCEEDED(d.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    test::ComPtr<ID3D12DescriptorHeap> heap; REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
    UINT inc = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    test::List l; REQUIRE(l.Create(d.device.Get()));

    FILE* result = nullptr; _wfopen_s(&result, (out + L"\\result.txt").c_str(), L"w");
    const struct { uint32_t w, h; } extents[] = {{320, 180}, {257, 145}};
    const uint32_t kFrames = 8;
    uint64_t pixels_checked = 0;
    abundance::Continuity continuity;
    abundance::BackendSupport support; support.reconstruction = true;

    for (auto ext : extents) {
        Plane scene, depth, motion, composite;
        REQUIRE(make_plane(d.device.Get(), scene, ext.w, ext.h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8));
        REQUIRE(make_plane(d.device.Get(), depth, ext.w, ext.h, DXGI_FORMAT_R32_FLOAT, 4));
        REQUIRE(make_plane(d.device.Get(), motion, ext.w, ext.h, DXGI_FORMAT_R16G16_FLOAT, 4));
        REQUIRE(make_plane(d.device.Get(), composite, ext.w, ext.h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8));
        Plane* planes[4] = {&scene, &depth, &motion, &composite};
        for (int i = 0; i < 4; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = planes[i]->fmt; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * inc;
            d.device->CreateUnorderedAccessView(planes[i]->tex.Get(), nullptr, &uav, h);
        }
        float prev_offset = 0;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            // Known truth: object slides +3 px/frame; Halton-ish jitter; frame 5 is a camera cut.
            Constants c{ext.w, ext.h, frame, frame == 5 ? 1u : 0u, (frame % 4) * 0.25f - 0.375f, (frame % 3) * 0.3333f - 0.3333f, prev_offset, 3.0f * frame};
            ID3D12DescriptorHeap* heaps[] = {heap.Get()};
            l.list->SetDescriptorHeaps(1, heaps);
            l.list->SetComputeRootSignature(root.Get());
            l.list->SetPipelineState(pso.Get());
            l.list->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
            l.list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
            l.list->Dispatch((ext.w + 7) / 8, (ext.h + 7) / 8, 1);
            for (Plane* p : planes) {
                D3D12_RESOURCE_BARRIER b = test::Transition(p->tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l.list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = p->tex.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = p->readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = p->fp;
                l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                b = test::Transition(p->tex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                l.list->ResourceBarrier(1, &b);
            }
            l.list->Close();
            ID3D12CommandList* lists[] = {l.list.Get()};
            d.queue->ExecuteCommandLists(1, lists);
            d.WaitIdle(); // offline measurement only
            l.Reset();

            // Read back and verify.
            std::vector<uint8_t> map[4];
            for (int i = 0; i < 4; ++i) {
                void* m = nullptr; D3D12_RANGE r{0, static_cast<SIZE_T>(planes[i]->bytes)};
                REQUIRE(SUCCEEDED(planes[i]->readback->Map(0, &r, &m)));
                map[i].assign(static_cast<uint8_t*>(m), static_cast<uint8_t*>(m) + planes[i]->bytes);
                D3D12_RANGE none{0, 0}; planes[i]->readback->Unmap(0, &none);
            }
            const float expect_mv = c.cameraCut ? 0.0f : c.previousOffset - c.offset;
            uint32_t object_px = 0, bad_mv = 0, bad_depth = 0, hud_leak = 0, hud_px = 0;
            std::vector<uint8_t> preview(static_cast<size_t>(ext.w) * ext.h * 3);
            for (uint32_t y = 0; y < ext.h; ++y) for (uint32_t x = 0; x < ext.w; ++x) {
                float px = x + 0.5f - c.jitterX, py = y + 0.5f - c.jitterY;
                bool object = std::fabs(px - (ext.w * 0.5f + c.offset)) < ext.w * 0.125f && std::fabs(py - ext.h * 0.5f) < ext.h * 0.2f;
                bool hud = x < ext.w / 3 && y < ext.h / 10;
                const uint8_t* mv = &map[2][y * motion.fp.Footprint.RowPitch + x * 4];
                float mx = half_to_float(*reinterpret_cast<const uint16_t*>(mv)), my = half_to_float(*reinterpret_cast<const uint16_t*>(mv + 2));
                float want = object ? expect_mv : 0.0f;
                if (std::fabs(mx - want) > 0.02f || std::fabs(my) > 0.02f) ++bad_mv;
                float dz = *reinterpret_cast<const float*>(&map[1][y * depth.fp.Footprint.RowPitch + x * 4]);
                if (std::fabs(dz - (object ? 0.25f : 0.9f)) > 1e-4f) ++bad_depth;
                const uint16_t* sc = reinterpret_cast<const uint16_t*>(&map[0][y * scene.fp.Footprint.RowPitch + x * 8]);
                float sr = half_to_float(sc[0]), sg = half_to_float(sc[1]), sb = half_to_float(sc[2]);
                if (sr > 0.99f && sg > 0.99f && sb > 0.99f) ++hud_leak; // white band in the CLEAN scene = leak
                const uint16_t* cp = reinterpret_cast<const uint16_t*>(&map[3][y * composite.fp.Footprint.RowPitch + x * 8]);
                bool comp_white = half_to_float(cp[0]) > 0.99f && half_to_float(cp[1]) > 0.99f && half_to_float(cp[2]) > 0.99f;
                if (hud) { if (comp_white) ++hud_px; } else if (comp_white) ++hud_leak;
                if (object) ++object_px;
                uint8_t* o = &preview[(static_cast<size_t>(y) * ext.w + x) * 3];
                o[0] = static_cast<uint8_t>(fminf(1, fmaxf(0, half_to_float(cp[0]))) * 255); o[1] = static_cast<uint8_t>(fminf(1, fmaxf(0, half_to_float(cp[1]))) * 255); o[2] = static_cast<uint8_t>(fminf(1, fmaxf(0, half_to_float(cp[2]))) * 255);
                ++pixels_checked;
            }
            CHECK(object_px > 0);
            CHECK(bad_mv == 0);
            CHECK(bad_depth == 0);
            CHECK(hud_leak == 0);
            CHECK(hud_px == (ext.w / 3) * (ext.h / 10));
            if (result) std::fprintf(result, "%ux%u frame %u: object_px=%u bad_motion=%u bad_depth=%u hud_leak=%u hud_px=%u expect_mv=%.2f cut=%u\n",
                                     ext.w, ext.h, frame, object_px, bad_mv, bad_depth, hud_leak, hud_px, expect_mv, c.cameraCut);
            if (frame == 2 || frame == 5) write_ppm(out + L"\\composite-" + std::to_wstring(ext.w) + L"-f" + std::to_wstring(frame) + L".ppm", ext.w, ext.h, preview);

            // Feed the frame contract with the fixture's metadata (adapter route, all inputs labeled).
            abundance::Frame f;
            f.id = frame + 1; f.generation = 1; f.route = abundance::Route::adapter;
            f.color = {0x100 + frame, f.id, 1, {ext.w, ext.h}}; f.depth = {0x200 + frame, f.id, 1, {ext.w, ext.h}}; f.motion = {0x300 + frame, f.id, 1, {ext.w, ext.h}};
            f.output = {ext.w, ext.h}; f.color_space = abundance::ColorSpace::linear_sdr; f.motion_space = abundance::MotionSpace::pixels_current_to_previous;
            f.jitter_x = c.jitterX; f.jitter_y = c.jitterY; f.jitter_known = true; f.exposure_policy_known = true; f.clean_scene = true; f.motion_excludes_jitter = true;
            f.camera_cut = c.cameraCut != 0;
            abundance::Decision dec = continuity.observe(f, abundance::Feature::reconstruction, support);
            CHECK(dec.ready);
            if (frame == 0 || c.cameraCut) CHECK(dec.reset); else CHECK(!dec.reset);
            prev_offset = c.offset;
        }
        continuity.invalidate();
    }
    if (result) { std::fprintf(result, "pixels_checked=%llu device=%s\n", static_cast<unsigned long long>(pixels_checked), d.warp ? "WARP" : "hardware"); std::fclose(result); }
    std::printf("pixels checked: %llu\n", static_cast<unsigned long long>(pixels_checked));
    return test::finish("dx12_fixture");
}
