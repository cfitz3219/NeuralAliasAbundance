#include "host_dx12/scale/settings_read_hook.hpp"
#include "host_dx12/scale/render_scale_patch.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace na::scale {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

CreateFileWFn o_create_w = nullptr;
CreateFileAFn o_create_a = nullptr;
ReadFileFn o_read = nullptr;
WriteFileFn o_write = nullptr;

constexpr uint64_t kMaxFileBytes = 1u << 20;

struct Tracked {
    std::wstring path;          // lower-cased final path (GetFinalPathNameByHandleW)
    DWORD volume = 0;
    uint64_t index = 0;         // volume serial + file index: the file's identity
    bool read = false, write = false;
};
struct Patched { // per file: what we changed, to put back in the game's own writes
    std::string original, ours; // value text, same length
    std::string key;
    Unit unit = Unit::Fraction;
    double original_value = 0;  // the game's value, its unit
    // (our value is kept as text only: a double copy in our heap would be found by the scan)
    bool async_logged = false;
};

std::mutex g_m;
double g_fraction = 0;
std::wstring g_log;
std::map<HANDLE, Tracked> g_handles;
std::map<std::wstring, Patched> g_patched; // lower-cased path
ReadHookStats g_stats;
// Live switching: the marked value the game parsed, in its unit, and the candidates found.
struct Candidate {
    uintptr_t addr = 0;
    bool is_double = false;
    unsigned char last[8] = {};   // the bytes we expect there (marker, then what we wrote)
    unsigned char before[8] = {}; // what was there before we tried it (put back on failure)
    bool written = false;
};
struct Live {
    bool have_tag = false;
    Unit unit = Unit::Fraction;
    double tag = 0;
    uint32_t generation = 0;     // bumped on each marked read; a scan publishes only if current
    bool scanned = false, scanning = false;
    std::vector<Candidate> cand;
    int trying = -1, control = -1;
    std::vector<double> values_written; // every value we wrote (its unit): restored in saves
    double pending = 0;           // latest requested fraction (0 = none)
    LiveStats stats;
} g_live;
HANDLE g_scan_done = nullptr;     // manual-reset event: set while no scan runs
bool g_installed = false;
thread_local int t_inside = 0; // our own file work (the log) is not observed
// Lock-free fast path: ReadFile/WriteFile run for every file the game touches, so untracked
// handles return after scanning these few slots, without the mutex. Stale slots are harmless
// (still_same() re-checks the file) and are overwritten round-robin.
constexpr int kSlots = 16;
std::atomic<HANDLE> g_slot[kSlots];
std::atomic<uint32_t> g_slot_next{0};
bool tracked_fast(HANDLE h)
{
    for (auto& x : g_slot) if (x.load(std::memory_order_acquire) == h) return true;
    return false;
}
void track_slot(HANDLE h)
{
    if (tracked_fast(h)) return;
    g_slot[g_slot_next.fetch_add(1) % kSlots].store(h, std::memory_order_release);
}

std::wstring lower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}
bool candidate(const std::wstring& p)
{
    const size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos || p.find_last_of(L"\\/") > dot) return false;
    static const wchar_t* const exts[] = {L".ini", L".cfg", L".json", L".xml", L".txt", L".yaml", L".yml", L".conf", L".config"};
    const std::wstring e = lower(p.substr(dot));
    if (lower(p).find(L"neuralalias") != std::wstring::npos) return false; // never our own settings or logs
    for (const wchar_t* x : exts) if (e == x) return true;
    return false;
}
std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
void log_line(const char* text)
{
    if (g_log.empty() || !o_create_w || !o_write) return;
    ++t_inside;
    const HANDLE f = o_create_w(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[700];
        const int n = std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u render_scale: %s\r\n", st.wYear, st.wMonth, st.wDay,
                                    st.wHour, st.wMinute, st.wSecond, text);
        DWORD w = 0;
        if (n > 0) o_write(f, buf, static_cast<DWORD>((std::min)(n, static_cast<int>(sizeof buf) - 1)), &w, nullptr);
        CloseHandle(f);
    }
    --t_inside;
}

