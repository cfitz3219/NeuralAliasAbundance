// Game-side bridge from D3D12 (DX9 plan step D4). See dx12_game_bridge.hpp.
#include "bridge/dx12_game_bridge.hpp"

#include "bridge/win_shared.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

using Microsoft::WRL::ComPtr;

namespace na::bridge {
namespace {

const char kBlitHlsl[] = R"(
Texture2D<float4> source : register(t0);
cbuffer Mode : register(b0) { uint mode; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target
{
    float4 c = source.Load(int3(int2(p.xy), 0));
    if (mode == 1) c = c.bgra;          // copy test: red/blue swapped on the way back
    if (mode == 2) c = float4(c.r, 0, 0, 1); // depth -> R32F
    return c;
}
)";

constexpr int kRing = 3;
constexpr int kViews = 4; // SRVs per frame slot

enum Mode : uint32_t { kCopy = 0, kSwap = 1, kDepth = 2 };

// A readable (typed) view format for a colour resource; DXGI_FORMAT_UNKNOWN if none.
DXGI_FORMAT ColorView(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_UNKNOWN: return DXGI_FORMAT_UNKNOWN;
    default: return f;
    }
}

// Depth: the typeless copy format and the red-channel view of it.
bool DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* storage, DXGI_FORMAT* view)
{
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        *storage = DXGI_FORMAT_R24G8_TYPELESS; *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        *storage = DXGI_FORMAT_R32_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        *storage = DXGI_FORMAT_R32G8X24_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM:
        *storage = DXGI_FORMAT_R16_TYPELESS; *view = DXGI_FORMAT_R16_UNORM; return true;
    default: return false;
    }
}

void Transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

// Copies subresource 0 (mip 0; the depth plane of a depth-stencil) of src into dst.
void CopySub0(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src)
{
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}

} // namespace

struct Dx12GameBridge::Impl {
    Dx12Config cfg;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3DBlob> vs, ps;
    std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pso;
    ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
    UINT srv_step = 0, rtv_step = 0;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    ComPtr<ID3D12GraphicsCommandList> list_in, list_out;
    ComPtr<ID3D12Fence> own;
    uint64_t own_value = 0, slot_value[kRing] = {};
    int slot = 0;
    Handle own_event;

    // Shared with the helper.
    ComPtr<ID3D12Resource> color, depth, motion, output;
    ComPtr<ID3D12Fence> input_fence, output_fence;
    Handle handles[6];
    // Ours.
    ComPtr<ID3D12Resource> last_good, depth_copy, color_copy, target_stage;
    D3D12_RESOURCE_DESC depth_copy_src{}, color_copy_src{}, target_stage_src{};
    bool have_last_good = false;

    std::wstring stem;
    SharedMemory memory;
    Handle record_event, submitted_event, stop_event;
    PROCESS_INFORMATION child{};
    uint64_t startup_tick = 0;
    uint32_t sequence = 0;
    bool pending = false, failed = false;
    uint64_t pending_value = 0;
    double wait_ms = 0;
    uint64_t frames = 0, repeats = 0;
    char reason[192] = {};

