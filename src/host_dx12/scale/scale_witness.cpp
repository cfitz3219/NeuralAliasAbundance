#include "host_dx12/scale/scale_witness.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

namespace na::scale::witness {
namespace {
using namespace analysis;

constexpr unsigned kFrames = 24, kGameFrames = 8;
constexpr uint64_t kChainIntervalMs = 2000; // one ordinary call-chain capture per 2 s
constexpr uint64_t kWindowMs = 500;         // the world's size is the largest viewport in a window
constexpr unsigned kMaxChains = 48, kMaxSizesSeen = 16, kMaxLogLines = 6000, kTopLogged = 12;
constexpr size_t kChunk = 1u << 20;         // memory sweep: 1 MB at a time
constexpr uint64_t kSweepBudget = 24ull << 30;
constexpr size_t kMaxImage = 1u << 30;
constexpr size_t kPercentRoom = 50000;      // of the board's 200,000 places

struct Capture {
    const char* api = "";
    uint32_t method = hunt::kCallChain;
    uint32_t w = 0, h = 0;
    void* frames[kFrames]{};
    unsigned count = 0;
};
struct TrialRow {
    uint64_t address = 0; hunt::Kind kind = hunt::Kind::ScaleF32;
    double before = 0, asked = 0, achieved = 0;
    hunt::Verdict verdict = hunt::Verdict::Idle; hunt::Hold hold = hunt::Hold::Once;
    uint32_t reverts = 0, writes = 0, respond_ms = 0;
    bool kept_value = false, restored = false, skipped = false;
    const char* note = "";
};
struct Module {
    uint64_t base = 0; size_t size = 0; std::string name;
    uint8_t* copy = nullptr; // our own copy of the read-only parts: the game cannot pull it away
    Image image; bool usable = false; const char* why = "";
};

struct State {
    std::atomic<bool> active{false};
    std::atomic<uint32_t> out_w{0}, out_h{0};
    // Hook side: call-chain rate limit and the size window.
    std::atomic<uint64_t> next_chain_ms{0}, window_start{0};
    std::atomic<uint64_t> window_best{0}, window_last{0}, published{0}; // packed w<<32|h
    std::mutex seen_m; std::set<uint64_t> viewport_sizes, created_sizes; std::map<uint64_t, unsigned> created_count;
    std::wstring log_path, verdict_path;
    // Test verdict counters.
    std::atomic<uint64_t> viewports{0}, created{0}, chains_read{0}, names_found{0}, bytes_searched{0}, candidates_from_code{0};
    std::atomic<uint32_t> scale_changes{0};
    // Code worker: call chains (and the named search at start).
    std::mutex code_m; std::condition_variable code_cv; std::deque<Capture> captures;
    // Memory worker: size events.
    std::mutex mem_m; std::condition_variable mem_cv; std::deque<hunt::Target> sizes;
    // Shared.
    std::mutex board_m; hunt::Board board; hunt::Target target; bool have_target = false;
    std::mutex log_m; FILE* log = nullptr; unsigned lines = 0;
    // Code worker only.
    std::map<uint64_t, Module> modules; std::set<uint64_t> chains; uint64_t self_base = 0;
    // The world's size right now (largest viewport of the last 100 ms), for the writing trial.
    std::atomic<uint64_t> fast_start{0}, fast_best{0}, live_size{0}, live_ms{0};
    // The writing trial ([scale] trial=1): NeuralAlias sets each confirmed number itself and the judge
    // watches whether the game's world follows.
    std::atomic<bool> trial_wanted{false}, trial_active{false}, sweeping{false};
    std::atomic<uint64_t> last_size_event_ms{0}, trial_ended_ms{0};
    std::mutex trial_m; std::vector<TrialRow> trial_rows; std::vector<std::string> trial_paths;
    int trial_state = 0; // 0 not run, 1 running, 2 finished, 3 stopped (the world did not come back)
    std::string trial_note;
};
State& g = *new State;

uint64_t pack(uint32_t w, uint32_t h) { return (uint64_t(w) << 32) | h; }

void line(const char* fmt, ...)
{
    std::lock_guard<std::mutex> l(g.log_m);
    if (g.lines > kMaxLogLines) return;
    if (!g.log) {
        if (_wfopen_s(&g.log, g.log_path.c_str(), L"a") != 0) g.log = nullptr;
        if (!g.log) return;
    }
    SYSTEMTIME t; GetSystemTime(&t);
    std::fprintf(g.log, "%02u:%02u:%02u.%03u scale_witness ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (g.lines++ == kMaxLogLines) std::fputs("log limit reached; the witness keeps working but writes no more lines", g.log);
    else { va_list a; va_start(a, fmt); std::vfprintf(g.log, fmt, a); va_end(a); }
    std::fputc('\n', g.log); std::fflush(g.log);
}

// ---- safe memory access: never touch a guard page, never fault ----
bool readable_region(const MEMORY_BASIC_INFORMATION& m)
{
    return m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (m.Protect & 0xff) != 0;
}
bool writable_protect(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD p = protect & 0xff;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
const void* ptr(uint64_t address) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(address)); }
bool safe_read(uint64_t address, void* out, size_t n)
{
    MEMORY_BASIC_INFORMATION m{};
    if (address < 0x10000 || !VirtualQuery(ptr(address), &m, sizeof m) || !readable_region(m)) return false;
    const uint64_t end = reinterpret_cast<uint64_t>(m.BaseAddress) + m.RegionSize;
    if (address + n > end) return false; // small reads: stay inside the checked region
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), ptr(address), out, n, &got) && got == n;
}
bool page_writable(uint64_t address)
{
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(ptr(address), &m, sizeof m) && m.State == MEM_COMMIT && writable_protect(m.Protect);
}
const Reader& reader() { static const Reader r{&safe_read, &page_writable}; return r; }
bool holds(uint64_t address, hunt::Kind kind, const hunt::Target& t)
{
    uint8_t b[8]{};
    const size_t n = hunt::IsSizePair(kind) || kind == hunt::Kind::ScaleF64 ? 8 : 4;
    return safe_read(address, b, n) && hunt::Holds(b, n, kind, t);
}

