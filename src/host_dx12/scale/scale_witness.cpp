#include "host_dx12/scale/scale_witness.hpp"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace na::scale::witness {
namespace {
using namespace analysis;
namespace locator = na::scale::native::locator;

constexpr unsigned kFrames = 24, kGameFrames = 8;
constexpr uint64_t kIntervalMs = 2000;     // one ordinary capture per 2 s
constexpr unsigned kMaxChains = 32, kMaxCandidatesLogged = 48;

struct Capture {
    std::string api;
    float w = 0, h = 0;
    uint32_t out_w = 0, out_h = 0;
    void* frames[kFrames]{};
    unsigned count = 0;
    bool size_changed = false;
};
struct Module { uint64_t base = 0; size_t size = 0; std::wstring name; Image image; bool usable = false; };
struct Tracked { Candidate c; double first = 0, last = 0; double first_ratio = 0; bool followed = false; };
struct Chain {
    uint64_t key = 0; unsigned id = 0;
    std::string frames_text;
    std::vector<Tracked> tracked;
    float last_w = 0, last_h = 0;
};

struct State {
    std::atomic<bool> active{false};
    std::atomic<uint32_t> out_w{0}, out_h{0};
    std::atomic<uint64_t> next_ms{0};
    std::atomic<uint32_t> last_w_bits{0}, last_h_bits{0};
    std::wstring log_path;
    std::mutex m; std::condition_variable cv;
    std::deque<Capture> queue;
    bool worker_started = false;
    // Worker-owned.
    std::map<uint64_t, Module> modules;
    std::map<uint64_t, Chain> chains;
    uint64_t self_base = 0;
    FILE* log = nullptr;
};
State& g = *new State;

uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

void line(const char* fmt, ...)
{
    if (!g.log) {
        if (_wfopen_s(&g.log, g.log_path.c_str(), L"a") != 0) g.log = nullptr;
        if (!g.log) return;
    }
    SYSTEMTIME t; GetSystemTime(&t);
    std::fprintf(g.log, "%02u:%02u:%02u.%03u scale_witness ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); std::vfprintf(g.log, fmt, a); va_end(a);
    std::fputc('\n', g.log); std::fflush(g.log);
}

bool safe_read(uint64_t address, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), out, n, &got) && got == n;
}
bool page_writable(uint64_t address)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    const DWORD p = mbi.Protect & 0xff;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
const Reader& reader() { static const Reader r{&safe_read, &page_writable}; return r; }

// Game code = the game's own modules: not Windows, not graphics drivers/runtimes, not NeuralAlias.
Module* module_of(uint64_t address)
{
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(address)), &h) || !h)
        return nullptr;
    const uint64_t base = reinterpret_cast<uint64_t>(h);
    auto it = g.modules.find(base);
    if (it != g.modules.end()) return it->second.usable ? &it->second : nullptr;
    Module mod; mod.base = base;
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(h, path, MAX_PATH);
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring p = path; mod.name = p.substr(p.find_last_of(L"\\/") + 1);
    const bool system = _wcsnicmp(path, windows, wcslen(windows)) == 0;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!system && base != g.self_base && safe_read(base, &dos, sizeof dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        safe_read(base + dos.e_lfanew, &nt, sizeof nt) && nt.Signature == IMAGE_NT_SIGNATURE) {
        mod.size = nt.OptionalHeader.SizeOfImage;
        mod.image = locator::mapped_image(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base)), mod.size, base);
        mod.usable = mod.image.bytes && !mod.image.functions.empty();
    }
    auto& slot = g.modules[base] = std::move(mod);
    return slot.usable ? &slot : nullptr;
}
std::string narrow(const std::wstring& w)
{
    std::string s; for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?'); return s;
}
const char* type_name(Type t) { return t == Type::F32 ? "f32" : t == Type::F64 ? "f64" : "i32"; }
const char* where_name(Where w) { return w == Where::Rip ? "global" : w == Where::GlobalPointer ? "object(global pointer)" : "static object"; }

