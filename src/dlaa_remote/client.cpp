#include "client.hpp"
#include "channel.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace na::dlaa_remote {
namespace {
using Microsoft::WRL::ComPtr;

// DLSS loads its model inside the first frame's request; that takes seconds, not milliseconds.
constexpr DWORD kStartTimeoutMs = 60000, kFrameTimeoutMs = 20000, kBindTimeoutMs = 5000;

struct Client {
    Channel channel;
    HANDLE process = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ready, done;
    uint64_t value = 0;
    Stats stats;
};
Client& C() { static auto* c = new Client; return *c; }
std::mutex g_m;

bool Fail(Client& c, const char* why)
{
    c.stats.failed = true;
    c.stats.available = false;
    strncpy_s(c.stats.reason, why, _TRUNCATE);
    return false;
}

const char* Why(Channel::Result r)
{
    switch (r) {
    case Channel::Result::Timeout: return "The DLAA companion stopped answering.";
    case Channel::Result::PeerGone: return "The DLAA companion program ended unexpectedly.";
    default: return "The message path to the DLAA companion failed.";
    }
}

// A handle of ours to a shared GPU object, copied into the companion. 0 on failure.
uint64_t Give(Client& c, ID3D12DeviceChild* object)
{
    HANDLE local = nullptr, remote = nullptr;
    if (!object || FAILED(c.device->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &local))) return 0;
    const BOOL ok = DuplicateHandle(GetCurrentProcess(), local, c.process, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(local);
    return ok ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote)) : 0;
}

std::wstring BesideThisModule(const wchar_t* file)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&BesideThisModule), &self);
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring s(path);
    const auto slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1)) + file;
}
} // namespace

bool Wanted()
{
#if !defined(_WIN64)
    return true;
#else
    wchar_t v[8]{};
    const DWORD n = GetEnvironmentVariableW(L"NA_DLAA_COMPANION", v, 8);
    return n == 1 && v[0] == L'1';
#endif
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_m);
    return C().stats;
}

int FormatStatus(char* out, size_t capacity)
{
    const Stats s = GetStats();
    const unsigned long long avg = s.frames ? s.total_wait_us / s.frames : 0;
    return std::snprintf(out, capacity,
        "dlaa_companion started=%d available=%d failed=%d frames=%llu refused=%llu stale=%llu handoff_us{last=%u avg=%llu max=%u companion_last=%u} \"%s\"",
        s.started, s.available, s.failed, static_cast<unsigned long long>(s.frames),
        static_cast<unsigned long long>(s.refused), static_cast<unsigned long long>(s.stale), s.last_wait_us, avg, s.max_wait_us, s.last_companion_us, s.reason);
}

bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, const wchar_t* data_path, const wchar_t* exe, bool copy_for_tests,
           uint32_t stale_every_for_tests)
{
    std::lock_guard<std::mutex> lock(g_m);
    Client& c = C();
    if (c.stats.failed) return false; // one attempt per session; the reason is kept
    if (c.stats.started) return c.stats.available && c.device.Get() == device;
    if (!device || !queue) return Fail(c, "No DirectX 12 device for the DLAA companion.");
    c.device = device;
    c.queue = queue;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.ready))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.done))))
        return Fail(c, "The shared frame fences for the DLAA companion could not be created.");
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\NeuralAlias-dlaa-%lu-%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    if (!c.channel.Create(name)) return Fail(c, "The message path to the DLAA companion could not be created.");
    const std::wstring program = exe && *exe ? std::wstring(exe) : BesideThisModule(L"NeuralAliasDlaa64.exe");
    if (program.empty() || GetFileAttributesW(program.c_str()) == INVALID_FILE_ATTRIBUTES)
        return Fail(c, "NeuralAliasDlaa64.exe (the 64-bit DLAA companion) is not next to the program.");
    std::wstring command = L"\"" + program + L"\" " + name + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return Fail(c, "Windows refused to start the 64-bit DLAA companion.");
    CloseHandle(pi.hThread);
    c.process = pi.hProcess;
    c.stats.started = true;

    Request request{};
    request.kind = Kind::Hello;
    const LUID luid = device->GetAdapterLuid();
    request.hello.adapter_luid_low = luid.LowPart;
    request.hello.adapter_luid_high = luid.HighPart;
    request.hello.ready_fence = Give(c, c.ready.Get());
    request.hello.done_fence = Give(c, c.done.Get());
    request.hello.copy_for_tests = copy_for_tests ? 1u : 0u;
    request.hello.stale_every_for_tests = stale_every_for_tests;
    for (size_t i = 0; data_path && i < 259 && data_path[i]; ++i) request.hello.data_path[i] = static_cast<char16_t>(data_path[i]);
    if (!request.hello.ready_fence || !request.hello.done_fence) return Fail(c, "The frame fences could not be shared with the DLAA companion.");
    Reply reply{};
    const auto r = c.channel.Call(request, &reply, c.process, kStartTimeoutMs);
    if (r != Channel::Result::Ok) return Fail(c, Why(r));
    if (!reply.ok || !reply.available) return Fail(c, reply.reason[0] ? reply.reason : "DLSS is not available in the DLAA companion.");
    c.stats.available = true;
    strncpy_s(c.stats.reason, "DLAA runs in the 64-bit companion.", _TRUNCATE);
    return true;
}

