#include "host_dx12/diag/gpu_breadcrumbs.hpp"

#include <windows.h>
#include <d3d12.h>

#include <FidelityFX/host/ffx_breadcrumbs.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace na::diag::crumbs {
namespace {
std::mutex g_m;
std::atomic<bool> g_on{false};
bool g_ready = false, g_failed = false;
ID3D12Device* g_device = nullptr; // AddRef'd while bound (never released: the trail must outlive a removal)
FfxBreadcrumbsContext g_ctx;
uint32_t g_queue = 0;
std::unordered_set<ID3D12GraphicsCommandList*> g_registered; // this frame
uint64_t g_frames = 0, g_markers = 0, g_lists = 0, g_errors = 0;
int g_last_error = 0;

// ---- Backend: AMD's DX12 breadcrumbs callbacks (FidelityFX SDK v1.1.4,
// sdk/src/backends/dx12/ffx_dx12.cpp, MIT), adapted to our device handle.
FfxVersionNumber get_sdk_version(FfxInterface*) { return FFX_SDK_MAKE_VERSION(1, 1, 4); }
FfxErrorCode create_backend_context(FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* id)
{
    if (id) *id = 0;
    return static_cast<FfxErrorCode>(FFX_OK);
}
FfxErrorCode destroy_backend_context(FfxInterface*, FfxUInt32) { return FFX_OK; }

// VirtualAlloc + OpenExistingHeapFromAddress + CreatePlacedResource: the buffer survives a TDR.
void alloc_virtual(ID3D12Device3* dev, D3D12_RESOURCE_DESC* rd, FfxBreadcrumbsBlockData* b)
{
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof eh)) || !eh.Supported) return;
    b->memory = VirtualAlloc(nullptr, SIZE_T(rd->Width), MEM_COMMIT, PAGE_READWRITE);
    if (!b->memory) return;
    ID3D12Heap* heap = nullptr;
    if (SUCCEEDED(dev->OpenExistingHeapFromAddress(b->memory, IID_PPV_ARGS(&heap)))) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(dev->CreatePlacedResource(heap, 0, rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)))) {
            r->SetName(L"NeuralAlias breadcrumbs (system memory)");
            b->heap = heap;
            b->buffer = r;
            return;
        }
        heap->Release();
    }
    VirtualFree(b->memory, 0, MEM_RELEASE);
    b->memory = nullptr;
}
FfxErrorCode alloc_block(FfxInterface* bi, uint64_t bytes, FfxBreadcrumbsBlockData* b)
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    auto* dev = static_cast<ID3D12Device*>(bi->device);
    ID3D12Device3* d3 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d3)))) {
        alloc_virtual(d3, &rd, b);
        d3->Release();
    }
    if (!b->buffer) { // readback heap, mapped
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        ID3D12Resource* r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        const D3D12_RANGE none{};
        if (FAILED(r->Map(0, &none, &b->memory))) {
            r->Release();
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
        r->SetName(L"NeuralAlias breadcrumbs (readback)");
        b->buffer = r;
    }
    b->baseAddress = static_cast<ID3D12Resource*>(b->buffer)->GetGPUVirtualAddress();
    return static_cast<FfxErrorCode>(FFX_OK);
}
void free_block(FfxInterface*, FfxBreadcrumbsBlockData* b)
{
    if (b->buffer && !b->heap) {
        if (b->memory) static_cast<ID3D12Resource*>(b->buffer)->Unmap(0, nullptr);
        static_cast<ID3D12Resource*>(b->buffer)->Release();
    } else {
        if (b->buffer) static_cast<ID3D12Resource*>(b->buffer)->Release();
        if (b->heap) static_cast<ID3D12Heap*>(b->heap)->Release();
        if (b->memory) VirtualFree(b->memory, 0, MEM_RELEASE);
    }
    b->buffer = b->heap = b->memory = nullptr;
}
void write_marker(FfxInterface*, FfxCommandList list, uint32_t value, uint64_t gpu, void*, bool begin)
{
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = begin ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{gpu, value};
    ID3D12GraphicsCommandList2* l2 = nullptr;
    if (SUCCEEDED(static_cast<ID3D12GraphicsCommandList*>(list)->QueryInterface(IID_PPV_ARGS(&l2)))) {
        l2->WriteBufferImmediate(1, &p, &mode);
        l2->Release();
    }
}

void* mem_alloc(size_t n) { return std::malloc(n); }
void* mem_realloc(void* p, size_t n) { return std::realloc(p, n); }
void mem_free(void* p) { std::free(p); }

void note_error_locked(FfxErrorCode e)
{
    if (e == static_cast<FfxErrorCode>(FFX_OK)) return;
    ++g_errors;
    g_last_error = int(e);
}

