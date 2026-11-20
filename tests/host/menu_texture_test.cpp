// Home menu flicker (Cort 2026-11-20): the overlay skipped the whole menu draw
// whenever ImGui had a font-image request (new glyphs, a new text size, a grown
// image) while an earlier menu draw was still on the GPU, so new text made the
// menu vanish for a few presents. This test drives the real ImGui DX12 backend
// with the overlay's MenuTextureKeeper on a real D3D12 device (hardware or
// WARP), holds an earlier menu draw on the GPU with a queue wait, and checks:
//  1. the scenario reproduces the old skip condition (request pending + draw in flight);
//  2. the keeper lets the draw go ahead, on a fresh image, without writing the old one;
//  3. the old image stays alive until the held draw completes, then is released;
//  4. a grown/repacked image (WantDestroy) is detached the same way;
//  5. the new text actually reaches the render target and the device is healthy.
#include "test_support.hpp"
#include "host_dx12/overlay/menu_textures.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <vector>

using test::ComPtr;

namespace {
constexpr UINT kW = 512, kH = 256, kSlots = 8, kSrv = 64;
ComPtr<ID3D12DescriptorHeap> g_srv;
UINT g_srv_stride = 0;
bool g_srv_used[kSrv] = {};

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    for (UINT i = 0; i < kSrv; ++i) if (!g_srv_used[i]) {
        g_srv_used[i] = true;
        cpu->ptr = g_srv->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g_srv_stride;
        gpu->ptr = g_srv->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g_srv_stride;
        return;
    }
    cpu->ptr = 0; gpu->ptr = 0;
}
void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    const SIZE_T base = g_srv->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr >= base && (cpu.ptr - base) / g_srv_stride < kSrv) g_srv_used[(cpu.ptr - base) / g_srv_stride] = false;
}
UINT SrvInUse() { UINT n = 0; for (bool u : g_srv_used) n += u ? 1u : 0u; return n; }

struct Slot { test::List list; ComPtr<ID3D12Fence> fence; UINT64 value = 0; };

struct Menu {
    test::Device& d;
    ComPtr<ID3D12CommandQueue> upload; // the overlay's private texture-upload queue
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    Slot slots[kSlots];
    UINT64 next = 1, draws = 0;
    na::overlay::MenuTextureKeeper keeper;
    // What the last frame saw before the keeper acted (the old code's skip condition).
    bool saw_request_with_draw_in_flight = false, saw_destroy_request = false, skipped = false;

    explicit Menu(test::Device& dev) : d(dev) {}

    std::vector<na::overlay::MenuDrawInFlight> InFlight(bool& unreadable) const
    {
        std::vector<na::overlay::MenuDrawInFlight> v;
        unreadable = false; // every Signal here succeeds
        for (const Slot& s : slots)
            if (s.value && s.fence->GetCompletedValue() < s.value) v.push_back({s.fence, s.value});
        return v;
    }

    // One overlay frame: build, keeper, record, submit. `hold`: the queue waits on `gate`
    // first, so this draw (and later ones) stay on the GPU until the CPU opens the gate.
    void Frame(void (*build)(), ID3D12Fence* gate = nullptr, UINT64 gate_value = 0)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(kW), float(kH));
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(float(kW), float(kH)));
        ImGui::Begin("menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        build();
        ImGui::End();
        ImGui::Render();
        ImDrawData* draw = ImGui::GetDrawData();

        bool unreadable = false;
        const auto in_flight = InFlight(unreadable);
        saw_request_with_draw_in_flight = saw_destroy_request = false;
        for (ImTextureData* t : *draw->Textures) {
            if (t->Status != ImTextureStatus_OK && !in_flight.empty()) saw_request_with_draw_in_flight = true;
            if (t->Status == ImTextureStatus_WantDestroy && t->BackendUserData) saw_destroy_request = true;
        }
        keeper.Collect(in_flight.empty() && !unreadable);
        skipped = !keeper.Prepare(draw->Textures, in_flight, unreadable);
        if (skipped) return;

        Slot& s = slots[draws % kSlots];
        if (s.value && s.fence->GetCompletedValue() < s.value) { std::printf("FATAL slot reused while in flight\n"); std::exit(1); }
        s.list.Reset();
        auto* list = s.list.list.Get();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {0, 0, 0, 0};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srv.Get()};
        list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(draw, list);
        ++draws;
        list->Close();
        if (gate) d.queue->Wait(gate, gate_value);
        ID3D12CommandList* lists[] = {list};
        d.queue->ExecuteCommandLists(1, lists);
        s.value = next++;
        d.queue->Signal(s.fence.Get(), s.value);
    }

    // Counts lit pixels in the render target (waits for the GPU).
    UINT LitPixels()
    {
        d.WaitIdle();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes = 0;
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        d.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto rb = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        test::List l;
        if (!rb || !l.Create(d.device.Get())) return 0;
        auto b = test::Transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b = test::Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        l.list->ResourceBarrier(1, &b);
        l.list->Close();
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        void* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, &mapped)) || !mapped) return 0;
        const uint8_t* p = static_cast<const uint8_t*>(mapped);
        UINT lit = 0;
        for (UINT y = 0; y < kH; ++y)
            for (UINT x = 0; x < kW; ++x) if (p[y * fp.Footprint.RowPitch + x * 4 + 3] > 64) ++lit;
        rb->Unmap(0, nullptr);
        return lit;
    }
};