// ---- modules ----
std::string leaf_name(const wchar_t* path)
{
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') leaf = p + 1;
    std::string s;
    for (; *leaf; ++leaf) s.push_back(*leaf < 128 ? char(*leaf) : '?');
    return s;
}
HMODULE module_handle(uint64_t address, bool pin)
{
    HMODULE h = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | (pin ? 0u : GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
    return GetModuleHandleExW(flags, static_cast<LPCWSTR>(ptr(address)), &h) ? h : nullptr;
}
// Copies [rva, rva+n) of a loaded module into our copy; unreadable pages stay zero.
void copy_range(uint64_t base, uint8_t* copy, uint32_t rva, uint32_t n)
{
    for (uint32_t done = 0; done < n;) {
        MEMORY_BASIC_INFORMATION m{};
        const uint64_t at = base + rva + done;
        if (!VirtualQuery(ptr(at), &m, sizeof m)) return;
        const uint64_t region_end = reinterpret_cast<uint64_t>(m.BaseAddress) + m.RegionSize;
        const uint32_t take = uint32_t((std::min<uint64_t>)(n - done, region_end - at));
        if (!take) return;
        if (readable_region(m)) {
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), ptr(at), copy + rva + done, take, &got);
        }
        done += take;
    }
}
// Game code = the game's own modules: not Windows, not NeuralAlias. The module is pinned (it can
// no longer unload) and its read-only parts are copied, so reading the code can never fault.
Module* module_at(uint64_t address)
{
    const HMODULE h = module_handle(address, false);
    if (!h) return nullptr;
    const uint64_t base = reinterpret_cast<uint64_t>(h);
    auto it = g.modules.find(base);
    if (it != g.modules.end()) return it->second.usable ? &it->second : nullptr;
    Module mod; mod.base = base;
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(h, path, MAX_PATH);
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    mod.name = leaf_name(path);
    const bool system = windows[0] && _wcsnicmp(path, windows, wcslen(windows)) == 0;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (system || base == g.self_base) mod.why = "not game code";
    else if (!safe_read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
             !safe_read(base + uint32_t(dos.e_lfanew), &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
             nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        mod.why = "not a 64-bit program image";
    else if (nt.OptionalHeader.SizeOfImage > kMaxImage) mod.why = "image too large";
    else {
        module_handle(base, true); // one reference we never give back: the module stays loaded
        mod.size = nt.OptionalHeader.SizeOfImage;
        mod.copy = static_cast<uint8_t*>(VirtualAlloc(nullptr, mod.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!mod.copy) mod.why = "no memory for a copy";
        else {
            copy_range(base, mod.copy, 0, (std::min<uint32_t>)(nt.OptionalHeader.SizeOfHeaders, uint32_t(mod.size)));
            const size_t sections = size_t(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
            for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER s{};
                const size_t at = sections + i * sizeof s;
                if (at + sizeof s > mod.size) break;
                std::memcpy(&s, mod.copy + at, sizeof s);
                if (s.VirtualAddress >= mod.size || s.Misc.VirtualSize > mod.size - s.VirtualAddress) continue;
                // Writable sections are read live (they are the settings); the rest is copied.
                if (!(s.Characteristics & IMAGE_SCN_MEM_WRITE)) copy_range(base, mod.copy, s.VirtualAddress, s.Misc.VirtualSize);
            }
            // Read-only from here: the memory search skips it, and nothing can scribble on it.
            DWORD was = 0;
            VirtualProtect(mod.copy, mod.size, PAGE_READONLY, &was);
            mod.image = locator::mapped_image(mod.copy, mod.size, base);
            mod.usable = mod.image.bytes && !mod.image.functions.empty();
            if (!mod.usable) mod.why = "its code table cannot be read (protected or packed)";
        }
    }
    auto& slot = g.modules[base] = std::move(mod);
    line("module %s: %s", slot.name.c_str(), slot.usable ? "code readable" : slot.why);
    if (!slot.usable && slot.copy) { VirtualFree(slot.copy, 0, MEM_RELEASE); slot.copy = nullptr; slot.image = Image{}; }
    return slot.usable ? &slot : nullptr;
}
std::string where_text(uint64_t address)
{
    char buf[160];
    if (const HMODULE h = module_handle(address, false)) {
        wchar_t path[MAX_PATH]{}; GetModuleFileNameW(h, path, MAX_PATH);
        std::snprintf(buf, sizeof buf, "%s+0x%llx", leaf_name(path).c_str(), static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(h)));
    } else std::snprintf(buf, sizeof buf, "game memory 0x%llx", static_cast<unsigned long long>(address));
    return buf;
}

// ---- the board: report and rank ----
void report(const std::vector<Candidate>& found, uint32_t method)
{
    std::lock_guard<std::mutex> l(g.board_m);
    for (const Candidate& c : found)
        g.board.Report(c.address, c.kind, method | (c.via_pointer ? uint32_t(hunt::kPointerChain) : 0u),
                       g.have_target && holds(c.address, c.kind, g.target));
}
std::string methods_text(uint32_t m)
{
    std::string s;
    const auto add = [&](uint32_t bit, const char* name) { if (m & bit) { if (!s.empty()) s += "+"; s += name; } };
    add(hunt::kCallChain, "call chain"); add(hunt::kCreation, "creation"); add(hunt::kMemory, "memory");
    add(hunt::kNamed, "named"); add(hunt::kPointerChain, "pointers");
    return s;
}
void log_ranking(const char* when)
{
    std::vector<hunt::Entry> top; size_t size, confirmed, dropped; uint32_t changes;
    {
        std::lock_guard<std::mutex> l(g.board_m);
        top = g.board.Ranked(kTopLogged); size = g.board.Size(); confirmed = g.board.ConfirmedCount();
        dropped = g.board.Dropped(); changes = g.board.Changes();
    }
    line("board %s: candidates=%zu confirmed=%zu size_changes_seen=%u dropped_full=%zu", when, size, confirmed, changes, dropped);
    unsigned rank = 0;
    for (const hunt::Entry& e : top) {
        double v = 0;
        char value[48] = "size pair";
        if (!hunt::IsSizePair(e.kind)) {
            if (ReadKind(reader(), e.address, e.kind, &v)) std::snprintf(value, sizeof value, "value=%g", v);
            else std::snprintf(value, sizeof value, "unreadable");
        }
        line("board #%u score=%d %s%s at %s found_by=[%s] followed=%u missed=%u self_changes=%u %s", ++rank, e.Score(),
             e.Confirmed() ? "CONFIRMED " : "", hunt::KindName(e.kind), where_text(e.address).c_str(), methods_text(e.methods).c_str(),
             e.followed, e.missed, e.noisy, value);
    }
}

// ---- method: call chain / creation (read the game code on the stack) ----
void analyse(const Capture& c)
{
    struct Frame { Module* mod; uint32_t rva; Function fn; };
    std::vector<Frame> frames;
    uint64_t key = 1469598103934665603ull ^ c.method;
    for (unsigned i = 0; i < c.count && frames.size() < kGameFrames; ++i) {
        // A return address is the instruction AFTER the call: one byte back is inside the caller.
        const uint64_t a = reinterpret_cast<uint64_t>(c.frames[i]) - 1;
        Module* mod = module_at(a);
        if (!mod) continue;
        const uint32_t rva = uint32_t(a - mod->base);
        const Function fn = locator::owner(mod->image, rva);
        if (!fn.size) continue;
        frames.push_back({mod, rva, fn});
        key = (key ^ (mod->base + fn.start)) * 1099511628211ull;
    }
    const char* method = c.method == hunt::kCreation ? "creation" : "call chain";
    if (frames.empty()) {
        static std::atomic<unsigned> said{0};
        if (said.fetch_add(1) < 2)
            line("%s api=%s %ux%u: no readable game code on the stack (the memory search does not need it)", method, c.api, c.w, c.h);
        return;
    }
    if (g.chains.count(key) || g.chains.size() >= kMaxChains) return;
    g.chains.insert(key);
    std::string text;
    ScanStats st;
    std::vector<Candidate> found;
    for (const auto& f : frames) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s%s+0x%x", text.empty() ? "" : " <- ", f.mod->name.c_str(), f.fn.start);
        text += buf;
        std::vector<Function> parts{f.fn};
        const uint32_t primary = locator::primary(f.mod->image, f.fn.start);
        if (primary && primary != f.fn.start) parts.push_back(locator::owner(f.mod->image, primary));
        for (const auto& part : parts) {
            auto got = Scan(f.mod->image, part, reader(), &st);
            found.insert(found.end(), got.begin(), got.end());
        }
    }
    report(found, c.method);
    g.chains_read.fetch_add(1); g.candidates_from_code.fetch_add(found.size());
    line("%s #%zu api=%s size=%ux%u code=[%s] number_loads=%u followed_to_an_address=%u not_followed=%u constants_skipped=%u "
         "functions_partly_unreadable=%u candidates=%zu",
         method, g.chains.size(), c.api, c.w, c.h, text.c_str(), st.loads, st.resolved, st.unresolved, st.constants, st.undecodable, found.size());
}

// ---- method: named (setting names in the game's own program file) ----
// Generic words for this setting in any engine; never a game's name.
const char* const kWords[] = {"renderscale", "render_scale", "render scale", "resolutionscale", "resolution_scale", "resolution scale",
                              "screenpercentage", "screen_percentage", "screen percentage", "renderresolution", "render_resolution",
                              "resscale", "res_scale", "internalres", "internal_res", "internal res", "renderpercent", "render_percent",
                              "resolutionpercent", "resolution_percent", "scalefactor", "scale_factor", "upscal", "supersampl",
                              "dynamicres", "dynamic_res", "dynres", "viewportscale", "viewport_scale", "3dscale", "3d_scale", "3d scale",
                              "3dresolution", "3d_resolution", "3d resolution"};
char lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); }
bool name_char(uint8_t p)
{
    return (p >= '0' && p <= '9') || (p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z') || p == '.' || p == '_' || p == ' ';
}
// Finds each word in the read-only data as ASCII (step 1) or UTF-16 (step 2), case-insensitive.
void find_words(const Image& im, std::vector<std::pair<uint32_t, std::string>>& out, size_t cap)
{
    for (const auto& range : im.ranges) {
        if (range.executable) continue;
        const uint8_t* b = im.bytes + range.start;
        for (uint32_t step = 1; step <= 2; ++step)
            for (uint32_t i = 0; i + 8 < range.size && out.size() < cap; ++i) {
                const char c = lower(b[i]);
                if (c != 'r' && c != 's' && c != 'i' && c != 'u' && c != 'd' && c != 'v' && c != '3') continue;
                for (const char* word : kWords) {
                    if (word[0] != c) continue;
                    const uint32_t n = uint32_t(std::strlen(word));
                    if (uint64_t(i) + uint64_t(n) * step > range.size) continue;
                    bool same = true;
                    for (uint32_t k = 0; k < n && same; ++k) same = lower(b[i + k * step]) == word[k] && (step == 1 || b[i + k * step + 1] == 0);
                    if (!same) continue;
                    // Back up to the start of the whole name.
                    uint32_t start = i;
                    while (start >= step && i - start < 96 && name_char(b[start - step]) && (step == 1 || b[start - step + 1] == 0)) start -= step;
                    std::string whole;
                    for (uint32_t k = start; k < range.size && whole.size() < 120; k += step) {
                        if (b[k] < 32 || b[k] > 126 || (step == 2 && k + 1 < range.size && b[k + 1] != 0)) break;
                        whole.push_back(char(b[k]));
                    }
                    out.emplace_back(range.start + start, whole);
                    i += n * step;
                    break;
                }
            }
    }
}
void named_search(Module& mod)
{
    std::vector<std::pair<uint32_t, std::string>> names;
    find_words(mod.image, names, 256);
    if (names.empty()) { line("named: no setting names found in %s", mod.name.c_str()); return; }
    std::map<uint32_t, std::string> by_rva;
    for (auto& n : names) by_rva.emplace(n.first, n.second);
    const uint32_t lo = by_rva.begin()->first, hi = by_rva.rbegin()->first;
    // One pass over the code: every RIP-relative reference that lands on one of the names.
    std::vector<Candidate> found; ScanStats st; unsigned refs = 0;
    std::map<uint32_t, std::vector<Ins>> decoded;
    std::set<uint32_t> used;
    for (const auto& range : mod.image.ranges) {
        if (!range.executable || range.size < 4) continue;
        for (uint32_t p = range.start; p <= range.start + range.size - 4; ++p) {
            int32_t d; std::memcpy(&d, mod.image.bytes + p, 4);
            const int64_t target = int64_t(p) + 4 + d;
            if (target < int64_t(lo) || target > int64_t(hi)) continue;
            const auto name = by_rva.find(uint32_t(target));
            if (name == by_rva.end()) continue;
            const Function fn = locator::owner(mod.image, p);
            if (!fn.size) continue;
            auto it = decoded.find(fn.start);
            if (it == decoded.end()) {
                if (decoded.size() >= 512) continue;
                it = decoded.emplace(fn.start, DecodeFunction(mod.image, fn)).first;
                auto loads = ScanCode(mod.image, fn.start, it->second, reader(), &st);
                found.insert(found.end(), loads.begin(), loads.end());
            }
            auto near_data = DataNear(mod.image, fn.start, it->second, p, 12, reader(), &st);
            found.insert(found.end(), near_data.begin(), near_data.end());
            ++refs; used.insert(name->first);
        }
    }
    report(found, hunt::kNamed);
    g.names_found.store(by_rva.size()); g.candidates_from_code.fetch_add(found.size());
    line("named: %s has %zu setting names, %u code references to %zu of them, candidates=%zu", mod.name.c_str(), by_rva.size(), refs,
         used.size(), found.size());
    unsigned shown = 0;
    for (const auto& n : by_rva) {
        if (++shown > 40) { line("named: ... %zu more names not listed", by_rva.size() - 40); break; }
        line("named: \"%s\"%s", n.second.c_str(), used.count(n.first) ? " (referenced by code)" : "");
    }
}

void code_worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // The named search needs nothing from the game but its program file: it runs at once.
    if (Module* exe = module_at(reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)) + 0x1000)) named_search(*exe);
    for (;;) {
        Capture c;
        {
            std::unique_lock<std::mutex> l(g.code_m);
            g.code_cv.wait(l, [] { return !g.captures.empty(); });
            c = g.captures.front(); g.captures.pop_front();
        }
        analyse(c);
    }
}

