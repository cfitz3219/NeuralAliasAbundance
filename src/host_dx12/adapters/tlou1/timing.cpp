#include "host_dx12/adapters/tlou1/timing.hpp"
#include "host_dx12/adapters/tlou1/present_batch.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"
#include "host_dx12/latency/pcl_stats.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace na::tlou1 {
namespace {
using Job = void(*)(void*);
using Call = void(*)();
Job simulation = nullptr, render_job = nullptr;
Call input = nullptr, kick_render = nullptr;
using SubmitBatch = uintptr_t(*)(void*,void*,void*);
SubmitBatch submit_batch=nullptr;
thread_local uint64_t present_id=0;
thread_local bool in_submit_batch=false;
using FrameParams = void*(*)(uint64_t);
FrameParams frame_params=nullptr;
uintptr_t present_frame_return=0;
void* frame_params_detour(uint64_t frame) {
    // Read the exact frame passed by the Present record as it is consumed,
    // not an entry-time scan of the worker's mutable batch.
    if(in_submit_batch && reinterpret_cast<uintptr_t>(_ReturnAddress())==present_frame_return)
        present_id=frame==UINT64_MAX ? 0 : frame+1;
    return frame_params(frame);
}
uintptr_t submit_batch_detour(void* a,void* batch,void* c) {
    const auto previous=present_id;
    const auto previous_scope=in_submit_batch;
    in_submit_batch=true; present_id=0;
    const auto result=submit_batch(a,batch,c);
    present_id=previous; in_submit_batch=previous_scope;
    return result;
}
thread_local uint64_t render_id = 0;
std::atomic<uint64_t> simulation_id{0};
std::atomic<bool> attached{false};
std::mutex trace_mutex; TimingTrace trace;
// This build's game loop waits for its simulation job before advancing its
// engine frame number. PreProcessUpdate consumes input before rendering starts.
void simulation_detour(void* context) {
    const uint64_t id = context ? *static_cast<const uint64_t*>(context) + 1 : 0;
    simulation_id.store(id, std::memory_order_release);
    na::latency::EngineBegin(id);
    simulation(context);
    simulation_id.store(0, std::memory_order_release);
}
void input_detour() {
    na::latency::EngineMark(simulation_id.load(std::memory_order_acquire), na::latency::Phase::Input);
    input();
}
void kick_detour() {
    const auto id = simulation_id.load(std::memory_order_acquire);
    na::latency::EngineMark(id, na::latency::Phase::SimulationEnd);
    kick_render();
}
void render_detour(void* context) {
    const auto previous = render_id;
    render_id = context ? *static_cast<const uint64_t*>(context) + 1 : 0;
    na::latency::EngineMark(render_id, na::latency::Phase::Render);
    render_job(context);
    render_id = previous;
}
bool pinned_executable() {
    wchar_t path[32768]{};
    auto n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n == 32768) return false;
    auto name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    if (_wcsicmp(name, L"tlou-i.exe")) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> bytes{};
    DWORD read = 0;
    while (ok) {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, bytes.data(), read, 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    constexpr unsigned char expected[] = {0x08,0x8d,0x37,0xaf,0xf3,0xb8,0xe3,0xf6,0x57,0x15,0xe7,0xe3,0x25,0x54,0x7c,0x07,0xe6,0xc4,0xd7,0x94,0x96,0x6c,0x05,0x73,0xcd,0x0f,0x7a,0xf5,0xc6,0xc3,0x50,0x3a};
    return ok && !std::memcmp(expected, digest.data(), sizeof(expected));
}
}
void InitializeTiming() {
    if (attached.load() || !pinned_executable()) return;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    present_frame_return=base+0x21930cd;
    if (!na::hooking::BeginInstall()) return;
    bool ok = na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x11df208), reinterpret_cast<void*>(&simulation_detour), reinterpret_cast<void**>(&simulation)) &&
        na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x1289078), reinterpret_cast<void*>(&input_detour), reinterpret_cast<void**>(&input)) &&
        na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x11c41e4), reinterpret_cast<void*>(&kick_detour), reinterpret_cast<void**>(&kick_render)) &&
        na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x11c8024), reinterpret_cast<void*>(&render_detour), reinterpret_cast<void**>(&render_job)) &&
        na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x2192fa4), reinterpret_cast<void*>(&submit_batch_detour), reinterpret_cast<void**>(&submit_batch)) &&
        na::hooking::PrepareHook(reinterpret_cast<void*>(base + 0x1ec5fa4), reinterpret_cast<void*>(&frame_params_detour), reinterpret_cast<void**>(&frame_params));
    if (!ok) { na::hooking::AbandonInstall(); return; }
    attached.store(na::hooking::CommitInstall(), std::memory_order_release);
    if (attached.load()) na::latency::pcl::Initialize();
}
uint64_t fiber_frame() {
    if (!attached.load(std::memory_order_acquire)) return 0;
    // Engine jobs are fibers: OS-thread TLS does not follow a suspended job.
    // The pinned 0x1e56314 getter reads slot 2 from the current fiber context.
    // Validate its key, never fall back to a process-global latest frame.
    __try {
        const auto fiber = __readgsqword(0x20);
        if (fiber <= 0x10000) return 0;
        const auto context = *reinterpret_cast<const uintptr_t*>(fiber);
        if (context <= 0x10000) return 0;
        uint64_t key = 0xcbf29ce484222325ull;
        for (const char c : "renderFrameParams") { if (!c) break; key = (key ^ static_cast<unsigned char>(c))*0x100000001b3ull; }
        if (*reinterpret_cast<const uint64_t*>(context+0x20) != key) return 0;
        const auto params = *reinterpret_cast<const uintptr_t*>(context+0x28);
        if (!params) return 0;
        const auto id = *reinterpret_cast<const uint64_t*>(params);
        return id != UINT64_MAX ? id+1 : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint64_t TimingRenderFrame() {
    const auto frame=present_id ? present_id : fiber_frame();
    std::lock_guard<std::mutex> lock(trace_mutex);
    ++trace.seen; if(frame) ++trace.with_frame;
    if ((!trace.count || (trace.seen % 256)==0) && attached.load()) {
        void* callers[16]{}; trace.count=RtlCaptureStackBackTrace(1,16,callers,nullptr);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for(unsigned i=0;i<trace.count;++i) {
            const uintptr_t address=reinterpret_cast<uintptr_t>(callers[i]);
            trace.callers[i]=address>=base && address-base<0x8451000 ? address-base : 0;
        }
    }
    return frame;
}
TimingTrace GetTimingTrace() { std::lock_guard<std::mutex> lock(trace_mutex); return trace; }
bool TimingAttached() { return attached.load(std::memory_order_acquire); }
}