void Small() { ImGui::TextUnformatted("Home"); }
void NewText()
{
    ImGui::PushFont(nullptr, 24.0f); // a size not baked yet: new glyphs into the existing font image
    ImGui::TextUnformatted("Sharpness 75%");
    ImGui::PopFont();
}
int g_grow_size = 30;
void Grow()
{
    // Many large sizes at once: the font image runs out of room and is regrown/repacked.
    for (int i = 0; i < 6; ++i) {
        ImGui::PushFont(nullptr, float(g_grow_size + i * 7));
        ImGui::TextUnformatted("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
        ImGui::PopFont();
    }
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("menu_texture_test");
    Menu m(d);
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m.upload))));
    D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = kSrv;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv))));
    g_srv_stride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 1;
    REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m.rtv_heap))));
    m.target = test::CreateTexture(d.device.Get(), kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(m.target);
    d.device->CreateRenderTargetView(m.target.Get(), nullptr, m.rtv_heap->GetCPUDescriptorHandleForHeapStart());
    for (Slot& s : m.slots) {
        REQUIRE(s.list.Create(d.device.Get()));
        s.list.list->Close();
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))));
    }
    ComPtr<ID3D12Fence> gate;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    ImGui_ImplDX12_InitInfo info{};
    info.Device = d.device.Get();
    info.CommandQueue = m.upload.Get();
    info.NumFramesInFlight = int(kSlots);
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_srv.Get();
    info.SrvDescriptorAllocFn = &SrvAlloc;
    info.SrvDescriptorFreeFn = &SrvFree;
    REQUIRE(ImGui_ImplDX12_Init(&info));
    REQUIRE(ImGui_ImplDX12_CreateDeviceObjects());

    // Warm up: the basic menu text is baked and on the GPU; nothing in flight.
    for (int i = 0; i < 3; ++i) m.Frame(&Small);
    d.WaitIdle();
    const UINT srv_baseline = SrvInUse();
    ImTextureData* atlas = io.Fonts->TexData;
    REQUIRE(atlas && atlas->Status == ImTextureStatus_OK);
    const ImTextureID image_before = atlas->TexID;

    // 1-3: an earlier menu draw is held on the GPU, then new text appears.
    UINT64 gate_value = 1;
    m.Frame(&Small, gate.Get(), gate_value);
    CHECK(!m.skipped);
    m.Frame(&NewText);
    std::printf("new text: request with draw in flight=%d skipped=%d fresh_copies=%llu waiting=%zu\n",
                m.saw_request_with_draw_in_flight ? 1 : 0, m.skipped ? 1 : 0,
                static_cast<unsigned long long>(m.keeper.GetStats().fresh_copies.load()), m.keeper.Pending());
    CHECK(m.saw_request_with_draw_in_flight); // the old code skipped exactly this draw
    CHECK(!m.skipped);                        // the keeper draws it
    CHECK(m.keeper.GetStats().detached.load() >= 1);
    CHECK(io.Fonts->TexData->Status == ImTextureStatus_OK);
    CHECK(m.keeper.GetStats().fresh_copies.load() >= 1); // the glyph update went to a fresh image...
    CHECK(io.Fonts->TexData->TexID != image_before);      // ...and the held draw keeps the old one
    CHECK(m.keeper.Pending() >= 1);
    m.keeper.Collect(false);
    CHECK(m.keeper.Pending() >= 1); // still held: never released under a running draw
    CHECK(SrvInUse() > srv_baseline);

    // Open the gate: the held draws finish; the old image is released by fence.
    CHECK(SUCCEEDED(gate->Signal(gate_value)));
    d.WaitIdle();
    m.keeper.Collect(false);
    CHECK(m.keeper.Pending() == 0);
    CHECK(m.keeper.GetStats().released.load() == m.keeper.GetStats().detached.load());
    CHECK(SrvInUse() == srv_baseline);
    const UINT lit_new = m.LitPixels();
    std::printf("lit pixels after the new-text frame: %u\n", lit_new);
    CHECK(lit_new > 100); // the 24 px text was drawn

    // 4: grow the font image while a draw is held; repeat until ImGui retires an image.
    bool saw_destroy = false;
    for (int round = 0; round < 12 && !saw_destroy; ++round) {
        ++gate_value;
        m.Frame(&Small, gate.Get(), gate_value);
        g_grow_size = 30 + round * 12;
        for (int f = 0; f < 3; ++f) {
            m.Frame(&Grow);
            CHECK(!m.skipped);
            saw_destroy = saw_destroy || m.saw_destroy_request;
        }
        CHECK(SUCCEEDED(gate->Signal(gate_value)));
        d.WaitIdle();
        m.keeper.Collect(false);
        CHECK(m.keeper.Pending() == 0);
    }
    std::printf("grow: retired-image request seen=%d fresh_copies=%llu detached=%llu released=%llu refused=%llu atlas=%dx%d\n",
                saw_destroy ? 1 : 0, static_cast<unsigned long long>(m.keeper.GetStats().fresh_copies.load()),
                static_cast<unsigned long long>(m.keeper.GetStats().detached.load()),
                static_cast<unsigned long long>(m.keeper.GetStats().released.load()),
                static_cast<unsigned long long>(m.keeper.GetStats().refused.load()),
                io.Fonts->TexData->Width, io.Fonts->TexData->Height);
    CHECK(saw_destroy);
    CHECK(m.keeper.GetStats().refused.load() == 0);
    CHECK(m.LitPixels() > 1000);
    CHECK(d.device->GetDeviceRemovedReason() == S_OK);

    d.WaitIdle();
    m.keeper.Collect(true);
    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext();
    return test::finish("menu_texture_test");
}