bool identity(HANDLE h, DWORD* vol, uint64_t* idx)
{
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) return false;
    *vol = fi.dwVolumeSerialNumber;
    *idx = (static_cast<uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow;
    return true;
}
std::wstring final_path(HANDLE h, const std::wstring& fallback)
{
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    if (!n || n >= MAX_PATH * 2) return lower(fallback);
    std::wstring fin = lower(buf);
    if (fin.rfind(L"\\\\?\\", 0) == 0) fin = fin.substr(4);
    return fin;
}
// The tracked entry still names the same file: handle values are reused after CloseHandle, and
// a same-named file in another folder is a different file (Astra 2026-11-01).
bool still_same(HANDLE h, const Tracked& t)
{
    DWORD v = 0;
    uint64_t i = 0;
    return identity(h, &v, &i) && v == t.volume && i == t.index;
}

void on_open(HANDLE h, std::wstring path, DWORD access)
{
    if (h == INVALID_HANDLE_VALUE || t_inside || !candidate(path)) return;
    const bool rd = (access & (GENERIC_READ | FILE_READ_DATA | GENERIC_ALL)) != 0;
    const bool wr = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL)) != 0;
    std::lock_guard<std::mutex> lk(g_m);
    if (!(g_fraction > 0)) return;
    if (rd && !wr) {
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || static_cast<uint64_t>(sz.QuadPart) > kMaxFileBytes) return;
        ++g_stats.files_opened;
    }
    Tracked t;
    if (!identity(h, &t.volume, &t.index)) return;
    t.path = final_path(h, path);
    t.read = rd;
    t.write = wr;
    if (g_handles.size() > 256) g_handles.clear(); // bounded: stale entries only
    g_handles[h] = std::move(t);
    track_slot(h);
}

HANDLE WINAPI create_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    const HANDLE h = o_create_w(name, access, share, sa, disp, flags, tmpl);
    if (name && !t_inside) on_open(h, name, access);
    return h;
}
HANDLE WINAPI create_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    ++t_inside; // CreateFileA may forward to CreateFileW: observe once, here
    const HANDLE h = o_create_a(name, access, share, sa, disp, flags, tmpl);
    --t_inside;
    if (name && !t_inside) {
        wchar_t w[MAX_PATH * 2] = {};
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH * 2) > 0) on_open(h, w, access);
    }
    return h;
}