void analyse(const Capture& c)
{
    const double ratio = c.out_w ? double(c.w) / double(c.out_w) : 0;
    // Game frames of the chain (module + function), innermost first.
    struct Frame { Module* mod; uint32_t rva; Function fn; };
    std::vector<Frame> frames;
    uint64_t key = 1469598103934665603ull;
    for (unsigned i = 0; i < c.count && frames.size() < kGameFrames; ++i) {
        const uint64_t a = reinterpret_cast<uint64_t>(c.frames[i]);
        Module* mod = module_of(a);
        if (!mod) continue;
        const uint32_t rva = uint32_t(a - mod->base);
        const Function fn = locator::owner(mod->image, rva);
        if (!fn.size) continue;
        frames.push_back({mod, rva, fn});
        key = (key ^ (mod->base + rva)) * 1099511628211ull;
    }
    if (frames.empty()) {
        static bool said = false;
        if (!said) line("api=%s no game code in the call chain (all frames are Windows, driver or NeuralAlias)", c.api.c_str());
        said = true;
        return;
    }
    auto it = g.chains.find(key);
    if (it == g.chains.end()) {
        if (g.chains.size() >= kMaxChains) return;
        Chain ch; ch.key = key; ch.id = unsigned(g.chains.size() + 1);
        char buf[96];
        for (const auto& f : frames) {
            std::snprintf(buf, sizeof buf, "%s%s+0x%x(fn 0x%x)", ch.frames_text.empty() ? "" : " <- ", narrow(f.mod->name).c_str(), f.rva, f.fn.start);
            ch.frames_text += buf;
        }
        ScanStats st;
        std::map<uint64_t, bool> seen;
        for (const auto& f : frames) {
            std::vector<Function> parts{f.fn};
            const uint32_t primary = locator::primary(f.mod->image, f.fn.start);
            if (primary && primary != f.fn.start) parts.push_back(locator::owner(f.mod->image, primary));
            for (const auto& part : parts)
                for (const auto& cand : Scan(f.mod->image, part, ratio, reader(), &st))
                    if (!seen[cand.address * 4 + uint64_t(cand.type)]) {
                        seen[cand.address * 4 + uint64_t(cand.type)] = true;
                        ch.tracked.push_back({cand, cand.value, cand.value, ratio, false});
                    }
        }
        ch.last_w = c.w; ch.last_h = c.h;
        line("chain=%u api=%s world_viewport=%.0fx%.0f output=%ux%u ratio=%.4f code=[%s]", ch.id, c.api.c_str(), c.w, c.h, c.out_w,
             c.out_h, ratio, ch.frames_text.c_str());
        line("chain=%u numbers: loads=%u resolved=%u unresolved=%u constants_skipped=%u candidates=%zu", ch.id, st.loads, st.resolved,
             st.unresolved, st.constants, ch.tracked.size());
        unsigned n = 0;
        for (const auto& t : ch.tracked) {
            if (++n > kMaxCandidatesLogged) { line("chain=%u ... %zu more candidates not listed", ch.id, ch.tracked.size() - kMaxCandidatesLogged); break; }
            Module* mod = nullptr;
            for (const auto& f : frames) if (f.fn.start == t.c.function) { mod = f.mod; break; }
            line("chain=%u candidate %s at %s+0x%x in %s value=%g matches_size=%s", ch.id, type_name(t.c.type),
                 mod ? narrow(mod->name).c_str() : "?", t.c.at, where_name(t.c.where), t.c.value, t.c.matches ? "yes" : "no");
        }
        it = g.chains.emplace(key, std::move(ch)).first;
        return;
    }
    // A known chain: re-read every candidate. When the world size changed (slider moved),
    // the candidates that followed the new size are the render scale.
    Chain& ch = it->second;
    const bool resized = std::fabs(c.w - ch.last_w) > 0.5f || std::fabs(c.h - ch.last_h) > 0.5f;
    if (resized) line("chain=%u world size changed %.0fx%.0f -> %.0fx%.0f (output %ux%u, ratio now %.4f)", ch.id, ch.last_w, ch.last_h, c.w,
                      c.h, c.out_w, c.out_h, ratio);
    unsigned followed = 0, changed = 0;
    for (auto& t : ch.tracked) {
        double v = 0;
        if (!ReadValue(reader(), t.c.address, t.c.type, &v)) continue;
        if (v != t.last) {
            ++changed;
            const bool follows = Matches(t.c.type, v, ratio) && !Matches(t.c.type, t.first, ratio);
            if (follows && !t.followed) { t.followed = true; ++followed; }
            line("chain=%u candidate %s at +0x%x changed %g -> %g%s", ch.id, type_name(t.c.type), t.c.at, t.last, v,
                 follows ? "  FOLLOWS THE WORLD SIZE: render scale found" : "");
            t.last = v;
        }
    }
    if (resized) {
        line("chain=%u after the size change: %u candidates changed, %u follow the new size%s", ch.id, changed, followed,
             followed ? "" : " (the scale may live outside this code; see the other chains)");
        ch.last_w = c.w; ch.last_h = c.h;
    }
}

void worker()
{
    for (;;) {
        Capture c;
        {
            std::unique_lock<std::mutex> l(g.m);
            g.cv.wait(l, [] { return !g.queue.empty(); });
            c = std::move(g.queue.front()); g.queue.pop_front();
        }
        analyse(c);
    }
}
} // namespace

void Start(const wchar_t* log_path)
{
    if (g.active.load()) return;
    g.log_path = log_path ? log_path : L"";
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Start), &self);
    g.self_base = reinterpret_cast<uint64_t>(self);
    g.active.store(!g.log_path.empty());
}
bool Active() { return g.active.load(std::memory_order_relaxed); }

void NoteOutput(uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    g.out_w.store(w, std::memory_order_relaxed);
    g.out_h.store(h, std::memory_order_relaxed);
}

void OnWorldViewport(const char* api, float w, float h)
{
    if (!g.active.load(std::memory_order_relaxed) || w < 64 || h < 64) return;
    const uint32_t wb = bits(w), hb = bits(h);
    const bool size_changed = g.last_w_bits.load(std::memory_order_relaxed) != wb || g.last_h_bits.load(std::memory_order_relaxed) != hb;
    const uint64_t now = GetTickCount64();
    const uint64_t next = g.next_ms.load(std::memory_order_relaxed);
    // A size change is captured at once (the slider moved), but never more than 4 a second.
    if (size_changed ? now + kIntervalMs - 250 < next : now < next) return;
    g.next_ms.store(now + kIntervalMs, std::memory_order_relaxed);
    g.last_w_bits.store(wb, std::memory_order_relaxed);
    g.last_h_bits.store(hb, std::memory_order_relaxed);
    Capture c;
    c.api = api; c.w = w; c.h = h; c.size_changed = size_changed;
    c.out_w = g.out_w.load(std::memory_order_relaxed); c.out_h = g.out_h.load(std::memory_order_relaxed);
    c.count = RtlCaptureStackBackTrace(1, kFrames, c.frames, nullptr);
    std::lock_guard<std::mutex> l(g.m);
    if (g.queue.size() >= 16) return;
    g.queue.push_back(std::move(c));
    if (!g.worker_started) { g.worker_started = true; std::thread(worker).detach(); }
    g.cv.notify_one();
}
} // namespace na::scale::witness