bool Bind(uint32_t slot, uint32_t width, uint32_t height, ID3D12Resource* color, ID3D12Resource* depth,
          ID3D12Resource* motion, ID3D12Resource* output)
{
    std::lock_guard<std::mutex> lock(g_m);
    Client& c = C();
    if (!c.stats.available) return false;
    Request request{};
    request.kind = Kind::Bind;
    request.bind.slot = slot;
    request.bind.width = width;
    request.bind.height = height;
    request.bind.color = Give(c, color);
    request.bind.depth = Give(c, depth);
    request.bind.motion = Give(c, motion);
    request.bind.output = Give(c, output);
    if (!request.bind.color || !request.bind.depth || !request.bind.motion || !request.bind.output)
        return Fail(c, "A frame texture could not be shared with the DLAA companion.");
    Reply reply{};
    const auto r = c.channel.Call(request, &reply, c.process, kBindTimeoutMs);
    if (r != Channel::Result::Ok) return Fail(c, Why(r));
    if (!reply.ok) return Fail(c, reply.reason[0] ? reply.reason : "The DLAA companion could not open the frame textures.");
    return true;
}

bool Evaluate(Frame frame, Handoff* handoff)
{
    std::lock_guard<std::mutex> lock(g_m);
    Client& c = C();
    if (!c.queue) return false;
    // The value is taken even when the companion is gone, so the caller's later work, which this
    // function always orders after `done`, is never left waiting.
    frame.value = ++c.value;
    c.queue->Signal(c.ready.Get(), frame.value);
    bool ran = false;
    Handoff seen;
    seen.asked = frame.value;
    seen.asked_slot = frame.slot;
    if (c.stats.available) {
        Request request{};
        request.kind = Kind::Frame;
        request.frame = frame;
        Reply reply{};
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const auto r = c.channel.Call(request, &reply, c.process, kFrameTimeoutMs);
        QueryPerformanceCounter(&t1);
        if (r != Channel::Result::Ok) {
            Fail(c, Why(r));
        } else {
            const auto us = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
            c.stats.last_wait_us = us;
            c.stats.max_wait_us = (std::max)(c.stats.max_wait_us, us);
            c.stats.total_wait_us += us;
            c.stats.last_companion_us = reply.micros;
            ++c.stats.frames;
            seen.replied = true;
            seen.answered = reply.value;
            seen.answered_slot = reply.slot;
            ran = reply.ok != 0;
            if (reply.value != frame.value || reply.slot != frame.slot) {
                // An answer to another frame: this frame's picture is whatever was there before.
                ran = false;
                ++c.stats.stale;
                strncpy_s(c.stats.reason, "The DLAA companion answered a different frame; this frame was not used.", _TRUNCATE);
            } else if (!ran) {
                ++c.stats.refused;
                strncpy_s(c.stats.reason, reply.reason[0] ? reply.reason : "DLSS refused this frame.", _TRUNCATE);
            }
        }
    }
    // The companion signals `done` itself for every frame it received. When it never received this
    // one, or is gone, nothing else will: release the value here.
    if (c.stats.failed && c.done->GetCompletedValue() < frame.value) c.done->Signal(frame.value);
    c.queue->Wait(c.done.Get(), frame.value);
    seen.ran = ran;
    if (handoff) *handoff = seen;
    return ran;
}

void Stop()
{
    std::lock_guard<std::mutex> lock(g_m);
    Client& c = C();
    if (c.process) {
        if (c.stats.available) {
            Request request{};
            request.kind = Kind::Bye;
            c.channel.Call(request, nullptr, c.process, 2000);
        }
        if (WaitForSingleObject(c.process, 2000) != WAIT_OBJECT_0) TerminateProcess(c.process, 0);
        CloseHandle(c.process);
        c.process = nullptr;
    }
    c.channel.Close();
    c.ready.Reset(); c.done.Reset(); c.queue.Reset(); c.device.Reset();
    c.stats = Stats{};
    c.value = 0;
}

} // namespace na::dlaa_remote