BOOL WINAPI read_file(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    const BOOL ok = o_read(h, buf, n, got, ov);
    if (!ok || !buf || t_inside || ov || !got || !*got || !tracked_fast(h)) return ok;
    std::lock_guard<std::mutex> lk(g_m);
    if (!(g_fraction > 0) || g_handles.empty()) return ok;
    const auto it = g_handles.find(h);
    if (it == g_handles.end() || !it->second.read) return ok;
    if (!still_same(h, it->second)) { g_handles.erase(it); return ok; }
    char* d = static_cast<char*>(buf);
    PatchInfo info;
    const int k = PatchRenderScale(d, *got, g_fraction, &info, /*tag=*/true);
    if (k == 0 && !info.m.key[0]) return ok; // no render-scale key in these bytes
    char line[600];
    const std::string file = utf8(it->second.path);
    if (k > 0) {
        const Match& m = info.m; // the first entry actually rewritten
        ++g_stats.reads_patched;
        g_stats.original = m.current;
        std::snprintf(g_stats.key, sizeof g_stats.key, "%s", m.key);
        std::snprintf(g_stats.file, sizeof g_stats.file, "%s", file.c_str());
        Patched& p = g_patched[it->second.path];
        p.original = info.original;
        p.ours = info.written;
        p.key = m.key;
        p.unit = m.unit;
        p.original_value = m.current;
        // Live discovery only from a marker that really is in the bytes and survives float
        // conversion; startup substitution works either way.
        g_live.have_tag = info.tagged;
        g_live.unit = m.unit;
        g_live.tag = info.tagged ? std::atof(info.written) : 0; // g_live is in our module: not scanned
        ++g_live.generation;
        g_live.scanned = false;
        g_live.cand.clear();
        g_live.trying = g_live.control = -1;
        g_live.stats.tagged = info.tagged;
        g_live.stats.tag = g_live.tag;
        g_live.stats.generation = g_live.generation;
        std::snprintf(g_stats.note, sizeof g_stats.note, "%s %s -> %s (%d value%s, %s)", m.key, info.original, info.written, k,
                      k == 1 ? "" : "s", info.tagged ? "marked for live switching" : "too short to mark: no live switching");
        std::snprintf(line, sizeof line, "patched read of %s: %s", file.c_str(), g_stats.note);
    } else {
        ++g_stats.reads_unpatched;
        std::snprintf(g_stats.note, sizeof g_stats.note, "%s = %.6g found, but %.4f does not fit its width", info.m.key, info.m.current, g_fraction);
        std::snprintf(line, sizeof line, "not patched (%s): %s", file.c_str(), g_stats.note);
    }
    log_line(line);
    return ok;
}

// A value in the game's save is one we put there: the substituted text, or (any formatting)
// the substituted value or a live value we wrote.
bool ours_value(const Patched& p, const std::string& text, double v)
{
    if (text == p.ours) return true;
    const auto close_to = [&](double a) { return std::fabs(v - a) <= 1e-5 * (std::max)(1.0, std::fabs(a)); };
    if (close_to(std::atof(p.ours.c_str()))) return true;
    for (double w : g_live.values_written) if (close_to(w)) return true;
    return false;
}

BOOL WINAPI write_file(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    if (!buf || !n || t_inside || !tracked_fast(h)) return o_write(h, buf, n, done, ov);
    std::string fixed;
    {
        std::lock_guard<std::mutex> lk(g_m);
        const auto it = g_handles.find(h);
        if (it != g_handles.end() && it->second.write && still_same(h, it->second)) {
            const auto p = g_patched.find(it->second.path);
            if (p != g_patched.end() && ov) {
                // Asynchronous save: the bytes must stay valid until completion, which a temporary
                // here would not. Forwarded untouched (Astra 2026-11-01); logged once.
                if (!p->second.async_logged) {
                    p->second.async_logged = true;
                    char line[400];
                    std::snprintf(line, sizeof line, "game saved %s asynchronously: not restored (the saved value may be ours)",
                                  utf8(it->second.path).c_str());
                    log_line(line);
                }
            } else if (p != g_patched.end()) {
                fixed.assign(static_cast<const char*>(buf), n);
                const Patched& pp = p->second;
                Match m;
                int restored = 0, failed = 0;
                for (size_t from = 0; FindRenderScale(fixed.data(), fixed.size(), &m, from); from = m.value_pos + m.value_len) {
                    const std::string text = fixed.substr(m.value_pos, m.value_len);
                    if (!ours_value(pp, text, m.current)) continue; // the user's own value: saved as it is
                    if (m.value_len == pp.original.size()) {
                        fixed.replace(m.value_pos, m.value_len, pp.original);
                        ++restored;
                        continue;
                    }
                    char v[16] = {};
                    const double frac = pp.unit == Unit::Percent ? pp.original_value / 100.0 : pp.original_value;
                    Match mm = m;
                    mm.unit = pp.unit;
                    if (FormatSameWidth(frac, mm, v) && std::fabs(std::atof(v) - pp.original_value) <= 1e-6 * (std::max)(1.0, pp.original_value)) {
                        fixed.replace(m.value_pos, m.value_len, v);
                        ++restored;
                    } else {
                        ++failed;
                    }
                }
                char line[400];
                if (restored) {
                    ++g_stats.writes_restored;
                    std::snprintf(line, sizeof line, "game saved %s: kept the original %s = %s", utf8(it->second.path).c_str(), pp.key.c_str(),
                                  pp.original.c_str());
                    log_line(line);
                }
                if (failed) {
                    std::snprintf(line, sizeof line, "game saved %s: our %s value could not be put back in its width (%d)",
                                  utf8(it->second.path).c_str(), pp.key.c_str(), failed);
                    log_line(line);
                }
                if (!restored) fixed.clear();
            }
        }
    }
    if (!fixed.empty()) return o_write(h, fixed.data(), static_cast<DWORD>(fixed.size()), done, ov);
    return o_write(h, buf, n, done, ov);
}

} // namespace

