#include "host_dx12/overlay/usage_sampler.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

namespace na::usage {
namespace {

struct TopThread { DWORD tid = 0; double pct = 0; char name[40] = {}; };

struct Snapshot {
    bool valid = false;
    bool cpu_ok = false, gpu_ok = false;
    double cpu_total = 0;     // whole machine, % (Task Manager style)
    double proc_cores = 0;    // this process, in cores
    double proc_pct = 0;      // this process, % of all logical processors
    double gpu3d_all = 0, gpu3d_ours = 0, gpu_compute_all = 0, gpu_copy_all = 0;
    unsigned logical = 0, threads = 0;
    TopThread top[4];
    // Video memory (2026-11-25: a 35 s drop to a third of the frame rate with the GPU busy and nothing
    // else in the log; a full card does that and nothing recorded it). MB. The card's own memory in use
    // by everything and by this process, the system memory standing in for it, and the amount Windows
    // currently lets this process use on the card: use above budget, or shared use rising, is the sign.
    bool vram_ok = false, budget_ok = false;
    double vram_all = 0, vram_shared_all = 0, vram_ours = 0, vram_shared_ours = 0;
    double vram_card = 0, vram_budget = 0, vram_dxgi_ours = 0;
};

std::mutex g_m;                     // leaked with the process: the sampler never stops
Snapshot g_snap;
std::atomic<bool> g_started{false};

ULONGLONG ft(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

// A busy thread without a description is named by the module its start address lies in
// ("@d3d11.dll", "@NeuralAliasDXGI.dll", "@game.exe"): on a DX11 game through the bridge the
// pinned threads were unnamed (WWII p35760), so the log could not say whose they were.
using NtQueryInformationThreadFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
void thread_module(HANDLE h, char* out, std::size_t size)
{
    static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) return;
    PVOID start = nullptr;
    if (query(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof start, nullptr) < 0 || !start) return;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(start), &m) || !m) return;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
    std::size_t i = 0;
    out[i++] = '@';
    for (; base[i - 1] && i + 1 < size; ++i) out[i] = (base[i - 1] > 32 && base[i - 1] < 127) ? char(base[i - 1]) : '_';
    out[i] = 0;
}
void thread_name(GetThreadDescriptionFn fn, HANDLE h, char* out, std::size_t size)
{
    out[0] = 0;
    PWSTR w = nullptr;
    if (fn && SUCCEEDED(fn(h, &w)) && w) {
        std::size_t i = 0;
        for (; w[i] && i + 1 < size; ++i) out[i] = (w[i] > 32 && w[i] < 127) ? char(w[i]) : '_';
        out[i] = 0;
        LocalFree(w);
    }
    if (!out[0]) thread_module(h, out, size);
}