    ~Impl()
    {
        // Our queue must be done with everything before it goes.
        if (own && own_value && own->GetCompletedValue() < own_value && own_event) {
            own->SetEventOnCompletion(own_value, own_event.get());
            WaitForSingleObject(own_event.get(), 2000);
        }
        stop_helper();
    }
    void stop_helper()
    {
        if (stop_event) SetEvent(stop_event.get());
        if (child.hProcess) {
            if (WaitForSingleObject(child.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 99);
            CloseHandle(child.hProcess);
        }
        if (child.hThread) CloseHandle(child.hThread);
        child = PROCESS_INFORMATION{};
    }
    const char* set_reason(const char* r)
    {
        CopyReason(reason, r);
        return reason;
    }
    const char* hr_reason(const char* what, HRESULT hr)
    {
        std::snprintf(reason, sizeof(reason), "%s (0x%08X).", what, static_cast<unsigned>(hr));
        return reason;
    }
    Result fail(const char* r)
    {
        set_reason(r);
        failed = true;
        return Result::Failed;
    }

    // ---------------------------------------------------------------- setup
    const char* make_pipeline()
    {
        ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "na_dx12_blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
        if (SUCCEEDED(hr)) hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "na_dx12_blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
        if (FAILED(hr)) return hr_reason("The bridge could not compile its copy shaders", hr);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters = 2;
        rs.pParameters = params;
        ComPtr<ID3DBlob> blob;
        hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        if (FAILED(hr)) return hr_reason("The bridge could not make its root signature", hr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kRing * kViews;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap));
        if (SUCCEEDED(hr)) {
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        }
        if (FAILED(hr)) return hr_reason("The bridge could not make its descriptor heaps", hr);
        srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (auto& a : alloc)
            if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return hr_reason("The bridge could not make its command allocators", hr);
        for (auto* l : {&list_in, &list_out}) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(l->ReleaseAndGetAddressOf()));
            if (SUCCEEDED(hr)) hr = (*l)->Close();
            if (FAILED(hr)) return hr_reason("The bridge could not make its command lists", hr);
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own));
        if (FAILED(hr)) return hr_reason("The bridge could not make its fence", hr);
        own_event = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        return own_event ? nullptr : set_reason("The bridge could not make an event.");
    }

    ID3D12PipelineState* pipeline(DXGI_FORMAT rtv)
    {
        auto& p = pso[rtv];
        if (p) return p.Get();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = rtv;
        d.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p)))) p.Reset();
        return p.Get();
    }

    ComPtr<ID3D12Resource> texture(UINT64 w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, bool shared, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = f;
        rd.SampleDesc.Count = 1;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        if (FAILED(device->CreateCommittedResource(&hp, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    const char* make_shared()
    {
        const SessionLimits& l = cfg.limits;
        const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        color = texture(l.max_render_w, l.max_render_h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt, true, D3D12_RESOURCE_STATE_COMMON);
        depth = texture(l.max_render_w, l.max_render_h, DXGI_FORMAT_R32_FLOAT, rt, true, D3D12_RESOURCE_STATE_COMMON);
        motion = texture(l.max_render_w, l.max_render_h, DXGI_FORMAT_R16G16_FLOAT, rt, true, D3D12_RESOURCE_STATE_COMMON);
        output = texture(l.output_w, l.output_h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true,
                         D3D12_RESOURCE_STATE_COMMON);
        last_good = texture(l.output_w, l.output_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, false, D3D12_RESOURCE_STATE_COMMON);
        if (!color || !depth || !motion || !output || !last_good) return set_reason("The bridge could not make its shared textures.");
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&input_fence));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&output_fence));
        if (FAILED(hr)) return hr_reason("The bridge could not make its shared fences", hr);
        ID3D12DeviceChild* objs[6] = {color.Get(), depth.Get(), motion.Get(), output.Get(), input_fence.Get(), output_fence.Get()};
        const Object names[6] = {Object::Color, Object::Depth, Object::Motion, Object::Output, Object::InputFence, Object::OutputFence};
        for (int i = 0; i < 6; ++i) {
            HANDLE h = nullptr;
            hr = device->CreateSharedHandle(objs[i], nullptr, GENERIC_ALL, ObjectName(stem, names[i]).c_str(), &h);
            if (FAILED(hr)) return hr_reason("The bridge could not share a texture or fence with the helper", hr);
            handles[i].reset(h);
        }
        // The motion texture stays zero (no motion) unless camera motion is built in the helper.
        return clear_motion();
    }

    const char* clear_motion()
    {
        alloc[0]->Reset();
        list_in->Reset(alloc[0].Get(), nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(motion.Get(), nullptr, rtv);
        Transition(list_in.Get(), motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4] = {0, 0, 0, 0};
        list_in->ClearRenderTargetView(rtv, zero, 0, nullptr);
        Transition(list_in.Get(), motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        if (FAILED(list_in->Close())) return set_reason("The bridge could not record its first clear.");
        ID3D12CommandList* lists[1] = {list_in.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(own.Get(), ++own_value);
        slot_value[0] = own_value;
        return nullptr;
    }

    const char* start_helper()
    {
        static volatile LONG session_counter = 0;
        const uint64_t unique = (GetTickCount64() << 16) | (uint64_t(InterlockedIncrement(&session_counter)) & 0xffffu);
        stem = SessionStem(GetCurrentProcessId(), unique);
        if (const char* r = memory.create(ObjectName(stem, Object::Block))) return set_reason(r);
        *memory.block() = SharedBlock{};
        record_event = CreateNamedEvent(ObjectName(stem, Object::RecordEvent), false);
        submitted_event = CreateNamedEvent(ObjectName(stem, Object::SubmittedEvent), false);
        stop_event = CreateNamedEvent(ObjectName(stem, Object::StopEvent), true);
        if (!record_event || !submitted_event || !stop_event) return set_reason("The bridge could not create its events.");
        if (const char* r = make_shared()) return r;

        const LUID luid = device->GetAdapterLuid();
        const SessionLimits& l = cfg.limits;
        std::wstring data = cfg.data_path;
        while (!data.empty() && (data.back() == L'\\' || data.back() == L'/')) data.pop_back();
        if (data.empty()) data = L".";
        std::wstring cmd = L"\"" + cfg.helper_path + L"\" " + stem + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
                           std::to_wstring(luid.LowPart) + L" " + std::to_wstring(luid.HighPart) + L" " + std::to_wstring(l.max_render_w) + L" " +
                           std::to_wstring(l.max_render_h) + L" " + std::to_wstring(l.output_w) + L" " + std::to_wstring(l.output_h) + L" " +
                           std::to_wstring(l.max_render_w) + L" " + std::to_wstring(l.max_render_h) + L" \"" + data + L"\"" +
                           (cfg.copy_test ? L" --copy-test" : L"");
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (!CreateProcessW(cfg.helper_path.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &child))
            return hr_reason("The bridge could not start its DX12 helper (na_bridge_helper64.exe missing or blocked?)", HRESULT_FROM_WIN32(GetLastError()));
        startup_tick = GetTickCount64();
        return nullptr;
    }

    // ---------------------------------------------------------------- per frame
    Result helper_health()
    {
        if (!child.hProcess) return fail("The DX12 helper is not running.");
        const HelperStatus& st = memory.block()->status;
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) {
            char why[192];
            std::snprintf(why, sizeof(why), "The DX12 helper stopped: %s", st.reason[0] ? st.reason : "no reason given");
            return fail(why);
        }
        const auto state = static_cast<HelperState>(st.state);
        if (state == HelperState::Failed || state == HelperState::Stopped) {
            char why[192];
            std::snprintf(why, sizeof(why), "The DX12 helper gave up: %s", st.reason[0] ? st.reason : "no reason given");
            return fail(why);
        }
        if (state != HelperState::Ready) {
            if (GetTickCount64() - startup_tick > cfg.startup_deadline_ms) return fail("The DX12 helper did not start in time.");
            return Result::NotReady;
        }
        return Result::Ok;
    }

    uint64_t helper_submitted() const
    {
        return static_cast<uint64_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&memory.block()->status.last_submitted), 0, 0));
    }

    // Waits (CPU) until this slot's allocator is free again; false on a GPU hang.
    bool begin_slot()
    {
        if (own->GetCompletedValue() < slot_value[slot]) {
            own->SetEventOnCompletion(slot_value[slot], own_event.get());
            if (WaitForSingleObject(own_event.get(), 5000) != WAIT_OBJECT_0) return false;
        }
        alloc[slot]->Reset();
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_at(int i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot * kViews + i) * rtv_step;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(int i) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot * kViews + i) * srv_step;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(int i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot * kViews + i) * srv_step;
        return h;
    }

    // Draws `src` (viewed as `src_view`) into `dst` (viewed as `dst_view`), w x h from the top-left.
    // Both must already be in PIXEL_SHADER_RESOURCE / RENDER_TARGET.
    bool blit(ID3D12GraphicsCommandList* l, int view, ID3D12Resource* src, DXGI_FORMAT src_view, ID3D12Resource* dst, DXGI_FORMAT dst_view,
              uint32_t w, uint32_t h, uint32_t mode)
    {
        ID3D12PipelineState* p = pipeline(dst_view);
        if (!p) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = src_view;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(src, &sd, srv_cpu(view));
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = dst_view;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_at(view);
        device->CreateRenderTargetView(dst, &rd, rtv);
        ID3D12DescriptorHeap* heaps[1] = {srv_heap.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetGraphicsRootSignature(root.Get());
        l->SetPipelineState(p);
        l->SetGraphicsRootDescriptorTable(0, srv_gpu(view));
        l->SetGraphicsRoot32BitConstant(1, mode, 0);
        l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sc);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->DrawInstanced(3, 1, 0, 0);
        return true;
    }

        // A private copy of `src` in `storage` format (made when the source changes).
    bool copy_of(ComPtr<ID3D12Resource>& copy, D3D12_RESOURCE_DESC& made_for, ID3D12Resource* src, DXGI_FORMAT storage, D3D12_RESOURCE_FLAGS flags)
    {
        D3D12_RESOURCE_DESC d = src->GetDesc();
        if (copy && std::memcmp(&made_for, &d, sizeof(d)) == 0) return true;
        copy.Reset();
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC c = d;
        c.Format = storage;
        c.Flags = flags;
        c.MipLevels = 1;
        c.DepthOrArraySize = 1;
        c.SampleDesc = {1, 0}; // a resolve target for MSAA colour; plain copies are 1 sample anyway
        c.Alignment = 0;
        c.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &c, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&copy)))) return false;
        made_for = d;
        return true;
    }

    // Records game colour + depth -> shared colour + depth. Resources come and stay in COMMON.
    const char* record_in(ID3D12GraphicsCommandList* l, const Dx12Inputs& in, const FrameRecord& f)
    {
        const D3D12_RESOURCE_DESC cd = in.color->GetDesc(), dd = in.depth->GetDesc();
        const DXGI_FORMAT cview = ColorView(cd.Format);
        if (cview == DXGI_FORMAT_UNKNOWN) return "The scene colour has an unknown format.";
        DXGI_FORMAT dstore{}, dview{};
        if (!DepthFormats(dd.Format, &dstore, &dview)) return "The scene depth has an unknown format.";
        if (dd.SampleDesc.Count > 1) return "The scene depth is multisampled (not handled yet).";

        // Colour: read directly when allowed, else through a copy (or a resolve for MSAA).
        ID3D12Resource* csrc = in.color;
        const bool c_direct = cd.SampleDesc.Count == 1 && !(cd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        if (!c_direct) {
            if (!copy_of(color_copy, color_copy_src, in.color, cd.SampleDesc.Count > 1 ? cview : cd.Format, D3D12_RESOURCE_FLAG_NONE))
                return "The bridge could not make a readable copy of the scene colour.";
            if (cd.SampleDesc.Count > 1) {
                Transition(l, in.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                Transition(l, color_copy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                l->ResolveSubresource(color_copy.Get(), 0, in.color, 0, cview);
                Transition(l, in.color, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Transition(l, color_copy.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                Transition(l, in.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(l, color_copy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                CopySub0(l, color_copy.Get(), in.color);
                Transition(l, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Transition(l, color_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            csrc = color_copy.Get();
        } else {
            Transition(l, in.color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        // Depth: always through a typeless copy (D3D9On12's depth is a typed depth format, often not shader-readable).
        if (!copy_of(depth_copy, depth_copy_src, in.depth, dstore, D3D12_RESOURCE_FLAG_NONE)) return "The bridge could not make a readable copy of the scene depth.";
        Transition(l, in.depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(l, depth_copy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        CopySub0(l, depth_copy.Get(), in.depth);
        Transition(l, in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(l, depth_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        Transition(l, color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(l, depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const bool ok = blit(l, 0, csrc, cview, color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, f.render_w, f.render_h, kCopy) &&
                        blit(l, 1, depth_copy.Get(), dview, depth.Get(), DXGI_FORMAT_R32_FLOAT, f.render_w, f.render_h, kDepth);
        Transition(l, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Transition(l, depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Transition(l, csrc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(l, depth_copy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        return ok ? nullptr : "The bridge could not make a copy pipeline for these formats.";
    }

    // Records `src` (RGBA16F, output size) -> the game's target. Also keeps it as the last good result.
    const char* record_out(ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* target, bool keep)
    {
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        const DXGI_FORMAT tview = ColorView(td.Format);
        if (tview == DXGI_FORMAT_UNKNOWN || td.SampleDesc.Count != 1) return "The result's target has an unusable format.";
        ID3D12Resource* dst = target;
        const bool direct = (td.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        if (!direct) {
            if (!copy_of(target_stage, target_stage_src, target, td.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
                return "The bridge could not make a staging copy for the result.";
            dst = target_stage.Get();
        }
        Transition(l, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(l, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const bool ok = blit(l, 2, src, DXGI_FORMAT_R16G16B16A16_FLOAT, dst, tview, cfg.limits.output_w, cfg.limits.output_h, cfg.copy_test ? kSwap : kCopy);
        Transition(l, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Transition(l, src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!direct) {
            Transition(l, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(l, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            CopySub0(l, target, dst);
            Transition(l, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Transition(l, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        if (keep && ok) {
            Transition(l, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(l, last_good.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            l->CopyResource(last_good.Get(), src);
            Transition(l, last_good.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Transition(l, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            have_last_good = true;
        }
        return ok ? nullptr : "The bridge could not make a copy pipeline for the result's format.";
    }

    // Executes `l` (closed) and signals our fence; returns the value.
    uint64_t execute(ID3D12GraphicsCommandList* l)
    {
        ID3D12CommandList* lists[1] = {l};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(own.Get(), ++own_value);
        return own_value;
    }

    // Shows the last good result (late frame), or leaves the target alone.
    Result repeat_last(ID3D12Resource* target, uint64_t* value)
    {
        if (!have_last_good) return Result::NotReady;
        list_out->Reset(alloc[slot].Get(), nullptr);
        const char* r = record_out(list_out.Get(), last_good.Get(), target, false);
        if (FAILED(list_out->Close()) || r) {
            set_reason(r ? r : "The bridge could not record the repeat.");
            return Result::NotReady;
        }
        *value = execute(list_out.Get());
        ++repeats;
        return Result::Repeated;
    }
};

Dx12GameBridge::Dx12GameBridge() = default;
Dx12GameBridge::~Dx12GameBridge() { Stop(); }

void Dx12GameBridge::Stop() { impl_.reset(); }
bool Dx12GameBridge::Started() const { return impl_ != nullptr; }
const SessionLimits& Dx12GameBridge::Limits() const
{
    static const SessionLimits none{};
    return impl_ ? impl_->cfg.limits : none;
}

const char* Dx12GameBridge::Start(ID3D12Device* device, ID3D12CommandQueue* queue, const Dx12Config& config)
{
    Stop();
    static thread_local char why[192];
    if (const char* r = ValidateLimits(config.limits)) { CopyReason(why, r); return why; }
    if (!device || !queue) { CopyReason(why, "The bridge needs a D3D12 device and queue."); return why; }
    if (config.helper_path.empty()) { CopyReason(why, "No DX12 helper path was given."); return why; }
    auto s = std::make_unique<Impl>();
    s->cfg = config;
    s->device = device;
    s->queue = queue;
    if (s->cfg.data_path.empty()) {
        wchar_t base[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        s->cfg.data_path = (n && n < MAX_PATH) ? std::wstring(base) + L"\\NeuralAlias\\bridge" : std::wstring(L".");
    }
    std::error_code ec;
    std::filesystem::create_directories(s->cfg.data_path, ec);
    if (const char* r = s->make_pipeline()) { CopyReason(why, r); return why; }
    if (const char* r = s->start_helper()) { CopyReason(why, r); return why; }
    impl_ = std::move(s);
    return nullptr;
}

Result Dx12GameBridge::Process(const Dx12Inputs& in, FrameRecord f, ID3D12Fence** fence, uint64_t* value)
{
    if (!impl_) return Result::Failed;
    Impl& s = *impl_;
    if (s.failed) return Result::Failed;
    if (!in.color || !in.depth || !in.target || !fence || !value) return s.fail("The bridge was called without its inputs.");
    *fence = s.own.Get();
    *value = 0;
    const Result health = s.helper_health();
    if (health != Result::Ok) return health;
    f.sequence = s.sequence;
    if (const char* r = Validate(f, s.cfg.limits, s.sequence)) {
        s.set_reason(r);
        return Result::NotReady;
    }
    const D3D12_RESOURCE_DESC td = in.target->GetDesc();
    if (td.Width != s.cfg.limits.output_w || td.Height != s.cfg.limits.output_h) {
        s.set_reason("The result's target is not the output size; the bridge must restart.");
        return Result::NotReady;
    }
    if (!s.begin_slot()) return s.fail("The bridge's earlier GPU work never finished.");

    // A late frame: nothing may be overwritten until the helper queued it.
    if (s.pending) {
        if (s.helper_submitted() < s.pending_value) {
            const Result r = s.repeat_last(in.target, value);
            s.slot_value[s.slot] = s.own_value;
            s.slot = (s.slot + 1) % kRing;
            return r;
        }
        s.pending = false;
    }

    // The helper must be done with the previous inputs before they are overwritten (GPU wait).
    s.queue->Wait(s.output_fence.Get(), s.helper_submitted());
    s.list_in->Reset(s.alloc[s.slot].Get(), nullptr);
    const char* bad = s.record_in(s.list_in.Get(), in, f);
    if (FAILED(s.list_in->Close()) && !bad) bad = "The bridge could not record its input copies.";
    if (bad) {
        s.set_reason(bad);
        // Nothing was executed: the list is dropped. Show the last good frame instead.
        const Result r = s.repeat_last(in.target, value);
        s.slot_value[s.slot] = s.own_value;
        s.slot = (s.slot + 1) % kRing;
        return r;
    }
    s.execute(s.list_in.Get());
    const uint64_t seq_value = uint64_t(s.sequence) + 1;
    std::memcpy(&s.memory.block()->frame, &f, sizeof(f));
    MemoryBarrier();
    s.queue->Signal(s.input_fence.Get(), seq_value);
    ++s.sequence;
    SetEvent(s.record_event.get());

    // Wait (CPU, short) only until the helper has QUEUED its work.
    const auto t0 = std::chrono::steady_clock::now();
    const auto waited = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    while (s.helper_submitted() < seq_value) {
        const double left = double(s.cfg.submit_deadline_ms) - waited();
        if (left <= 0) break;
        HANDLE hs[2] = {s.submitted_event.get(), s.child.hProcess};
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(left) + 1);
        if (w == WAIT_OBJECT_0 + 1) {
            s.wait_ms = waited();
            s.slot_value[s.slot] = s.own_value;
            s.slot = (s.slot + 1) % kRing;
            *value = s.own_value;
            return s.helper_health();
        }
        if (w != WAIT_OBJECT_0) break;
    }
    s.wait_ms = waited();
    Result result;
    if (s.helper_submitted() < seq_value) {
        s.pending = true;
        s.pending_value = seq_value;
        result = s.repeat_last(in.target, value);
        if (result == Result::NotReady) *value = s.own_value; // the input copies still need a hand-back point
    } else {
        s.queue->Wait(s.output_fence.Get(), seq_value);
        s.list_out->Reset(s.alloc[s.slot].Get(), nullptr);
        const char* r = s.record_out(s.list_out.Get(), s.output.Get(), in.target, true);
        if (FAILED(s.list_out->Close()) || r) {
            s.set_reason(r ? r : "The bridge could not record the result copy.");
            *value = s.own_value;
            result = Result::NotReady;
        } else {
            *value = s.execute(s.list_out.Get());
            ++s.frames;
            result = Result::Ok;
        }
    }
    s.slot_value[s.slot] = s.own_value;
    s.slot = (s.slot + 1) % kRing;
    return result;
}

const char* Dx12GameBridge::Reason() const { return impl_ ? impl_->reason : ""; }
double Dx12GameBridge::LastWaitMs() const { return impl_ ? impl_->wait_ms : 0; }
uint64_t Dx12GameBridge::Frames() const { return impl_ ? impl_->frames : 0; }
uint64_t Dx12GameBridge::Repeats() const { return impl_ ? impl_->repeats : 0; }
uint32_t Dx12GameBridge::HelperMotionFrames() const
{
    if (!impl_ || !impl_->memory.block()) return 0;
    return *reinterpret_cast<volatile uint32_t*>(&impl_->memory.block()->status.motion_frames);
}

} // namespace na::bridge