void Configure(double fraction, const wchar_t* log_path)
{
    std::lock_guard<std::mutex> lk(g_m);
    g_fraction = fraction > 0 && fraction <= 2.0 ? fraction : 0;
    g_log = log_path ? log_path : L"";
}
double ConfiguredFraction()
{
    std::lock_guard<std::mutex> lk(g_m);
    return g_fraction;
}
double FractionForMode(int mode)
{
    switch (mode) {
    case 1: return 1.0 / 1.5;
    case 2: return 1.0 / 1.72;
    case 3: return 0.5;
    case 4: return 1.0 / 3.0;
    default: return 1.0;
    }
}

bool InstallSettingsReadHook()
{
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_installed) return true;
    }
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
    if (!kb) return false;
    void* cw = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW"));
    void* ca = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA"));
    void* rf = reinterpret_cast<void*>(GetProcAddress(kb, "ReadFile"));
    void* wf = reinterpret_cast<void*>(GetProcAddress(kb, "WriteFile"));
    if (!cw || !ca || !rf || !wf || !na::hooking::Initialize()) return false;
    if (!na::hooking::BeginInstallWait("render-scale read hook")) { na::hooking::Shutdown(); return false; }
    // Originals first (publish-before-enable); the detours never run before they are set.
    if (!na::hooking::PrepareHook(cw, reinterpret_cast<void*>(&create_w), reinterpret_cast<void**>(&o_create_w)) ||
        !na::hooking::PrepareHook(ca, reinterpret_cast<void*>(&create_a), reinterpret_cast<void**>(&o_create_a)) ||
        !na::hooking::PrepareHook(rf, reinterpret_cast<void*>(&read_file), reinterpret_cast<void**>(&o_read)) ||
        !na::hooking::PrepareHook(wf, reinterpret_cast<void*>(&write_file), reinterpret_cast<void**>(&o_write))) {
        na::hooking::AbandonInstall();
        na::hooking::Shutdown();
        return false;
    }
    if (!na::hooking::CommitInstall()) { na::hooking::Shutdown(); return false; }
    std::lock_guard<std::mutex> lk(g_m);
    g_installed = true; // hooks persist for the process lifetime (vtable_hook.hpp threading rules)
    return true;
}

