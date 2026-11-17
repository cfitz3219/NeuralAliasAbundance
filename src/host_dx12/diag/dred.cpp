#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/dred.hpp"
#include "host_dx12/diag/gpu_breadcrumbs.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace na::diag {
namespace {
std::mutex g_m;
wchar_t g_dir[MAX_PATH] = {};
std::atomic<bool> g_reported{false};
bool g_page_faults = false;
bool g_breadcrumbs = false;

// D3D12_AUTO_BREADCRUMB_OP 0..44 (d3d12.h order).
const char* const kOps[] = {
    "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
    "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
    "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
    "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64",
    "ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
    "ProcessFrames1", "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
    "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
    "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh",
    "EncodeFrame", "ResolveEncoderOutputMetadata"};

std::string op_name(int op)
{
    if (op >= 0 && op < int(sizeof kOps / sizeof kOps[0])) return kOps[op];
    return "op" + std::to_string(op);
}

std::string narrow(const wchar_t* w, const char* a)
{
    if (w && *w) {
        char buf[256];
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
        if (n > 0) return buf;
    }
    if (a && *a) return a;
    return "(unnamed)";
}

void append(std::string& s, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, size_t(n < int(sizeof buf) ? n : int(sizeof buf) - 1));
}

void allocations(std::string& s, const char* title, const D3D12_DRED_ALLOCATION_NODE* n)
{
    append(s, "  %s:\r\n", title);
    int count = 0;
    for (; n && count < 64; n = n->pNext, ++count)
        append(s, "    %s (type %d)\r\n", narrow(n->ObjectNameW, n->ObjectNameA).c_str(), int(n->AllocationType));
    if (!count) s += "    (none)\r\n";
}
} // namespace

std::string EnableDred(bool page_faults, bool breadcrumbs, const wchar_t* log_dir)
{
    {
        std::lock_guard<std::mutex> lock(g_m);
        lstrcpynW(g_dir, log_dir ? log_dir : L"", MAX_PATH);
    }
    if (!page_faults && !breadcrumbs) return "dred: off";
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(hr) || !settings) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "dred: settings interface unavailable (hr=0x%08lX)", static_cast<unsigned long>(hr));
        return buf;
    }
    // Only ever FORCED_ON for what was asked; the rest is left to the system default
    // (which is off unless a debugger/tool turned it on), so we never turn anything off.
    if (page_faults) settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    if (breadcrumbs) settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    g_page_faults = page_faults;
    g_breadcrumbs = breadcrumbs;
    char buf[96];
    std::snprintf(buf, sizeof buf, "dred: on (page_faults=%d breadcrumbs=%d)", page_faults ? 1 : 0, breadcrumbs ? 1 : 0);
    return buf;
}

std::string DescribeRemoval(ID3D12Device* device)
{
    if (!device) return {};
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (reason == S_OK) return {};
    std::string s;
    append(s, "device removed: reason=0x%08lX\r\n", static_cast<unsigned long>(reason));
    append(s, "dred enabled by host: page_faults=%d breadcrumbs=%d\r\n", g_page_faults ? 1 : 0, g_breadcrumbs ? 1 : 0);
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
        s += "dred: not available on this device (created before DRED was enabled, or old runtime)\r\n";
        return s;
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    const HRESULT pf_hr = dred->GetPageFaultAllocationOutput(&pf);
    if (SUCCEEDED(pf_hr)) {
        append(s, "page fault: va=0x%016llX\r\n", static_cast<unsigned long long>(pf.PageFaultVA));
        allocations(s, "allocations alive at that address", pf.pHeadExistingAllocationNode);
        allocations(s, "allocations recently freed at that address", pf.pHeadRecentFreedAllocationNode);
    } else {
        append(s, "page fault: no data (hr=0x%08lX; off, or not a page fault)\r\n", static_cast<unsigned long>(pf_hr));
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    const HRESULT bc_hr = dred->GetAutoBreadcrumbsOutput(&bc);
    if (FAILED(bc_hr)) {
        append(s, "breadcrumbs: no data (hr=0x%08lX; set [diagnostics] dred_breadcrumbs=1 to record them)\r\n",
               static_cast<unsigned long>(bc_hr));
        return s;
    }
    int lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && lists < 256; n = n->pNext, ++lists) {
        const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        const UINT total = n->BreadcrumbCount;
        // Only lists the GPU had not finished are interesting; finished ones are listed by name.
        if (done >= total) {
            append(s, "list %s on %s: complete (%u ops)\r\n", narrow(n->pCommandListDebugNameW, n->pCommandListDebugNameA).c_str(),
                   narrow(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA).c_str(), total);
            continue;
        }
        append(s, "list %s on %s: NOT finished, %u of %u ops completed\r\n",
               narrow(n->pCommandListDebugNameW, n->pCommandListDebugNameA).c_str(),
               narrow(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA).c_str(), done, total);
        if (!n->pCommandHistory) continue;
        const UINT from = done > 8 ? done - 8 : 0;
        const UINT to = done + 8 < total ? done + 8 : total;
        for (UINT i = from; i < to; ++i)
            append(s, "    %s %4u %s\r\n", i < done ? "done" : i == done ? "-->" : "    ", i, op_name(int(n->pCommandHistory[i])).c_str());
    }
    if (!lists) s += "breadcrumbs: no command lists recorded\r\n";
    return s;
}

void ReportIfRemoved(ID3D12Device* device)
{
    if (!device || g_reported.load(std::memory_order_relaxed)) return;
    if (device->GetDeviceRemovedReason() == S_OK) return;
    if (g_reported.exchange(true)) return;
    const HRESULT removed = device->GetDeviceRemovedReason();
    na::eventlog::Emit("device_removed", "hr=0x%08lX", static_cast<unsigned long>(removed));
    std::string text;
    try {
        char head[200];
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(head, sizeof head, "device removed at %02u:%02u:%02u.%03u tick=%llu frame=%llu hr=0x%08lX\r\n",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, static_cast<unsigned long long>(GetTickCount64()),
                 static_cast<unsigned long long>(na::eventlog::Frame()), static_cast<unsigned long>(removed));
        text = head;
        text += DescribeRemoval(device);
        const std::string trail = na::diag::crumbs::Status();
        if (!trail.empty()) text += "\r\n---- AMD FidelityFX Breadcrumbs (NeuralAlias passes) ----\r\n" + trail;
    } catch (...) {
        text = "device removed: report failed (out of memory)\r\n";
    }
    wchar_t path[MAX_PATH];
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_dir[0]) swprintf_s(path, L"%s\\NeuralAlias-dred-%lu.txt", g_dir, GetCurrentProcessId());
        else swprintf_s(path, L"NeuralAlias-dred-%lu.txt", GetCurrentProcessId());
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
    CloseHandle(f);
}

bool DredReported() { return g_reported.load(); }
} // namespace na::diag