// ---- method: memory (find the world's size and scale by value) ----
void sweep(const hunt::Target& t)
{
    const uint64_t started = GetTickCount64();
    std::vector<uint8_t> buffer(kChunk + 8);
    std::vector<hunt::Hit> hits;
    hunt::SweepOptions options;
    uint64_t scanned = 0, regions = 0, total_hits = 0, percent_left_out = 0;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uint64_t at = reinterpret_cast<uint64_t>(si.lpMinimumApplicationAddress);
    const uint64_t top = reinterpret_cast<uint64_t>(si.lpMaximumApplicationAddress);
    const uint64_t own = reinterpret_cast<uint64_t>(buffer.data());
    while (at < top && scanned < kSweepBudget) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(ptr(at), &m, sizeof m)) break;
        const uint64_t base = reinterpret_cast<uint64_t>(m.BaseAddress), size = m.RegionSize;
        at = base + size;
        // Settings live in the program's own writable data and on its heaps. Not in files mapped
        // from disk, and not in this search's own buffer.
        if (m.State != MEM_COMMIT || !writable_protect(m.Protect) || m.Type == MEM_MAPPED) continue;
        if (base <= own && own < base + size) continue;
        ++regions;
        for (uint64_t off = 0; off < size && scanned < kSweepBudget; off += kChunk) {
            const size_t want = size_t((std::min<uint64_t>)(kChunk + 8, size - off));
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), ptr(base + off), buffer.data(), want, &got) || got < 8)
                break; // the region changed under us: skip the rest of it
            scanned += (std::min<uint64_t>)(got, kChunk);
            hits.clear();
            hunt::SweepBlock(buffer.data(), got, base + off, t, options, hits);
            if (!hits.empty()) {
                std::lock_guard<std::mutex> l(g.board_m);
                for (const hunt::Hit& h : hits) {
                    // A whole number such as 70 or 50 is everywhere in a program's memory. Such finds may
                    // fill a quarter of the board, never the room the scale numbers and sizes need
                    // (Crusader Kings III 2026-11-28: over a million dropped at the board's limit).
                    if (h.kind == hunt::Kind::PercentI32 && g.board.Size() >= kPercentRoom) { ++percent_left_out; continue; }
                    g.board.Report(h.address, h.kind, hunt::kMemory, true);
                }
                total_hits += hits.size();
            }
            if (((off / kChunk) & 15) == 15) Sleep(1); // every 16 MB: let the game have the core
        }
    }
    g.bytes_searched.fetch_add(scanned);
    line("memory: searched %.1f GB in %llu regions in %.1f s for world %ux%u (scale %.4f%s): %llu matches", double(scanned) / double(1ull << 30),
         static_cast<unsigned long long>(regions), double(GetTickCount64() - started) / 1000.0, t.world_w, t.world_h, t.Ratio(),
         t.Distinct() ? "" : ", full size: size pairs only", static_cast<unsigned long long>(total_hits));
    if (percent_left_out) line("memory: %llu whole-number percent matches left out (that kind is limited to %zu places on the board)",
                               static_cast<unsigned long long>(percent_left_out), kPercentRoom);
}
// ---- the test verdict: PASS or FAIL with the reason, rewritten after every event ----
void write_verdict()
{
    if (g.verdict_path.empty()) return;
    static std::mutex one_writer; // the memory worker and the trial both rewrite the file
    std::lock_guard<std::mutex> writer(one_writer);
    std::vector<hunt::Entry> top; hunt::Target t; bool have; size_t size;
    {
        std::lock_guard<std::mutex> l(g.board_m);
        top = g.board.Ranked(64); t = g.target; have = g.have_target; size = g.board.Size();
    }
    const uint32_t changes = g.scale_changes.load();
    std::vector<hunt::Entry> scales, pairs;
    for (const auto& e : top)
        if (e.Confirmed()) (hunt::IsSizePair(e.kind) ? pairs : scales).push_back(e);
    bool pass = false;
    char reason[512];
    if (!have) {
        if (!g.viewports.load())
            std::snprintf(reason, sizeof reason, "NeuralAlias never saw the game draw its 3D world (no world picture was chosen, or the game was "
                          "only in menus). Load into the game map/level and wait 10 seconds.");
        else std::snprintf(reason, sizeof reason, "The 3D world was seen but its size never held steady for one second.");
    } else if (!changes) {
        std::snprintf(reason, sizeof reason, "The world stayed at %ux%u the whole run: the game's Render Scale was not changed, or the game "
                      "only applies it after a restart. Change Render Scale in the game's menu, apply, wait 5 seconds, then change it again.",
                      t.world_w, t.world_h);
    } else if (!scales.empty()) {
        pass = true;
        std::snprintf(reason, sizeof reason, "The game's render-scale number was found: %s at %s, found by [%s]. It followed %u of %u size "
                      "changes and never changed on its own.", hunt::KindName(scales[0].kind), where_text(scales[0].address).c_str(),
                      methods_text(scales[0].methods).c_str(), scales[0].followed, scales[0].followed + scales[0].missed);
    } else if (!pairs.empty()) {
        std::snprintf(reason, sizeof reason, "Only the RESULT was found (where the game keeps the world's size: %zu places), not the scale "
                      "setting itself. %s", pairs.size(), changes < 2 ? "The memory search needs a second change to confirm the setting: "
                      "change Render Scale once more." : "After two or more changes no scale number followed: this game may compute the "
                      "size without storing a scale.");
    } else {
        std::snprintf(reason, sizeof reason, "The world changed size %u time(s) but no number in the game followed it (%zu candidates "
                      "checked).", changes, size);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g.verdict_path.c_str(), L"w") != 0 || !f) return;
    SYSTEMTIME now; GetSystemTime(&now);
    std::fprintf(f, "NeuralAlias render-scale finder test (log only: nothing in the game was changed)\n");
    std::fprintf(f, "Written %04u-%02u-%02u %02u:%02u:%02u UTC, game process %lu\n\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond, GetCurrentProcessId());
    std::fprintf(f, "RESULT: %s\nREASON: %s\n\n", pass ? "PASS" : "FAIL", reason);
    std::fprintf(f, "What each step saw:\n");
    if (have) std::fprintf(f, "  3D world seen:            yes, now %ux%u on a %ux%u screen (%.1f%%)\n", t.world_w, t.world_h, t.out_w, t.out_h, t.Ratio() * 100.0);
    else std::fprintf(f, "  3D world seen:            no (%llu world viewports)\n", static_cast<unsigned long long>(g.viewports.load()));
    std::fprintf(f, "  Render scale changes:     %u\n", changes);
    std::fprintf(f, "  Game code paths read:     %llu (world-sized pictures created: %llu)\n", static_cast<unsigned long long>(g.chains_read.load()),
                 static_cast<unsigned long long>(g.created.load()));
    std::fprintf(f, "  Setting names in program: %llu\n", static_cast<unsigned long long>(g.names_found.load()));
    std::fprintf(f, "  Numbers taken from code:  %llu\n", static_cast<unsigned long long>(g.candidates_from_code.load()));
    std::fprintf(f, "  Memory searched:          %.1f GB\n", double(g.bytes_searched.load()) / double(1ull << 30));
    std::fprintf(f, "  Candidates on the board:  %zu (confirmed scale numbers: %zu, confirmed size results: %zu)\n\n", size, scales.size(), pairs.size());
    unsigned n = 0;
    for (const auto& e : scales) {
        if (++n > 8) break;
        double v = 0; ReadKind(reader(), e.address, e.kind, &v);
        std::fprintf(f, "  CONFIRMED %s at %s, value now %g, found by [%s], followed %u\n", hunt::KindName(e.kind), where_text(e.address).c_str(), v,
                     methods_text(e.methods).c_str(), e.followed);
    }
    {   // The writing trial: did the game follow when NeuralAlias set the number itself?
        std::lock_guard<std::mutex> l(g.trial_m);
        if (g.trial_wanted.load()) {
            const TrialRow* works = nullptr;
            for (const auto& r : g.trial_rows)
                if (!r.skipped && (r.verdict == hunt::Verdict::Responded || r.verdict == hunt::Verdict::Snapped) && r.restored && !works) works = &r;
            std::fprintf(f, "\nWRITING TRIAL (NeuralAlias sets the number itself; the game's own value is put back after each try)\n");
            if (g.trial_state == 0)
                std::fprintf(f, "HIJACK: NOT RUN YET. It starts by itself once a scale number is confirmed and the world has been steady for 6 seconds.\n");
            else if (g.trial_state == 1) std::fprintf(f, "HIJACK: RUNNING (%zu tried so far)\n", g.trial_rows.size());
            else if (g.trial_state == 3) std::fprintf(f, "HIJACK: STOPPED. %s\n", g.trial_note.c_str());
            else if (works)
                std::fprintf(f, "HIJACK: PASS. Writing %s at %s moved the game's 3D world to %.1f%% in %u ms; the game %s. It was put back afterwards.\n",
                             hunt::KindName(works->kind), where_text(works->address).c_str(), works->achieved * 100.0, works->respond_ms,
                             works->hold == hunt::Hold::Once ? "kept it with one write" : works->hold == hunt::Hold::EveryFrame ? "needs it written every frame"
                             : works->hold == hunt::Hold::Periodic ? "needs it written again on a rhythm" : "puts its own value back now and then, and a new write restores it");
            else std::fprintf(f, "HIJACK: FAIL. None of the %zu confirmed numbers moved the world when written: this game applies its render scale "
                              "through a command, not by reading the number each frame.\n", g.trial_rows.size());
            for (const auto& r : g.trial_rows) {
                if (r.skipped) { std::fprintf(f, "  %s at %s: not tried (%s)\n", hunt::KindName(r.kind), where_text(r.address).c_str(), r.note); continue; }
                std::fprintf(f, "  %s at %s: was %g, asked %.0f%% -> %s", hunt::KindName(r.kind), where_text(r.address).c_str(), r.before, r.asked * 100.0,
                             hunt::VerdictName(r.verdict));
                if (r.verdict == hunt::Verdict::Responded || r.verdict == hunt::Verdict::Snapped)
                    std::fprintf(f, " at %.1f%% after %u ms, hold: %s, reverts %u", r.achieved * 100.0, r.respond_ms, hunt::HoldName(r.hold), r.reverts);
                std::fprintf(f, "; writes %u; the game %s our value in memory; put back: %s%s%s\n", r.writes, r.kept_value ? "kept" : "replaced",
                             r.restored ? "yes" : "NO", r.note[0] ? "; " : "", r.note);
            }
            if (!g.trial_paths.empty()) {
                std::fprintf(f, "  How to reach the working number from the game's program on a later launch (%zu routes):\n", g.trial_paths.size());
                for (const auto& path : g.trial_paths) std::fprintf(f, "    %s\n", path.c_str());
            }
        }
    }
    std::fclose(f);
}
void memory_worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        hunt::Target t; bool event = false;
        {
            std::unique_lock<std::mutex> l(g.mem_m);
            g.mem_cv.wait_for(l, std::chrono::seconds(5), [] { return !g.sizes.empty(); });
            if (!g.sizes.empty()) { t = g.sizes.back(); g.sizes.clear(); event = true; } // only the newest size matters
        }
        // The writing trial changes the world's size itself: those changes are not the player's, and
        // prove nothing about the candidates. Nothing is counted during it or for 6 s after.
        const uint64_t ended = g.trial_ended_ms.load();
        const bool ours = g.trial_active.load() || (ended && GetTickCount64() - ended < 6000);
        if (!event) {
            // Quiet time: a candidate that changes while the world does not is a timer, not a setting.
            if (!ours) {
                std::lock_guard<std::mutex> l(g.board_m);
                if (g.have_target) g.board.Steady(g.target, &holds);
            }
            write_verdict();
            continue;
        }
        if (ours) continue;
        {
            std::lock_guard<std::mutex> l(g.board_m);
            if (g.have_target && t.world_w == g.target.world_w && t.world_h == g.target.world_h &&
                t.out_w == g.target.out_w && t.out_h == g.target.out_h) continue; // back where it was
        }
        g.last_size_event_ms.store(GetTickCount64());
        bool first, scale_changed = false;
        {
            std::lock_guard<std::mutex> l(g.board_m);
            first = !g.have_target;
            // Only a change of world/screen is the render scale moving. A resized window or a new
            // screen mode changes both together and proves nothing about the scale.
            scale_changed = !first && std::fabs(t.Ratio() - g.target.Ratio()) > 0.01;
            if (scale_changed) g.board.SizeChanged(t, &holds);
            g.target = t; g.have_target = true;
        }
        if (first) line("world: %ux%u on a %ux%u screen (scale %.4f)", t.world_w, t.world_h, t.out_w, t.out_h, t.Ratio());
        else if (scale_changed) {
            g.scale_changes.fetch_add(1);
            line("render scale changed: world now %ux%u on a %ux%u screen (scale %.4f)", t.world_w, t.world_h, t.out_w, t.out_h, t.Ratio());
            log_ranking("after the scale change, before the new search");
        } else line("screen size changed (same scale): world now %ux%u on a %ux%u screen", t.world_w, t.world_h, t.out_w, t.out_h);
        g.sweeping.store(true);
        sweep(t); // also finds settings objects the game created since the last search
        g.sweeping.store(false);
        g.last_size_event_ms.store(GetTickCount64());
        log_ranking(first ? "after the first search" : "after the new search");
        write_verdict();
    }
}