namespace {
constexpr size_t kMaxCandidates = 8; // more than this: ambiguous, nothing is tried

struct Hit { uintptr_t addr; bool is_double; };
struct ScanOut {
    std::vector<Hit> cand;
    uint32_t floats = 0, doubles = 0, regions = 0, writable = 0, stacks = 0;
    uint64_t bytes = 0;
};

// Committed, writable regions of this process, outside our own module, our scan buffer and
// every thread's stack (an allocation holding a guard page is a stack: other threads' locals
// and spills hold the marker transiently and are never a setting's home).
void scan_memory(double v, ScanOut& o)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_live), &self);
    struct Region { uintptr_t base; size_t size; void* alloc; };
    std::vector<Region> list;
    std::vector<void*> stacks;
    MEMORY_BASIC_INFORMATION mi{};
    for (uintptr_t a = 0x10000; VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi) == sizeof mi;) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mi.BaseAddress), next = base + mi.RegionSize;
        if (next <= a) break;
        a = next;
        ++o.regions;
        if (mi.State != MEM_COMMIT) continue;
        if (mi.Protect & PAGE_GUARD) { stacks.push_back(mi.AllocationBase); continue; }
        if (mi.Protect & PAGE_NOACCESS) continue;
        if (!(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) continue;
        ++o.writable;
        if (self && mi.AllocationBase == self) continue;
        if (mi.RegionSize > (2ull << 30)) continue; // one huge region: not a settings value's home
        list.push_back({base, static_cast<size_t>(mi.RegionSize), mi.AllocationBase});
    }
    o.stacks = static_cast<uint32_t>(stacks.size());
    const float fv = static_cast<float>(v);
    constexpr size_t kChunk = 1u << 20;
    auto* buf = static_cast<unsigned char*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) return;
    bool full = false;
    for (const Region& r : list) {
        if (full) break;
        if (std::find(stacks.begin(), stacks.end(), r.alloc) != stacks.end()) continue;
        if (o.bytes + r.size > (16ull << 30)) break; // bounded in total
        o.bytes += r.size;
        for (size_t off = 0; off < r.size && !full; off += kChunk) {
            const size_t n = (std::min)(kChunk, r.size - off);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(r.base + off), buf, n, &got) || got < 4) continue;
            for (size_t i = 0; i + 4 <= got; i += 4) {
                // Only addresses are recorded here; the candidates' expected bytes are filled in
                // after the scan, so no copy of the marker sits in our own heap while it runs.
                const uintptr_t at = r.base + off + i;
                float f;
                std::memcpy(&f, buf + i, 4);
                if (f == fv) { ++o.floats; o.cand.push_back({at, false}); }
                if (i + 8 <= got) {
                    double dv;
                    std::memcpy(&dv, buf + i, 8);
                    if (dv == v) { ++o.doubles; o.cand.push_back({at, true}); }
                }
                if (o.cand.size() > kMaxCandidates) { full = true; break; }
            }
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
}

size_t width(const Candidate& c) { return c.is_double ? 8 : 4; }
bool holds(const Candidate& c, const unsigned char* expect)
{
    unsigned char cur[8] = {};
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.addr), cur, width(c), &got) && got == width(c) &&
           std::memcmp(cur, expect, width(c)) == 0;
}
bool store(Candidate& c, const unsigned char* bytes)
{
    SIZE_T put = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.addr), bytes, width(c), &put) || put != width(c)) return false;
    std::memcpy(c.last, bytes, width(c));
    return true;
}
void encode(const Candidate& c, double v, unsigned char* out)
{
    if (c.is_double) std::memcpy(out, &v, 8);
    else { const float f = static_cast<float>(v); std::memcpy(out, &f, 4); }
}

LiveResult set_result(LiveResult r, const char* fmt, ...)
{
    g_live.stats.result = r;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_live.stats.note, sizeof g_live.stats.note, fmt, ap);
    va_end(ap);
    char line[300];
    std::snprintf(line, sizeof line, "live switch %s: %s", LiveResultName(r), g_live.stats.note);
    log_line(line);
    return r;
}

void remember(double v) // values we wrote, restored in the game's saves (bounded)
{
    auto& w = g_live.values_written;
    if (std::find(w.begin(), w.end(), v) != w.end()) return;
    if (w.size() >= 32) w.erase(w.begin());
    w.push_back(v);
}

