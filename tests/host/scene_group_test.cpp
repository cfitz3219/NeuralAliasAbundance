// Actual DX12 allocation and shader test for Layer2's first building block.
// No DLSS stand-in is called: this verifies reduced inputs, native preservation
// and owned patch bytes, NOT end-to-end DLSS/game compatibility.
#include "test_support.hpp"
#include "host_dx12/scale/scene_group.hpp"
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
using namespace na::scale;
namespace {
constexpr Extent kFull{128, 64}, kSmall{86, 43};
void math()
{
    D3D12_VIEWPORT before{0, 0, 128, 64, .1f, .9f}, after{};
    CHECK(ScaleViewport(before, kFull, kSmall, after));
    CHECK(after.Width == 86 && after.Height == 43 && after.MinDepth == .1f && after.MaxDepth == .9f);
    D3D12_RECT r{1, 1, 127, 63}, s{};
    CHECK(ScaleScissor(r, kFull, kSmall, s));
    CHECK(s.left == 0 && s.top == 0 && s.right == 86 && s.bottom == 43);
    CHECK(ScaleScissor({3, 3, 3, 3}, kFull, kSmall, s));
    CHECK(s.left == s.right && s.top == s.bottom);
    CHECK(ScaleScissor({-20, -20, 200, 200}, kFull, kSmall, s));
    CHECK(s.left == 0 && s.top == 0 && s.right == 86 && s.bottom == 43);
    UINT x = 91, y = 92;
    CHECK(DispatchForExtent(kSmall, 8, 8, x, y) && x == 11 && y == 6);
    CHECK(!DispatchForExtent({UINT32_MAX, 43}, 1, 8, x, y) && x == 11 && y == 6);
    const std::array<float, 8> input{128, 64, 1.f/128, 1.f/64, 128, 64, 7, 8};
    const auto saved = input;
    std::vector<ConstantPatch> patches;
    for (uint32_t i = 0; i < 4; ++i) patches.push_back({i * 4, Scalar::Float, static_cast<Dimension>(i), kFull, kSmall});
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(PatchConstants(input.data(), sizeof input, patches, bytes, error));
    std::array<float, 8> result{};
    std::memcpy(result.data(), bytes.data(), sizeof result);
    CHECK(result[0] == 86 && result[1] == 43 && std::fabs(result[2] - 1.f/86) < 1e-8f);
    CHECK(result[4] == 128 && result[5] == 64 && input == saved); // output/HUD dimensions were not selected
    const auto good = bytes;
    patches.push_back({0, Scalar::Float, Dimension::Width, kFull, {80, 40}});
    CHECK(!PatchConstants(input.data(), sizeof input, patches, bytes, error) && bytes == good && input == saved);
    patches.pop_back(); patches[0].native.width = 100;
    CHECK(!PatchConstants(input.data(), sizeof input, patches, bytes, error) && bytes == good);
    std::array<uint32_t, 2> integers{128, 64};
    CHECK(PatchConstants(integers.data(), sizeof integers,
        {{0, Scalar::UInt, Dimension::Width, kFull, kSmall}}, bytes, error));
    uint32_t width = 0; std::memcpy(&width, bytes.data(), 4);
    CHECK(width == 86 && integers[0] == 128);
}
test::ComPtr<ID3DBlob> compile(const char* source)
{
    test::ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source, std::strlen(source), "Layer2 fixture", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors)))
        std::printf("shader: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed");
    return code;
}
int gpu(test::Device& d)
{
    auto* dev = d.device.Get();
    auto native = test::CreateTexture(dev, 128, 64, DXGI_FORMAT_R32G32B32A32_FLOAT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto depth = test::CreateTexture(dev, 128, 64, DXGI_FORMAT_R32_TYPELESS,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto half = test::CreateTexture(dev, 64, 32, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(native && depth && half);
    std::string error;
    std::vector<MemberRequest> request{
        {1, native.Get(), Role::Color, kSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
        {2, depth.Get(), Role::Depth, kSmall, D3D12_RESOURCE_STATE_DEPTH_WRITE},
        {3, half.Get(), Role::Intermediate, {43, 22}, D3D12_RESOURCE_STATE_COMMON}};
    auto group = SceneGroup::Create(dev, 1, kFull, kSmall, request, error);
    if (!group) std::printf("allocation: %s\n", error.c_str());
    REQUIRE(group);
    CHECK(group->Find(1)->reduced->GetDesc().Width == 86);
    CHECK(group->Find(2)->reduced->GetDesc().Height == 43);
    CHECK(group->Find(3)->reduced->GetDesc().Width == 43);
    CHECK(native->GetDesc().Width == 128 && depth->GetDesc().Height == 64 && half->GetDesc().Width == 64);
    GroupSelection selection;
    selection.Publish(group);
    const auto original_selection = selection.Acquire();
    auto bad = request; bad.push_back({4, native.Get(), Role::Intermediate, kSmall});
    CHECK(!SceneGroup::Create(dev, 2, kFull, kSmall, bad, error));
    CHECK(selection.Acquire() == original_selection); // failure never replaces a published complete group
    auto next = SceneGroup::Create(dev, 2, kFull, {64, 32},
        {{1, native.Get(), Role::Color, {64, 32}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}, error);
    REQUIRE(next);
    selection.Publish(next);
    CHECK(selection.Acquire()->Generation() == 2 && original_selection->Generation() == 1);
    // Readers latch whole snapshots; they cannot see a partially built/disabled set.
    std::atomic<bool> done{false};
    std::thread reader([&] {
        while (!done.load()) if (auto g = selection.Acquire()) {
            CHECK(g->Find(1) && (g->Generation() == 1 || g->Generation() == 2));
        }
    });
    for (int i = 0; i < 500; ++i) { selection.Publish(group); selection.Disable(); selection.Publish(next); }
    done = true; reader.join();
    selection.Disable();
    CHECK(!selection.Acquire() && original_selection->Find(1)->reduced);

    auto code = compile(R"(
cbuffer Size : register(b0) { float2 extent; };
RWTexture2D<float4> picture : register(u0);
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= extent.x || id.y >= extent.y) return;
    picture[id.xy] = float4((float2(id.xy)+0.5)/extent, 0.25, 1);
})");
    REQUIRE(code);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 2; rs.pParameters = params;
    test::ComPtr<ID3DBlob> blob, errors;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root.Get(); pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso;
    REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    test::ComPtr<ID3D12DescriptorHeap> heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
    const auto stride = dev->GetDescriptorHandleIncrementSize(hd.Type);
    const auto native_cpu = heap->GetCPUDescriptorHandleForHeapStart(); auto small_cpu = native_cpu; small_cpu.ptr += stride;
    auto native_gpu = heap->GetGPUDescriptorHandleForHeapStart(); auto small_gpu = native_gpu; small_gpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(native.Get(), nullptr, &uv, native_cpu);
    auto* reduced = original_selection->Find(1)->reduced.Get();
    dev->CreateUnorderedAccessView(reduced, nullptr, &uv, small_cpu);
    const std::array<float,2> native_size{128,64};
    std::vector<uint8_t> patched;
    REQUIRE(PatchConstants(native_size.data(), sizeof native_size,
        {{0, Scalar::Float, Dimension::Width, kFull, kSmall}, {4, Scalar::Float, Dimension::Height, kFull, kSmall}}, patched, error));
    const auto desc = reduced->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(rb);
    test::List l; REQUIRE(l.Create(dev));
    auto* list = l.list.Get();
    ID3D12DescriptorHeap* heaps[]{heap.Get()}; list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
    list->SetComputeRoot32BitConstants(0, 2, native_size.data(), 0);
    list->SetComputeRootDescriptorTable(1, native_gpu);
    list->Dispatch(16, 8, 1); // native remains a valid independent fallback image
    list->SetComputeRoot32BitConstants(0, 2, patched.data(), 0);
    list->SetComputeRootDescriptorTable(1, small_gpu);
    UINT gx = 0, gy = 0; REQUIRE(DispatchForExtent(kSmall, 8, 8, gx, gy)); list->Dispatch(gx, gy, 1);
    const auto barrier = test::Transition(reduced, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = reduced; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* lists[]{list}; d.queue->ExecuteCommandLists(1, lists);
    // Test utility only: the product allocation/patch code never waits/readbacks.
    test::SignalAndWait(d.queue.Get(), d.fence.Get(), d.fence_value, d.event, dev, "layer2 input fixture");
    const void* mapped = nullptr; REQUIRE(SUCCEEDED(rb->Map(0, nullptr, const_cast<void**>(&mapped))));
    for (uint32_t y = 0; y < 43; ++y) for (uint32_t x = 0; x < 86; ++x) {
        float px[4]; std::memcpy(px, static_cast<const uint8_t*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch + x * 16, 16);
        CHECK(std::fabs(px[0] - (x+.5f)/86) < 1e-6f && std::fabs(px[1] - (y+.5f)/43) < 1e-6f);
    }
    rb->Unmap(0, nullptr);
    CHECK(native_size[0] == 128 && native->GetDesc().Width == 128);
    return 0;
}
}
int main()
{
    math();
    test::Device device;
    if (!test::CreateDevice(device)) return test::SkipNoDevice("scene_group_test");
    if (gpu(device)) return 1;
    return test::finish("scene_group_test");
}