void enqueue_chain(const char* api, uint32_t method, uint32_t w, uint32_t h)
{
    Capture c;
    c.api = api; c.method = method; c.w = w; c.h = h;
    c.count = RtlCaptureStackBackTrace(0, kFrames, c.frames, nullptr); // our own frames are filtered out as "not game code"
    {
        // Never wait on a background thread from the game's thread: skip this capture instead.
        std::unique_lock<std::mutex> l(g.code_m, std::try_to_lock);
        if (!l.owns_lock() || g.captures.size() >= 32) return;
        g.captures.push_back(c);
    }
    g.code_cv.notify_one();
}
} // namespace

namespace {
// ---- the writing trial ----
bool safe_write(uint64_t address, const void* in, size_t n)
{
    MEMORY_BASIC_INFORMATION m{};
    if (address < 0x10000 || !VirtualQuery(ptr(address), &m, sizeof m) || m.State != MEM_COMMIT || !writable_protect(m.Protect)) return false;
    if (address + n > reinterpret_cast<uint64_t>(m.BaseAddress) + m.RegionSize) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), const_cast<void*>(ptr(address)), in, n, &put) && put == n;
}
size_t kind_bytes(hunt::Kind k) { return k == hunt::Kind::ScaleF64 ? 8 : 4; }
bool write_scale(uint64_t address, hunt::Kind k, double scale)
{
    switch (k) {
    case hunt::Kind::ScaleF32: { const float v = float(scale); return safe_write(address, &v, 4); }
    case hunt::Kind::ScaleF64: return safe_write(address, &scale, 8);
    case hunt::Kind::PercentI32: { const int32_t v = int32_t(scale * 100.0 + 0.5); return safe_write(address, &v, 4); }
    case hunt::Kind::PercentF32: { const float v = float(scale * 100.0); return safe_write(address, &v, 4); }
    default: return false; // a size is the result, not the command
    }
}
// The world as it is drawn right now. False in a menu or on a loading screen.
bool live_frame(hunt::Frame* f, uint64_t index)
{
    f->index = index;
    f->out_w = g.out_w.load(std::memory_order_relaxed); f->out_h = g.out_h.load(std::memory_order_relaxed);
    const uint64_t size = g.live_size.load(std::memory_order_relaxed), at = g.live_ms.load(std::memory_order_relaxed);
    f->world_seen = size && f->out_w && GetTickCount64() - at < 400;
    f->world_w = uint32_t(size >> 32); f->world_h = uint32_t(size & 0xffffffffu);
    return f->world_seen;
}
constexpr DWORD kTickMs = 16;
// Ask for `goal` by writing `address` whenever the judge says a write is due; watch for up to `watch_ms`
// after the game answered (that is where a game that puts its own value back shows itself).
void drive(hunt::ResponseJudge& judge, uint64_t address, hunt::Kind kind, double goal, uint32_t watch_ms, TrialRow* row)
{
    uint64_t index = 0;
    const uint64_t began = GetTickCount64();
    uint64_t answered = 0;
    judge.Ask(goal, index);
    for (;;) {
        ++index;
        if (judge.NeedsCommand(index)) {
            if (write_scale(address, kind, goal)) { judge.Commanded(index); if (row) ++row->writes; }
            else break; // the memory went away
        }
        Sleep(kTickMs);
        hunt::Frame f;
        live_frame(&f, index);
        const hunt::Verdict v = judge.Observe(f);
        const uint64_t now = GetTickCount64();
        if ((v == hunt::Verdict::Responded || v == hunt::Verdict::Snapped) && !answered) {
            answered = now;
            if (row) row->respond_ms = uint32_t(now - began);
        }
        if (v == hunt::Verdict::Ignored || v == hunt::Verdict::Refused) break;
        if (answered && now - answered >= watch_ms) break;
        if (now - began > 20000) break; // menus give no evidence: never wait for ever
    }
}
// Where in the game's memory do pointers to these places live? One pass over memory for all of them.
struct PointerHit { uint64_t at = 0; uint32_t offset = 0; uint32_t child = 0; bool in_program = false; };
void scan_pointers(const std::vector<uint64_t>& targets, uint32_t reach, std::vector<PointerHit>* out, size_t limit)
{
    if (targets.empty()) return;
    std::vector<std::pair<uint64_t, uint32_t>> sorted; // address, index in targets
    for (uint32_t i = 0; i < targets.size(); ++i) sorted.emplace_back(targets[i], i);
    std::sort(sorted.begin(), sorted.end());
    const uint64_t lowest = sorted.front().first >= reach ? sorted.front().first - reach : 0, highest = sorted.back().first;
    std::vector<uint8_t> buffer(kChunk + 8);
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uint64_t at = reinterpret_cast<uint64_t>(si.lpMinimumApplicationAddress), scanned = 0;
    const uint64_t top = reinterpret_cast<uint64_t>(si.lpMaximumApplicationAddress);
    const uint64_t own = reinterpret_cast<uint64_t>(buffer.data());
    while (at < top && scanned < kSweepBudget && out->size() < limit) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(ptr(at), &m, sizeof m)) break;
        const uint64_t base = reinterpret_cast<uint64_t>(m.BaseAddress), size = m.RegionSize;
        at = base + size;
        if (m.State != MEM_COMMIT || !writable_protect(m.Protect) || m.Type == MEM_MAPPED) continue;
        if (base <= own && own < base + size) continue;
        const bool in_program = m.Type == MEM_IMAGE;
        for (uint64_t off = 0; off < size && scanned < kSweepBudget && out->size() < limit; off += kChunk) {
            const size_t want = size_t((std::min<uint64_t>)(kChunk, size - off));
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), ptr(base + off), buffer.data(), want, &got) || got < 8) break;
            scanned += got;
            for (size_t i = 0; i + 8 <= got; i += 8) {
                uint64_t v; std::memcpy(&v, buffer.data() + i, 8);
                if (v < lowest || v > highest) continue;
                auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(v, uint32_t(0)));
                if (it == sorted.end() || it->first - v > reach) continue;
                out->push_back(PointerHit{base + off + i, uint32_t(it->first - v), it->second, in_program});
                if (out->size() >= limit) break;
            }
            if (((off / kChunk) & 15) == 15) Sleep(1);
        }
    }
}
// Routes from the game's own program (a fixed place on every launch) to the working number: at most
// three pointers deep. Logged and put in the result; a later launch can follow one instead of searching.
void find_routes(uint64_t address)
{
    struct Node { uint64_t at; uint32_t offset; int parent; }; // parent = index of the node this one points to (-1: the number)
    std::vector<Node> nodes;
    std::vector<uint64_t> targets{address};
    std::vector<int> target_node{-1};
    std::vector<std::string> routes;
    constexpr uint32_t kReach = 0x1000;
    const uint64_t began = GetTickCount64();
    for (int depth = 1; depth <= 3 && routes.size() < 8 && !targets.empty(); ++depth) {
        std::vector<PointerHit> hits;
        scan_pointers(targets, kReach, &hits, 200000);
        // Nearest pointers first: an object's own fields are close to its start.
        std::sort(hits.begin(), hits.end(), [](const PointerHit& a, const PointerHit& b) {
            return a.in_program != b.in_program ? a.in_program : a.offset < b.offset;
        });
        std::vector<uint64_t> next; std::vector<int> next_node;
        for (const PointerHit& h : hits) {
            const int node = int(nodes.size());
            if (h.in_program) {
                if (routes.size() >= 8) continue;
                // Written from the program outwards: [program place] +offset -> ... -> the number.
                std::string text = where_text(h.at);
                char part[48];
                std::snprintf(part, sizeof part, " -> +0x%x", h.offset);
                text += part;
                for (int p = target_node[h.child]; p >= 0; p = nodes[size_t(p)].parent) {
                    std::snprintf(part, sizeof part, " -> +0x%x", nodes[size_t(p)].offset);
                    text += part;
                }
                routes.push_back(text);
            } else if (next.size() < 2048) {
                nodes.push_back(Node{h.at, h.offset, target_node[h.child]});
                next.push_back(h.at); next_node.push_back(node);
            }
        }
        line("trial: route search depth %d: %zu pointers found, %zu routes from the program so far", depth, hits.size(), routes.size());
        targets.swap(next); target_node.swap(next_node);
    }
    line("trial: route search took %.1f s; %zu routes", double(GetTickCount64() - began) / 1000.0, routes.size());
    for (const auto& r : routes) line("trial: route %s", r.c_str());
    std::lock_guard<std::mutex> l(g.trial_m);
    g.trial_paths = routes;
}
void run_trial()
{
    std::vector<hunt::Entry> scales; hunt::Target start;
    {
        std::lock_guard<std::mutex> l(g.board_m);
        start = g.target;
        for (const auto& e : g.board.Ranked(256))
            if (e.Confirmed() && !hunt::IsSizePair(e.kind) && scales.size() < 12) scales.push_back(e);
    }
    const double home = start.Ratio();
    // Ask for something clearly different from where the game is, and inside what any game allows.
    const double goal = home > 0.85 ? 0.70 : 1.0;
    line("trial: starting. World %ux%u (%.1f%%), asking each of %zu confirmed numbers for %.0f%%", start.world_w, start.world_h, home * 100.0, scales.size(), goal * 100.0);
    { std::lock_guard<std::mutex> l(g.trial_m); g.trial_state = 1; }
    g.trial_active.store(true);
    uint64_t works = 0; bool stopped = false;
    for (const hunt::Entry& e : scales) {
        TrialRow row; row.address = e.address; row.kind = e.kind; row.asked = goal;
        uint8_t saved[8]{};
        const size_t n = kind_bytes(e.kind);
        hunt::Frame now_frame;
        if (!live_frame(&now_frame, 0)) { row.skipped = true; row.note = "the game was in a menu or loading"; }
        else if (!safe_read(e.address, saved, n) || !holds(e.address, e.kind, start)) { row.skipped = true; row.note = "it no longer holds the game's current scale"; }
        else if (!ReadKind(reader(), e.address, e.kind, &row.before)) { row.skipped = true; row.note = "it could not be read"; }
        if (!row.skipped) {
            hunt::ResponseJudge judge;
            drive(judge, e.address, e.kind, goal, 3000, &row);
            row.verdict = judge.State(); row.hold = judge.HoldPolicy(); row.reverts = judge.Reverts(); row.achieved = judge.Achieved();
            if (row.verdict == hunt::Verdict::Reverted || row.verdict == hunt::Verdict::Waiting)
                row.verdict = row.respond_ms ? hunt::Verdict::Responded : hunt::Verdict::Ignored; // ended mid-watch
            hunt::Target asked = start; asked.world_w = uint32_t(start.out_w * goal + 0.5); asked.world_h = uint32_t(start.out_h * goal + 0.5);
            row.kept_value = holds(e.address, e.kind, asked);
            // Put the game's own value back, and make sure the world is back before the next try.
            safe_write(e.address, saved, n);
            hunt::Frame f;
            const uint64_t t0 = GetTickCount64();
            bool back = false;
            int steady = 0;
            while (GetTickCount64() - t0 < 6000 && !back) {
                Sleep(kTickMs);
                if (!live_frame(&f, 0)) continue;
                steady = std::fabs(double(f.world_w) / double(f.out_w) - home) <= 0.012 ? steady + 1 : 0;
                back = steady >= 20;
            }
            if (!back) { // the game kept our size: ask for the original the same way
                hunt::ResponseJudge undo;
                drive(undo, e.address, e.kind, home, 500, nullptr);
                safe_write(e.address, saved, n);
                back = undo.State() == hunt::Verdict::Responded || undo.State() == hunt::Verdict::Snapped;
            }
            row.restored = back;
            line("trial: %s at %s: was %g, asked %.0f%% -> %s (achieved %.1f%%, %u ms, hold %s, reverts %u, writes %u, value %s, put back %s)",
                 hunt::KindName(e.kind), where_text(e.address).c_str(), row.before, goal * 100.0, hunt::VerdictName(row.verdict), row.achieved * 100.0,
                 row.respond_ms, hunt::HoldName(row.hold), row.reverts, row.writes, row.kept_value ? "kept" : "replaced", back ? "yes" : "NO");
            if (!works && back && (row.verdict == hunt::Verdict::Responded || row.verdict == hunt::Verdict::Snapped)) works = e.address;
            if (!back) stopped = true;
        } else line("trial: %s at %s: not tried (%s)", hunt::KindName(e.kind), where_text(e.address).c_str(), row.note);
        { std::lock_guard<std::mutex> l(g.trial_m); g.trial_rows.push_back(row); }
        write_verdict();
        if (stopped) break;
        Sleep(700);
    }
    g.trial_ended_ms.store(GetTickCount64());
    g.trial_active.store(false);
    {
        std::lock_guard<std::mutex> l(g.trial_m);
        g.trial_state = stopped ? 3 : 2;
        if (stopped) g.trial_note = "After one try the game's 3D world did not return to its own size, so no further numbers were tried. "
                                    "Set Render Scale in the game's menu to put it right.";
    }
    write_verdict();
    if (works) { find_routes(works); write_verdict(); }
    line("trial: finished");
}
void trial_worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Sleep(500);
        if (g.sweeping.load()) continue;
        const uint64_t last = g.last_size_event_ms.load();
        if (!last || GetTickCount64() - last < 6000) continue; // the player may still be changing it
        bool ready = false;
        {
            std::lock_guard<std::mutex> l(g.board_m);
            if (g.have_target)
                for (const auto& e : g.board.Ranked(256)) if (e.Confirmed() && !hunt::IsSizePair(e.kind)) { ready = true; break; }
        }
        hunt::Frame f;
        if (!ready || !live_frame(&f, 0)) continue;
        run_trial();
        return; // once a run
    }
}
} // namespace