DWORD WINAPI sampler(void*)
{
    const DWORD pid = GetCurrentProcessId();
    const unsigned logical = (std::max)(1u, unsigned(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    const auto describe = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER cpu = nullptr, gpu = nullptr;
    PDH_HCOUNTER mem[4] = {}; // adapter dedicated, adapter shared, process dedicated, process shared
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &cpu) != ERROR_SUCCESS)
            if (PdhAddEnglishCounterW(q, L"\\Processor(_Total)\\% Processor Time", 0, &cpu) != ERROR_SUCCESS) cpu = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu) != ERROR_SUCCESS) gpu = nullptr;
        static const wchar_t* const kMem[4] = {L"\\GPU Adapter Memory(*)\\Dedicated Usage", L"\\GPU Adapter Memory(*)\\Shared Usage",
                                               L"\\GPU Process Memory(*)\\Dedicated Usage", L"\\GPU Process Memory(*)\\Shared Usage"};
        for (int i = 0; i < 4; ++i)
            if (PdhAddEnglishCounterW(q, kMem[i], 0, &mem[i]) != ERROR_SUCCESS) mem[i] = nullptr;
        PdhCollectQueryData(q);
    }
    // The card with the most memory of its own: its size, and this process's budget on it.
    IDXGIAdapter3* card = nullptr;
    double card_mb = 0;
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && double(d.DedicatedVideoMemory) / 1048576.0 > card_mb) {
                    IDXGIAdapter3* a3 = nullptr;
                    if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
                        if (card) card->Release();
                        card = a3;
                        card_mb = double(d.DedicatedVideoMemory) / 1048576.0;
                    }
                }
                a->Release();
                a = nullptr;
            }
            factory->Release();
        }
    }
    wchar_t ours[32];
    swprintf(ours, 32, L"pid_%lu_", static_cast<unsigned long>(pid));

    std::unordered_map<DWORD, ULONGLONG> last_thread;
    ULONGLONG last_wall = GetTickCount64(), last_proc = 0;
    {
        FILETIME c, e, k, u;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) last_proc = ft(k) + ft(u);
    }
    std::vector<BYTE> buf;
    bool first = true;

    for (;;) {
        Sleep(5000);
        Snapshot s;
        s.logical = logical;
        const ULONGLONG wall = GetTickCount64();
        const double wall_100ns = double(wall - last_wall) * 10000.0;
        last_wall = wall;
        if (wall_100ns <= 0) continue;

        FILETIME c, e, k, u;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            const ULONGLONG p = ft(k) + ft(u);
            s.proc_cores = double(p - last_proc) / wall_100ns;
            s.proc_pct = 100.0 * s.proc_cores / logical;
            last_proc = p;
        }

        // Busiest threads of this process.
        std::unordered_map<DWORD, ULONGLONG> now_thread;
        std::vector<TopThread> all;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); // start address needs the full query right (own process)
                if (!h) h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                if (GetThreadTimes(h, &c, &e, &k, &u)) {
                    const ULONGLONG t = ft(k) + ft(u);
                    now_thread[te.th32ThreadID] = t;
                    const auto it = last_thread.find(te.th32ThreadID);
                    if (it != last_thread.end() && t >= it->second) {
                        TopThread tt;
                        tt.tid = te.th32ThreadID;
                        tt.pct = 100.0 * double(t - it->second) / wall_100ns;
                        if (tt.pct >= 1.0) thread_name(describe, h, tt.name, sizeof tt.name);
                        all.push_back(tt);
                    }
                }
                CloseHandle(h);
            }
            CloseHandle(snap);
        }
        s.threads = unsigned(now_thread.size());
        last_thread.swap(now_thread);
        std::sort(all.begin(), all.end(), [](const TopThread& a, const TopThread& b) { return a.pct > b.pct; });
        for (std::size_t i = 0; i < all.size() && i < 4; ++i) s.top[i] = all[i];

        if (q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE v{};
            if (cpu && PdhGetFormattedCounterValue(cpu, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS &&
                v.CStatus == ERROR_SUCCESS) {
                s.cpu_ok = true;
                s.cpu_total = v.doubleValue;
            }
            DWORD bytes = 0, count = 0;
            if (gpu && PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, nullptr) == PDH_MORE_DATA && bytes) {
                buf.resize(bytes);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
                if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
                    s.gpu_ok = true;
                    for (DWORD i = 0; i < count; ++i) {
                        if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName) continue;
                        const double x = items[i].FmtValue.doubleValue;
                        const wchar_t* n = items[i].szName;
                        if (std::wcsstr(n, L"engtype_3D")) {
                            s.gpu3d_all += x;
                            if (std::wcsstr(n, ours)) s.gpu3d_ours += x;
                        } else if (std::wcsstr(n, L"engtype_Compute")) s.gpu_compute_all += x;
                        else if (std::wcsstr(n, L"engtype_Copy")) s.gpu_copy_all += x;
                    }
                }
            }
        }
        if (q) {
            double* const all_out[2] = {&s.vram_all, &s.vram_shared_all};
            double* const ours_out[2] = {&s.vram_ours, &s.vram_shared_ours};
            for (int m = 0; m < 4; ++m) {
                DWORD bytes = 0, count = 0;
                if (!mem[m] || PdhGetFormattedCounterArrayW(mem[m], PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA || !bytes) continue;
                buf.resize(bytes);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
                if (PdhGetFormattedCounterArrayW(mem[m], PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) continue;
                s.vram_ok = true;
                double sum = 0, largest = 0;
                for (DWORD i = 0; i < count; ++i) {
                    if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName) continue;
                    const double mb = items[i].FmtValue.doubleValue / 1048576.0;
                    if (m < 2) largest = (std::max)(largest, mb); // one row per adapter: the busiest card
                    else if (std::wcsstr(items[i].szName, ours)) sum += mb;
                }
                if (m < 2) *all_out[m] = largest; else *ours_out[m - 2] = sum;
            }
        }
        if (card) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(card->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                s.budget_ok = true;
                s.vram_card = card_mb;
                s.vram_budget = double(info.Budget) / 1048576.0;
                s.vram_dxgi_ours = double(info.CurrentUsage) / 1048576.0;
            }
        }
        if (first) { first = false; continue; } // thread deltas need two samples
        s.valid = true;
        std::lock_guard<std::mutex> lock(g_m);
        g_snap = s;
    }
}

} // namespace

void Start()
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    HANDLE h = CreateThread(nullptr, 0, sampler, nullptr, 0, nullptr);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
    }
}

int FormatLine(char* out, std::size_t size)
{
    if (!g_started.load(std::memory_order_relaxed) || !size) return 0;
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(g_m);
        s = g_snap;
    }
    if (!s.valid) return 0;
    char top[200] = {};
    int off = 0;
    for (const TopThread& t : s.top) {
        if (!t.tid) break;
        const int n = std::snprintf(top + off, sizeof top - std::size_t(off), "%s%lu:%.0f%%:%s", off ? "," : "",
                                    static_cast<unsigned long>(t.tid), t.pct, t.name[0] ? t.name : "-");
        if (n <= 0 || off + n >= int(sizeof top)) break;
        off += n;
    }
    return std::snprintf(out, size,
        "usage cpu_ok=%d cpu_total=%.1f%% proc_cores=%.2f proc_pct=%.1f%% logical=%u threads=%u top_threads=%s "
        "gpu_ok=%d gpu3d_all=%.1f%% gpu3d_ours=%.1f%% gpu_compute_all=%.1f%% gpu_copy_all=%.1f%% | "
        "video_memory ok=%d card_mb=%.0f in_use_all_mb=%.0f in_use_process_mb=%.0f shared_all_mb=%.0f shared_process_mb=%.0f "
        "budget_ok=%d budget_mb=%.0f process_against_budget_mb=%.0f state=%s\r\n",
        s.cpu_ok ? 1 : 0, s.cpu_total, s.proc_cores, s.proc_pct, s.logical, s.threads, top[0] ? top : "-",
        s.gpu_ok ? 1 : 0, s.gpu3d_all, s.gpu3d_ours, s.gpu_compute_all, s.gpu_copy_all,
        s.vram_ok ? 1 : 0, s.vram_card, s.vram_all, s.vram_ours, s.vram_shared_all, s.vram_shared_ours,
        s.budget_ok ? 1 : 0, s.vram_budget, s.vram_dxgi_ours,
        !s.budget_ok ? "unknown" : s.vram_dxgi_ours > s.vram_budget ? "OVER_BUDGET: the card is full, pictures are being moved to system memory"
        : s.vram_dxgi_ours > 0.92 * s.vram_budget ? "near_budget" : "within_budget");
}

} // namespace na::usage