bool bind_locked(ID3D12Device* device)
{
    if (g_ready) return true;
    if (g_failed || !device) return false;
    FfxBreadcrumbsContextDescription d{};
    d.flags = FFX_BREADCRUMBS_ENABLE_THREAD_SYNCHRONIZATION | FFX_BREADCRUMBS_PRINT_SKIP_DEVICE_INFO | FFX_BREADCRUMBS_PRINT_SKIP_PIPELINE_INFO |
              FFX_BREADCRUMBS_PRINT_NOT_STARTED_LISTS;
    d.frameHistoryLength = 4;
    d.maxMarkersPerMemoryBlock = 256;
    d.usedGpuQueuesCount = 1;
    d.pUsedGpuQueues = &g_queue;
    d.allocCallbacks = FfxAllocationCallbacks{&mem_alloc, &mem_realloc, &mem_free};
    d.backendInterface.device = device;
    d.backendInterface.fpGetSDKVersion = &get_sdk_version;
    d.backendInterface.fpCreateBackendContext = &create_backend_context;
    d.backendInterface.fpDestroyBackendContext = &destroy_backend_context;
    d.backendInterface.fpBreadcrumbsAllocBlock = &alloc_block;
    d.backendInterface.fpBreadcrumbsFreeBlock = &free_block;
    d.backendInterface.fpBreadcrumbsWrite = &write_marker;
    std::memset(&g_ctx, 0, sizeof g_ctx);
    const FfxErrorCode e = ffxBreadcrumbsContextCreate(&g_ctx, &d);
    if (e != static_cast<FfxErrorCode>(FFX_OK)) {
        g_failed = true;
        note_error_locked(e);
        return false;
    }
    device->AddRef();
    g_device = device;
    g_ready = true;
    return true;
}
} // namespace

void SetEnabled(bool on) { g_on.store(on, std::memory_order_relaxed); }
bool Enabled() { return g_on.load(std::memory_order_relaxed); }

void OnPresent(ID3D12Device* device)
{
    if (!g_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!bind_locked(device) || device != g_device) return;
    if (device->GetDeviceRemovedReason() != S_OK) return; // keep the trail as it was
    note_error_locked(ffxBreadcrumbsStartFrame(&g_ctx));
    g_registered.clear();
    ++g_frames;
}

Scope::Scope(ID3D12GraphicsCommandList* list, const char* name)
{
    if (!list || !g_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_ready || !g_frames) return;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;
    const bool same = dev == g_device;
    dev->Release();
    if (!same) return;
    if (g_registered.insert(list).second) {
        FfxBreadcrumbsCommandListDescription cd{};
        cd.commandList = list;
        cd.queueType = 0;
        cd.name = FfxBreadcrumbsNameTag{"command list", true};
        const FfxErrorCode e = ffxBreadcrumbsRegisterCommandList(&g_ctx, &cd);
        if (e == static_cast<FfxErrorCode>(FFX_OK)) ++g_lists;
        // Already registered this frame is fine (a list recording several of our passes).
        else if (e != static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT)) { note_error_locked(e); return; }
    }
    const FfxBreadcrumbsNameTag tag{name ? name : "NeuralAlias pass", true};
    const FfxErrorCode e = ffxBreadcrumbsBeginMarker(&g_ctx, list, FFX_BREADCRUMBS_MARKER_PASS, &tag);
    if (e != static_cast<FfxErrorCode>(FFX_OK)) { note_error_locked(e); return; }
    ++g_markers;
    list_ = list;
}

Scope::~Scope()
{
    if (!list_) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_ready) note_error_locked(ffxBreadcrumbsEndMarker(&g_ctx, list_));
}

std::string Status()
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_ready) return {};
    FfxBreadcrumbsMarkersStatus st{};
    if (ffxBreadcrumbsPrintStatus(&g_ctx, &st) != static_cast<FfxErrorCode>(FFX_OK) || !st.pBuffer) return "breadcrumbs: status unavailable\r\n";
    std::string s(st.pBuffer, st.bufferSize && st.pBuffer[st.bufferSize - 1] == 0 ? st.bufferSize - 1 : st.bufferSize);
    mem_free(st.pBuffer);
    return s;
}

std::string StatusLine()
{
    std::lock_guard<std::mutex> lock(g_m);
    char b[200];
    std::snprintf(b, sizeof b, "gpu_breadcrumbs on=%d ready=%d failed=%d frames=%llu lists=%llu markers=%llu errors=%llu last_error=%d",
                  g_on.load() ? 1 : 0, g_ready ? 1 : 0, g_failed ? 1 : 0, (unsigned long long)g_frames, (unsigned long long)g_lists,
                  (unsigned long long)g_markers, (unsigned long long)g_errors, g_last_error);
    return b;
}
} // namespace na::diag::crumbs