void SetTrial(bool on) { g.trial_wanted.store(on); }
void Start(const wchar_t* log_path, const wchar_t* verdict_path)
{
    if (g.active.load() || !log_path || !*log_path) return;
    g.log_path = log_path;
    if (verdict_path) g.verdict_path = verdict_path;
    g.self_base = reinterpret_cast<uint64_t>(module_handle(reinterpret_cast<uint64_t>(&Start), false));
    try {
        std::thread(code_worker).detach();
        std::thread(memory_worker).detach();
        if (g.trial_wanted.load()) std::thread(trial_worker).detach();
    } catch (...) { return; }
    g.active.store(true);
}
bool Active() { return g.active.load(std::memory_order_relaxed); }
std::atomic<bool> g_test_mode{false};
void SetTestMode(bool on) { g_test_mode.store(on); }
bool TestMode() { return g_test_mode.load(); }

void NoteOutput(uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    g.out_w.store(w, std::memory_order_relaxed);
    g.out_h.store(h, std::memory_order_relaxed);
}

void OnWorldViewport(const char* api, float fw, float fh)
{
    if (!g.active.load(std::memory_order_relaxed) || !(fw >= 64) || !(fh >= 64) || fw > 32768 || fh > 32768) return;
    const uint32_t w = uint32_t(fw + 0.5f), h = uint32_t(fh + 0.5f);
    g.viewports.fetch_add(1, std::memory_order_relaxed);
    const uint64_t size = pack(w, h), now = GetTickCount64();
    {   // The size right now: the largest viewport of each 100 ms.
        uint64_t fb = g.fast_best.load(std::memory_order_relaxed);
        if (uint64_t(w) * h > (fb >> 32) * (fb & 0xffffffffu)) g.fast_best.store(size, std::memory_order_relaxed);
        uint64_t fs = g.fast_start.load(std::memory_order_relaxed);
        if (now - fs >= 100 && g.fast_start.compare_exchange_strong(fs, now, std::memory_order_relaxed)) {
            const uint64_t done = g.fast_best.exchange(0, std::memory_order_relaxed);
            if (done) { g.live_size.store(done, std::memory_order_relaxed); g.live_ms.store(now, std::memory_order_relaxed); }
        }
    }
    // The world's size = the largest viewport set on it within a window (smaller ones are
    // part-screen passes). A size must hold for two windows before it counts as a change.
    uint64_t best = g.window_best.load(std::memory_order_relaxed);
    if (uint64_t(w) * h > (best >> 32) * (best & 0xffffffffu)) g.window_best.store(size, std::memory_order_relaxed);
    uint64_t start = g.window_start.load(std::memory_order_relaxed);
    if (now - start >= kWindowMs && g.window_start.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        best = g.window_best.exchange(0, std::memory_order_relaxed);
        const uint64_t last = g.window_last.exchange(best, std::memory_order_relaxed);
        const uint32_t ow = g.out_w.load(std::memory_order_relaxed), oh = g.out_h.load(std::memory_order_relaxed);
        if (best && best == last && best != g.published.load(std::memory_order_relaxed) && ow && oh) {
            // Never wait on a background thread from the game's thread: try again next window.
            std::unique_lock<std::mutex> l(g.mem_m, std::try_to_lock);
            if (l.owns_lock()) {
                g.published.store(best, std::memory_order_relaxed);
                g.sizes.push_back(hunt::Target{uint32_t(best >> 32), uint32_t(best & 0xffffffffu), ow, oh});
                l.unlock();
                g.mem_cv.notify_one();
            }
        }
    }
    // Call chain: at once for a size not seen before, otherwise one every 2 s (other passes).
    const bool due = now >= g.next_chain_ms.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(g.seen_m);
        const bool fresh = g.viewport_sizes.size() < kMaxSizesSeen && g.viewport_sizes.insert(size).second;
        if (!fresh && !due) return;
    }
    g.next_chain_ms.store(now + kChainIntervalMs, std::memory_order_relaxed);
    enqueue_chain(api, hunt::kCallChain, w, h);
}

void OnTargetCreated(const char* api, uint32_t w, uint32_t h)
{
    if (!g.active.load(std::memory_order_relaxed) || w < 64 || h < 64) return;
    const uint32_t ow = g.out_w.load(std::memory_order_relaxed), oh = g.out_h.load(std::memory_order_relaxed);
    if (!ow || !oh) return;
    // A world-shaped picture: the screen's shape, between a quarter and twice its size.
    const double shape = double(w) / double(h), screen = double(ow) / double(oh);
    if (std::fabs(shape - screen) > 0.01 * screen || uint64_t(w) * 4 < ow || w > uint64_t(ow) * 2) return;
    {
        std::lock_guard<std::mutex> l(g.seen_m);
        const uint64_t size = pack(w, h);
        if (!g.created_sizes.count(size)) {
            if (g.created_sizes.size() >= kMaxSizesSeen) return;
            g.created_sizes.insert(size);
        }
        if (++g.created_count[size] > 6) return; // the first few creators of each size
    }
    g.created.fetch_add(1, std::memory_order_relaxed);
    enqueue_chain(api, hunt::kCreation, w, h);
}
} // namespace na::scale::witness