// g_m held. Writes the pending request to the confirmed control, or to the next untried candidate.
LiveResult apply_locked()
{
    auto& L = g_live;
    const double fraction = L.pending;
    if (!(fraction > 0)) return L.stats.result;
    const double nv = L.unit == Unit::Percent ? fraction * 100.0 : fraction;
    L.stats.requested = nv;
    if (L.cand.empty()) return set_result(LiveResult::NotFound, "the marked value %.6f was not found in memory", L.tag);
    if (L.cand.size() > kMaxCandidates)
        return set_result(LiveResult::Ambiguous, "more than %zu copies of the marked value: nothing written", kMaxCandidates);
    unsigned char bytes[8] = {};
    if (L.control >= 0) {
        Candidate& c = L.cand[static_cast<size_t>(L.control)];
        if (!holds(c, c.last)) return set_result(LiveResult::NoLongerOurs, "the control no longer holds our last value: left alone");
        encode(c, nv, bytes);
        if (!store(c, bytes)) return set_result(LiveResult::NoLongerOurs, "the control could not be written");
        ++L.stats.writes;
        remember(nv);
        L.pending = 0;
        return set_result(LiveResult::Applied, "wrote %.6f to the control (%s at %p); J2 confirms the extent", nv,
                          c.is_double ? "double" : "float", reinterpret_cast<void*>(c.addr));
    }
    // Untried candidate, one at a time. A candidate already written and still being tried is rewritten.
    for (int i = L.trying < 0 ? 0 : L.trying; i < static_cast<int>(L.cand.size()); ++i) {
        Candidate& c = L.cand[static_cast<size_t>(i)];
        if (!c.written && !holds(c, c.last)) continue; // reused for something else since the scan: skip
        if (c.written && !holds(c, c.last)) continue;
        if (!c.written) std::memcpy(c.before, c.last, sizeof c.before);
        encode(c, nv, bytes);
        if (!store(c, bytes)) continue;
        c.written = true;
        L.trying = i;
        ++L.stats.writes;
        remember(nv);
        L.stats.trying = i;
        L.pending = 0;
        return set_result(LiveResult::Trying, "trying candidate %d of %zu (%s at %p) = %.6f; waiting for J2's extent", i + 1, L.cand.size(),
                          c.is_double ? "double" : "float", reinterpret_cast<void*>(c.addr), nv);
    }
    L.trying = static_cast<int>(L.cand.size()); // stays exhausted until the next marked read
    L.stats.trying = -1;
    return set_result(LiveResult::Exhausted, "none of the %zu candidates changed the picture: applies on next launch", L.cand.size());
}

DWORD WINAPI scan_thread(void*)
{
    for (;;) {
        double tag = 0;
        uint32_t gen = 0;
        {
            std::lock_guard<std::mutex> lk(g_m);
            tag = g_live.tag;
            gen = g_live.generation;
        }
        const ULONGLONG t0 = GetTickCount64();
        ScanOut o;
        scan_memory(tag, o); // outside g_m: the file hooks keep running meanwhile
        const ULONGLONG ms = GetTickCount64() - t0;
        std::lock_guard<std::mutex> lk(g_m);
        auto& L = g_live;
        if (gen != L.generation && L.have_tag) continue; // a newer marked read: scan again for it
        if (gen != L.generation) { // a newer read carried no marker: nothing to publish
            L.scanning = false;
            L.cand.clear();
            SetEvent(g_scan_done);
            return 0;
        }
        L.cand.clear();
        for (const Hit& h : o.cand) {
            Candidate c;
            c.addr = h.addr;
            c.is_double = h.is_double;
            encode(c, tag, c.last);
            L.cand.push_back(c);
        }
        L.scanned = true;
        L.scanning = false;
        L.trying = L.control = -1;
        L.stats.float_hits = o.floats;
        L.stats.double_hits = o.doubles;
        L.stats.candidates = static_cast<uint32_t>(L.cand.size());
        L.stats.scanned_kb = o.bytes >> 10;
        L.stats.scan_ms = ms;
        L.stats.regions = o.regions;
        L.stats.writable_regions = o.writable;
        L.stats.stack_allocations_skipped = o.stacks;
        char line[300];
        std::snprintf(line, sizeof line, "scan for %.6f: %llu KB in %llu ms, %u float + %u double candidate%s, %u stacks skipped", tag,
                      (unsigned long long)L.stats.scanned_kb, (unsigned long long)ms, o.floats, o.doubles,
                      L.cand.size() == 1 ? "" : "s", o.stacks);
        log_line(line);
        apply_locked();
        SetEvent(g_scan_done);
        return 0;
    }
}
} // namespace

const char* LiveResultName(LiveResult r)
{
    switch (r) {
    case LiveResult::Off: return "off";
    case LiveResult::NoTag: return "no-marker";
    case LiveResult::Scanning: return "scanning";
    case LiveResult::NotFound: return "not-found";
    case LiveResult::Ambiguous: return "ambiguous";
    case LiveResult::Trying: return "trying";
    case LiveResult::Applied: return "applied";
    case LiveResult::NoLongerOurs: return "no-longer-ours";
    case LiveResult::Exhausted: return "exhausted";
    }
    return "?";
}

LiveResult SetLiveFraction(double fraction)
{
    std::lock_guard<std::mutex> lk(g_m);
    auto& L = g_live;
    if (!(fraction > 0) || fraction > 2.0) return set_result(LiveResult::Off, "requested %.4f is out of range", fraction);
    if (!L.have_tag) return set_result(LiveResult::NoTag, "no marked render-scale value was read at start-up");
    L.pending = fraction;
    if (L.scanned) return apply_locked();
    if (!L.scanning) {
        if (!g_scan_done) g_scan_done = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!g_scan_done) return set_result(LiveResult::NotFound, "could not start the scan");
        ResetEvent(g_scan_done);
        const HANDLE t = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr);
        if (!t) { SetEvent(g_scan_done); return set_result(LiveResult::NotFound, "could not start the scan thread"); }
        CloseHandle(t);
        L.scanning = true;
    }
    L.stats.result = LiveResult::Scanning;
    std::snprintf(L.stats.note, sizeof L.stats.note, "scanning for the marked value; %.4f is applied when it ends", fraction);
    return LiveResult::Scanning;
}

void ReportLiveOutcome(bool extent_changed)
{
    std::lock_guard<std::mutex> lk(g_m);
    auto& L = g_live;
    if (L.control >= 0 || L.trying < 0 || L.trying >= static_cast<int>(L.cand.size())) return;
    Candidate& c = L.cand[static_cast<size_t>(L.trying)];
    if (extent_changed) {
        L.control = L.trying;
        L.stats.control = L.control;
        L.stats.trying = -1;
        L.trying = -1;
        set_result(LiveResult::Applied, "candidate %d is the control (%s at %p)", L.control + 1, c.is_double ? "double" : "float",
                   reinterpret_cast<void*>(c.addr));
        return;
    }
    // Did not move the picture: put it back exactly as it was, if it still holds what we wrote.
    if (holds(c, c.last) && store(c, c.before)) ++L.stats.restores;
    c.written = false;
    const int next = L.trying + 1;
    L.trying = next < static_cast<int>(L.cand.size()) ? next : static_cast<int>(L.cand.size());
    L.stats.trying = -1;
    char line[200];
    std::snprintf(line, sizeof line, "live switch: candidate %d did not change the picture; put back", next);
    log_line(line);
}

bool WaitLiveScan(uint32_t timeout_ms)
{
    HANDLE e = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!g_live.scanning) return true;
        e = g_scan_done;
    }
    return e && WaitForSingleObject(e, timeout_ms) == WAIT_OBJECT_0;
}

LiveStats GetLiveStats()
{
    std::lock_guard<std::mutex> lk(g_m);
    return g_live.stats;
}

ReadHookStats GetReadHookStats()
{
    std::lock_guard<std::mutex> lk(g_m);
    return g_stats;
}

} // namespace na::scale
