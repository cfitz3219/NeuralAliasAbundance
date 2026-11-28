#include "host_dx12/scale/scale_witness.hpp"
#include <windows.h>
#include <tlhelp32.h>
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
    // (the writer search reads modules too, under this lock)
    std::mutex modules_m; std::set<uint64_t> creation_functions; // game functions on the trails that created world-sized pictures
    std::map<uint64_t, std::string> named_functions; // game functions that use a render-scale setting name (under modules_m)
    // Who writes the confirmed numbers (processor write watch): filled by the exception handler.
    std::atomic<uint64_t> trial_writing{0}; // the number the trial is writing right now (0: none)
    std::vector<std::string> writer_lines;  // under trial_m
    // The world's size right now (largest viewport of the last 100 ms), for the writing trial.
    std::atomic<uint64_t> fast_start{0}, fast_best{0}, live_size{0}, live_ms{0};
    // The writing trial ([scale] trial=1): NeuralAlias sets each confirmed number itself and the judge
    // watches whether the game's world follows.
    std::atomic<bool> trial_wanted{false}, trial_active{false}, sweeping{false};
    // The Search ([scale] search=1): the number is found at launch with no slider moves.
    std::atomic<bool> search_wanted{false}, names_done{false};
    // The handle the Connection drives: the one number the game obeys (set once the writing trial passed).
    std::atomic<bool> control_ready{false}, control_thread{false};
    uint64_t control_address = 0; hunt::Kind control_kind = hunt::Kind::ScaleF32; double control_home = 1.0; // fixed once ready
    std::atomic<uint32_t> control_asked{0}; // wanted scale in 1/10000; 0: the game's own value
    int search_state = 0;            // under trial_m. 0 not started, 1 running, 2 found, 3 not found, 4 stopped
    std::string search_text;         // under trial_m: the one-sentence outcome and why
    std::vector<std::string> search_lines; // under trial_m: what each step saw
    std::vector<uint64_t> name_addresses;  // under modules_m: where the setting names are in the game's program
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
    std::lock_guard<std::mutex> modules_lock(g.modules_m);
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
        if (c.method == hunt::kCreation && g.creation_functions.size() < 4096) g.creation_functions.insert(mod->base + fn.start);
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
// Matched anywhere inside a name, so "ApplyRenderScale", "r.ScreenPercentage" and "m_renderScale" are all
// found by their noun. The list is what engines in general call this setting.
const char* const kWords[] = {
    "renderscale", "render_scale", "render scale", "renderingscale", "rendering_scale", "rendering scale",
    "resolutionscale", "resolution_scale", "resolution scale", "resscale", "res_scale", "resolutionscaling", "resolution_scaling",
    "screenpercentage", "screen_percentage", "screen percentage", "screenscale", "screen_scale",
    "renderresolution", "render_resolution", "render resolution", "renderingresolution", "rendering_resolution",
    "internalres", "internal_res", "internal res", "renderpercent", "render_percent", "resolutionpercent", "resolution_percent",
    "resolutionquality", "resolution_quality", "resolutionmultiplier", "resolution_multiplier", "resmult", "res_mult",
    "resolutionfactor", "resolution_factor", "resolutionmodifier", "resolution_modifier", "renderfactor", "render_factor",
    "scalefactor", "scale_factor", "scalingfactor", "scaling_factor", "pixeldensity", "pixel_density",
    "imagequality", "image_quality", "image quality", "upscal", "supersampl", "super_sampl", "subsampl", "downsampl",
    "superresolution", "super_resolution", "super resolution",
    "dynamicres", "dynamic_res", "dynamic res", "dynres", "dyn_res", "adaptiveres", "adaptive_res", "scalablebuffer",
    "viewportscale", "viewport_scale", "viewport scale", "bufferscale", "buffer_scale", "targetscale", "target_scale",
    "sceneresolution", "scene_resolution", "scenescale", "scene_scale", "worldresolution", "world_resolution",
    "3dscale", "3d_scale", "3d scale", "3dresolution", "3d_resolution", "3d resolution", "3dres", "3d_res"};
char lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); }
bool name_char(uint8_t p)
{
    return (p >= '0' && p <= '9') || (p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z') || p == '.' || p == '_' || p == ' ';
}
// Finds each word in the read-only data as ASCII (step 1) or UTF-16 (step 2), case-insensitive.
void find_words(const Image& im, std::vector<std::pair<uint32_t, std::string>>& out, size_t cap)
{
    bool first[256]{};
    for (const char* word : kWords) first[uint8_t(word[0])] = true;
    for (const auto& range : im.ranges) {
        if (range.executable) continue;
        const uint8_t* b = im.bytes + range.start;
        for (uint32_t step = 1; step <= 2; ++step)
            for (uint32_t i = 0; i + 8 < range.size && out.size() < cap; ++i) {
                const char c = lower(b[i]);
                if (!first[uint8_t(c)]) continue;
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
    find_words(mod.image, names, 2048);
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
            if (g.named_functions.size() < 8192) {
                std::string& uses = g.named_functions[mod.base + fn.start];
                if (uses.size() < 160 && uses.find(name->second) == std::string::npos) uses += (uses.empty() ? "" : ", ") + name->second;
            }
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
    for (const auto& n : by_rva) if (g.name_addresses.size() < 2048) g.name_addresses.push_back(mod.base + n.first);
    line("named: %s has %zu setting names, %u code references to %zu of them from %zu functions, candidates=%zu", mod.name.c_str(),
         by_rva.size(), refs, used.size(), g.named_functions.size(), found.size());
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
    {
        std::lock_guard<std::mutex> modules_lock(g.modules_m);
    if (Module* exe = module_at(reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)) + 0x1000)) named_search(*exe);
    }
    g.names_done.store(true);
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
    if (g.search_wanted.load() && !changes) { // hands-free run: the outcome is the Search's, not the slider test's
        std::lock_guard<std::mutex> l(g.trial_m);
        pass = g.search_state == 2;
        if (g.search_state >= 2) std::snprintf(reason, sizeof reason, "%s", g.search_text.c_str());
        else if (have) std::snprintf(reason, sizeof reason, "%s", g.search_state == 1 ? "The Search is running." : "The Search has not started yet: it "
                           "starts by itself once the game's 3D world has been on screen and steady for 6 seconds. No slider moves are needed.");
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, g.verdict_path.c_str(), L"w") != 0 || !f) return;
    SYSTEMTIME now; GetSystemTime(&now);
    std::fprintf(f, "NeuralAlias render-scale finder test (%s)\n", g.search_wanted.load() ? "The Search: candidate numbers are nudged by 5 points "
                 "for under a second each and put back" : g.trial_wanted.load() ? "confirmed numbers are nudged by 5 points and put back"
                 : "log only: nothing in the game was changed");
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
        if (g.search_wanted.load()) {
            static const char* const kState[] = {"NOT STARTED", "RUNNING", "FOUND", "NOT FOUND", "STOPPED"};
            std::fprintf(f, "\nTHE SEARCH (NeuralAlias finds the render-scale number by itself; no slider moves)\n");
            std::fprintf(f, "SEARCH: %s. %s\n", kState[g.search_state], g.search_text.c_str());
            for (const auto& s : g.search_lines) std::fprintf(f, "  %s\n", s.c_str());
            if (g.search_state == 2) std::fprintf(f, "NEXT STAGE: THE CONNECTION (DLSS on the game's smaller picture; Quality, Balanced, Performance).\n");
            else if (g.search_state >= 3) std::fprintf(f, "NEXT STAGE: THE FOLD (the Search did not succeed; Quality and Performance only, Balanced unavailable).\n");
        }
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
            if (!g.writer_lines.empty()) {
                std::fprintf(f, "  THE GAME'S OWN COMMAND (game code seen writing the confirmed numbers; %zu writing place(s)):\n", g.writer_lines.size());
                for (const auto& w : g.writer_lines) std::fprintf(f, "    %s\n", w.c_str());
            } else if (g.trial_state >= 2)
                std::fprintf(f, "  THE GAME'S OWN COMMAND: no game code was seen writing the confirmed numbers while the watch was on.\n");
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
// ---- who writes the number: the game's own "apply render scale" code gives itself away ----
// A processor write watch on up to four confirmed numbers (the debug registers of every thread of the
// game). When game code changes one, the processor reports the instruction and the handler keeps the
// trail of calls that led there. Log only: nothing in the game is changed by it.
#if defined(_M_X64)
constexpr unsigned kWatchSlots = 4, kWatchHits = 256, kWatchPerSlot = 48, kWatchFrames = 20;
struct WatchHit { uint64_t rip, during, tick; uint32_t slot, thread; void* frames[kWatchFrames]; unsigned count; };
uint64_t g_watch_address[kWatchSlots]{};
hunt::Kind g_watch_kind[kWatchSlots]{};
std::atomic<bool> g_watch_on{false}, g_watch_done{false};
std::atomic<uint32_t> g_watch_count{0}, g_watch_slot_hits[kWatchSlots]{};
WatchHit g_watch_hits[kWatchHits];
void* g_watch_handler = nullptr;

LONG CALLBACK watch_handler(EXCEPTION_POINTERS* x)
{
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch_on.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = x->ContextRecord;
    const DWORD64 which = c->Dr6 & 0xf;
    if (!which) return EXCEPTION_CONTINUE_SEARCH; // somebody else's single step
    for (uint32_t slot = 0; slot < kWatchSlots; ++slot) {
        if (!(which & (1ull << slot))) continue;
        const uint32_t seen = g_watch_slot_hits[slot].fetch_add(1, std::memory_order_relaxed);
        if (seen >= kWatchPerSlot) { c->Dr7 &= ~(3ull << (slot * 2)); continue; } // a number rewritten every frame: enough seen, stop watching it on this thread
        const uint32_t i = g_watch_count.fetch_add(1, std::memory_order_relaxed);
        if (i >= kWatchHits) continue;
        WatchHit& h = g_watch_hits[i];
        h.rip = c->Rip; h.slot = slot; h.thread = GetCurrentThreadId(); h.tick = GetTickCount64();
        h.during = g.trial_writing.load(std::memory_order_relaxed);
        // The trail, walked from where the game was (not from inside this handler). First entry: the
        // instruction after the write. A function with no unwind record is a leaf: its caller is on the stack.
        CONTEXT u = *c;
        h.count = 0;
        h.frames[h.count++] = reinterpret_cast<void*>(u.Rip);
        while (h.count < kWatchFrames) {
            DWORD64 base = 0;
            if (PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(u.Rip, &base, nullptr)) {
                void* handler_data = nullptr; DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, rf, &u, &handler_data, &frame, nullptr);
            } else {
                DWORD64 ret = 0;
                if (!safe_read(u.Rsp, &ret, sizeof ret)) break;
                u.Rip = ret; u.Rsp += 8;
            }
            if (u.Rip < 0x10000) break;
            h.frames[h.count++] = reinterpret_cast<void*>(u.Rip);
        }
    }
    c->Dr6 &= ~0xfull;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS; // so a watch turned off above stays off
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Set (or clear) the watch on every thread of the game but this one. Between suspending a thread and
// letting it go on, only kernel calls are made: nothing here can wait on a lock that thread holds.
unsigned watch_threads(bool on)
{
    static DWORD ids[1024];
    unsigned n = 0, done = 0;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{}; te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok && n < 1024; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids[n++] = te.th32ThreadID;
    CloseHandle(snap);
    DWORD64 dr7 = 0;
    if (on)
        for (unsigned slot = 0; slot < kWatchSlots; ++slot) {
            if (!g_watch_address[slot]) continue;
            const DWORD64 len = g_watch_kind[slot] == hunt::Kind::ScaleF64 ? 2 : 3; // 8 bytes : 4 bytes
            dr7 |= (1ull << (slot * 2)) | (1ull << (16 + slot * 4)) | (len << (18 + slot * 4)); // local enable, break on write
        }
    for (unsigned i = 0; i < n; ++i) {
        const HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, ids[i]);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = g_watch_address[0]; c.Dr1 = g_watch_address[1]; c.Dr2 = g_watch_address[2]; c.Dr3 = g_watch_address[3];
                c.Dr6 = 0; c.Dr7 = dr7; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SetThreadContext(t, &c)) ++done;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    return done;
}
void watch_start(const std::vector<hunt::Entry>& scales)
{
    unsigned slots = 0;
    for (const hunt::Entry& e : scales) {
        if (slots >= kWatchSlots) break;
        const uint64_t mask = e.kind == hunt::Kind::ScaleF64 ? 7 : 3;
        if (e.address & mask) continue; // the processor watches aligned places only
        g_watch_address[slots] = e.address; g_watch_kind[slots] = e.kind; ++slots;
    }
    if (!slots) { line("writer watch: none of the confirmed numbers can be watched"); return; }
    if (!g_watch_handler) g_watch_handler = AddVectoredExceptionHandler(1, &watch_handler);
    g_watch_on.store(true);
    const unsigned threads = watch_threads(true);
    line("writer watch: on for %u confirmed number(s) on %u game threads. The next change of render scale shows which game code writes them", slots, threads);
}
// What the watch saw, in words: one line per distinct writing instruction.
void watch_report(bool final)
{
    const uint32_t n = (std::min)(g_watch_count.load(), uint32_t(kWatchHits));
    struct Writer { uint64_t rip; uint32_t slot; unsigned times, during_trial; uint32_t first; };
    std::vector<Writer> writers;
    for (uint32_t i = 0; i < n; ++i) {
        const WatchHit& h = g_watch_hits[i];
        auto it = std::find_if(writers.begin(), writers.end(), [&](const Writer& w) { return w.rip == h.rip && w.slot == h.slot; });
        if (it == writers.end()) { writers.push_back(Writer{h.rip, h.slot, 0, 0, i}); it = writers.end() - 1; }
        ++it->times; if (h.during) ++it->during_trial;
    }
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> modules_lock(g.modules_m);
        for (const Writer& w : writers) {
            const WatchHit& h = g_watch_hits[w.first];
            std::string text = where_text(g_watch_address[w.slot]) + " is written by game code at " + where_text(h.rip - 1);
            char buf[200];
            std::snprintf(buf, sizeof buf, " (%u time(s), %u of them right after NeuralAlias wrote another number, thread %u). Called from: ", w.times, w.during_trial, h.thread);
            if (g_watch_slot_hits[w.slot].load() >= kWatchPerSlot)
                text.insert(text.find(" is written by"), " [rewritten constantly: a copy the game keeps refreshing]");
            text += buf;
            unsigned shown = 0; std::string shared, named;
            for (unsigned k = 0; k < h.count && shown < kGameFrames; ++k) {
                const uint64_t a = reinterpret_cast<uint64_t>(h.frames[k]) - 1;
                Module* mod = module_at(a);
                if (!mod || mod->base == g.self_base) continue;
                const Function fn = locator::owner(mod->image, uint32_t(a - mod->base));
                if (!fn.size) continue;
                std::snprintf(buf, sizeof buf, "%s%s+0x%x", shown ? " <- " : "", mod->name.c_str(), fn.start);
                text += buf; ++shown;
                if (shared.empty() && g.creation_functions.count(mod->base + fn.start)) { std::snprintf(buf, sizeof buf, "%s+0x%x", mod->name.c_str(), fn.start); shared = buf; }
                if (named.empty()) {
                    const auto uses = g.named_functions.find(mod->base + fn.start);
                    if (uses != g.named_functions.end()) { std::snprintf(buf, sizeof buf, "%s+0x%x", mod->name.c_str(), fn.start); named = std::string(buf) + " (uses \"" + uses->second + "\")"; }
                }
            }
            if (!shown) text += "(no readable game code on the trail)";
            if (!shared.empty()) text += ". Nearest function that is also on a trail that rebuilt the 3D pictures: " + shared;
            if (!named.empty()) text += ". Nearest function on the trail that uses a render-scale setting name: " + named;
            lines.push_back(text);
        }
    }
    for (const auto& l : lines) line("writer: %s", l.c_str());
    if (lines.empty() && final) line("writer watch: no game code wrote the watched numbers while it was on");
    std::lock_guard<std::mutex> l(g.trial_m);
    g.writer_lines = lines;
}
void watch_stop()
{
    if (!g_watch_on.load()) return;
    watch_threads(false);
    g_watch_on.store(false);
    g_watch_done.store(true);
    for (auto& a : g_watch_address) a = 0;
}
bool watch_on() { return g_watch_on.load(); }
bool watch_done() { return g_watch_done.load(); }
uint32_t watch_count() { return g_watch_count.load(); }
void watch_rearm() { if (g_watch_on.load()) watch_threads(true); }
#else // the write watch is built for 64-bit games first
void watch_start(const std::vector<hunt::Entry>&) {}
void watch_report(bool) {}
void watch_stop() {}
bool watch_on() { return false; }
bool watch_done() { return true; }
uint32_t watch_count() { return 0; }
void watch_rearm() {}
#endif

void run_trial(const std::vector<hunt::Entry>& given = {})
{
    std::vector<hunt::Entry> scales; hunt::Target start;
    {
        std::lock_guard<std::mutex> l(g.board_m);
        start = g.target;
        for (const auto& e : g.board.Ranked(256))
            if (e.Confirmed() && !hunt::IsSizePair(e.kind) && scales.size() < 12) scales.push_back(e);
    }
    if (!given.empty()) scales = given; // the Search found it without the player's help
    const double home = start.Ratio();
    // A small nudge the player will not notice: five points down, or up when the game is already low.
    const double goal = home > 0.60 ? home - 0.05 : home + 0.05;
    watch_rearm(); // threads the game started since
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
            g.trial_writing.store(e.address);
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
            g.trial_writing.store(0);
            line("trial: %s at %s: was %g, asked %.0f%% -> %s (achieved %.1f%%, %u ms, hold %s, reverts %u, writes %u, value %s, put back %s)",
                 hunt::KindName(e.kind), where_text(e.address).c_str(), row.before, goal * 100.0, hunt::VerdictName(row.verdict), row.achieved * 100.0,
                 row.respond_ms, hunt::HoldName(row.hold), row.reverts, row.writes, row.kept_value ? "kept" : "replaced", back ? "yes" : "NO");
            if (!works && back && (row.verdict == hunt::Verdict::Responded || row.verdict == hunt::Verdict::Snapped)) {
                works = e.address;
                g.control_address = e.address; g.control_kind = e.kind; g.control_home = home;
            }
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
    Sleep(1500); // the game's own writes that answer our last one
    watch_report(true);
    watch_stop();
    write_verdict();
    if (works) {
        g.control_ready.store(true);
        line("control: ready. %s at %s is the handle for the game's render scale (its own value: %.1f%%)", hunt::KindName(g.control_kind),
             where_text(works).c_str(), home * 100.0);
        find_routes(works); write_verdict();
    }
    line("trial: finished");
}

// ---- THE SEARCH: find the render-scale number at launch, with no help from the player ----
// What never changes between launches: the number holds the starting value, it sits in an object that
// carries a render-scale name or beside the 3D picture's size, and the game obeys it. Candidates are
// gathered by those clues, then nudged by five points in halving groups until one number is left.
enum : uint8_t { kByCode = 1, kByName = 2, kBySize = 4 };
struct Cand { uint64_t address = 0; hunt::Kind kind = hunt::Kind::ScaleF32; uint8_t source = 0; uint8_t saved[8]{}; bool written = false; };

// The setting names as text in the game's writable memory: a settings object often carries its own name.
void sweep_name_text(std::vector<uint64_t>* found, size_t limit)
{
    bool first[256]{};
    for (const char* word : kWords) first[uint8_t(word[0])] = true;
    constexpr size_t kTail = 64;
    std::vector<uint8_t> buffer(kChunk + kTail);
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uint64_t at = reinterpret_cast<uint64_t>(si.lpMinimumApplicationAddress), scanned = 0;
    const uint64_t top = reinterpret_cast<uint64_t>(si.lpMaximumApplicationAddress);
    const uint64_t own = reinterpret_cast<uint64_t>(buffer.data());
    while (at < top && scanned < kSweepBudget && found->size() < limit) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(ptr(at), &m, sizeof m)) break;
        const uint64_t base = reinterpret_cast<uint64_t>(m.BaseAddress), size = m.RegionSize;
        at = base + size;
        if (m.State != MEM_COMMIT || !writable_protect(m.Protect) || m.Type == MEM_MAPPED) continue;
        if (base <= own && own < base + size) continue;
        for (uint64_t off = 0; off < size && scanned < kSweepBudget && found->size() < limit; off += kChunk) {
            const size_t want = size_t((std::min<uint64_t>)(kChunk + kTail, size - off));
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), ptr(base + off), buffer.data(), want, &got) || got < 8) break;
            const uint8_t* b = buffer.data();
            const size_t starts = (std::min<size_t>)(got, kChunk);
            scanned += starts;
            for (size_t i = 0; i < starts && found->size() < limit; ++i) {
                const char c = lower(b[i]);
                if (!first[uint8_t(c)]) continue;
                for (const char* word : kWords) {
                    if (word[0] != c) continue;
                    const size_t n = std::strlen(word);
                    if (i + n > got) continue;
                    bool same = true;
                    for (size_t k = 1; k < n && same; ++k) same = lower(b[i + k]) == word[k];
                    if (!same) continue;
                    size_t start = i;
                    while (start > 0 && i - start < 96 && name_char(b[start - 1])) --start;
                    found->push_back(base + off + start);
                    i += n;
                    break;
                }
            }
            if (((off / kChunk) & 15) == 15) Sleep(1);
        }
    }
    g.bytes_searched.fetch_add(scanned);
}
// Every number within `reach` bytes of `center` that holds the game's current scale becomes a candidate.
void add_near(uint64_t center, uint32_t reach, uint8_t source, const hunt::Target& t, std::map<uint64_t, Cand>* out, size_t limit)
{
    MEMORY_BASIC_INFORMATION m{};
    if (center < 0x10000 || !VirtualQuery(ptr(center), &m, sizeof m) || m.State != MEM_COMMIT || !writable_protect(m.Protect)) return;
    const uint64_t region = reinterpret_cast<uint64_t>(m.BaseAddress), region_end = region + m.RegionSize;
    const uint64_t lo = ((std::max)(region, center > reach ? center - reach : 0) + 3) & ~3ull;
    const uint64_t hi = (std::min)(region_end, center + reach + 8);
    uint8_t block[1040];
    if (hi <= lo + 4 || hi - lo > sizeof block) return;
    const size_t n = size_t(hi - lo);
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), ptr(lo), block, n, &got) || got != n) return;
    if (reinterpret_cast<uint64_t>(module_handle(lo, false)) == g.self_base && g.self_base) return; // never NeuralAlias's own numbers
    for (size_t o = 0; o + 4 <= n; o += 4) {
        const uint64_t address = lo + o;
        hunt::Kind kind = hunt::Kind::ScaleF32; bool is = true;
        if (hunt::Holds(block + o, n - o, hunt::Kind::ScaleF32, t)) kind = hunt::Kind::ScaleF32;
        else if (!(address & 7) && o + 8 <= n && hunt::Holds(block + o, n - o, hunt::Kind::ScaleF64, t)) kind = hunt::Kind::ScaleF64;
        else if (hunt::Holds(block + o, n - o, hunt::Kind::PercentF32, t)) kind = hunt::Kind::PercentF32;
        // A whole number such as 100 is too common beside a size; beside the setting's own name it is telling.
        else if ((source & kByName) && hunt::Holds(block + o, n - o, hunt::Kind::PercentI32, t)) kind = hunt::Kind::PercentI32;
        else is = false;
        if (!is) continue;
        auto it = out->find(address);
        if (it == out->end()) {
            if (out->size() >= limit) return;
            it = out->emplace(address, Cand{}).first;
            it->second.address = address; it->second.kind = kind;
        }
        it->second.source |= source;
    }
}
// Writes the nudge to every candidate in [lo, hi), watches the world, puts every value back and waits for
// the world to come home. 1: the world moved. 0: it did not. -1: no evidence (a menu). -2: it did not come home.
int nudge_once(std::vector<Cand>& c, size_t lo, size_t hi, const hunt::Target& start, double home, double goal)
{
    hunt::Frame f;
    if (!live_frame(&f, 0)) return -1;
    for (size_t i = lo; i < hi; ++i) {
        Cand& k = c[i];
        k.written = false;
        const size_t n = kind_bytes(k.kind);
        if (!safe_read(k.address, k.saved, n) || !hunt::Holds(k.saved, n, k.kind, start)) continue; // gone, or the game changed it
        k.written = write_scale(k.address, k.kind, goal);
    }
    const uint64_t t0 = GetTickCount64();
    int away = 0, seen = 0;
    bool moved = false;
    while (GetTickCount64() - t0 < 800 && !moved) {
        Sleep(kTickMs);
        if (!live_frame(&f, 0)) continue;
        ++seen;
        away = std::fabs(double(f.world_w) / double(f.out_w) - home) > 0.012 ? away + 1 : 0;
        moved = away >= 3;
    }
    for (size_t i = lo; i < hi; ++i)
        if (c[i].written) safe_write(c[i].address, c[i].saved, kind_bytes(c[i].kind));
    const uint64_t t1 = GetTickCount64();
    int steady = 0; bool back = false, live = false;
    while (GetTickCount64() - t1 < 5000 && !back) {
        Sleep(kTickMs);
        if (!live_frame(&f, 0)) continue;
        live = true;
        steady = std::fabs(double(f.world_w) / double(f.out_w) - home) <= 0.012 ? steady + 1 : 0;
        back = steady >= 6;
    }
    if (!back) return live ? -2 : -1;
    if (!seen) return -1;
    return moved ? 1 : 0;
}
int nudge(std::vector<Cand>& c, size_t lo, size_t hi, const hunt::Target& start, double home, double goal, unsigned* steps)
{
    for (int tries = 0; tries < 15; ++tries) {
        const int r = nudge_once(c, lo, hi, start, home, goal);
        ++*steps;
        if (r != -1) return r;
        Sleep(2000); // a menu or a loading screen: wait for the 3D world
    }
    return -1;
}
// Halving. Returns the index of the one number the game obeys, or -1 none in this group, -2 the world
// did not come home, -3 no evidence, -4 the group moved the world but no single number did.
long long hunt_in(std::vector<Cand>& c, size_t lo, size_t hi, const hunt::Target& start, double home, double goal, unsigned* steps)
{
    if (lo >= hi) return -1;
    int r = nudge(c, lo, hi, start, home, goal, steps);
    if (r != 1) return r == 0 ? -1 : r == -2 ? -2 : -3;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        r = nudge(c, lo, mid, start, home, goal, steps);
        if (r == 1) hi = mid;
        else if (r == 0) lo = mid; // then it is in the other half: checked alone below
        else return r == -2 ? -2 : -3;
    }
    for (int again = 0; again < 2; ++again) { // alone, twice: no coincidence
        r = nudge(c, lo, hi, start, home, goal, steps);
        if (r != 1) return r == 0 ? -4 : r == -2 ? -2 : -3;
    }
    return static_cast<long long>(lo);
}
void search_say(const char* fmt, ...)
{
    char buf[400];
    va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    line("search: %s", buf);
    std::lock_guard<std::mutex> l(g.trial_m);
    g.search_lines.push_back(buf);
}
void search_end(int state, const char* fmt, ...)
{
    char buf[480];
    va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    line("search: RESULT %s: %s", state == 2 ? "FOUND" : state == 4 ? "STOPPED" : "NOT FOUND", buf);
    std::lock_guard<std::mutex> l(g.trial_m);
    g.search_state = state; g.search_text = buf;
}
// True when the number was found (the writing trial then measured how it must be held).
bool run_search()
{
    hunt::Target start; std::vector<hunt::Entry> board;
    {
        std::lock_guard<std::mutex> l(g.board_m);
        start = g.target; board = g.board.Ranked(200000);
    }
    const double home = start.Ratio(), goal = home > 0.60 ? home - 0.05 : home + 0.05;
    const uint64_t began = GetTickCount64();
    { std::lock_guard<std::mutex> l(g.trial_m); g.search_state = 1; g.search_text = "The Search is running."; }
    g.trial_active.store(true);
    write_verdict();
    line("search: starting. World %ux%u on a %ux%u screen: the starting value is %.1f%%", start.world_w, start.world_h, start.out_w, start.out_h, home * 100.0);
    constexpr size_t kMaxCands = 8192;
    std::map<uint64_t, Cand> found;
    // Clue: numbers the game's own code reads beside a render-scale name, or on the trail that built the 3D pictures.
    for (const hunt::Entry& e : board) {
        if (hunt::IsSizePair(e.kind) || !(e.methods & ~uint32_t(hunt::kMemory)) || !holds(e.address, e.kind, start)) continue;
        if (found.size() >= kMaxCands) break;
        Cand& c = found[e.address];
        c.address = e.address; c.kind = e.kind; c.source |= kByCode;
    }
    const size_t by_code = found.size();
    // Clue: objects that carry a render-scale name (as text, or as a pointer to the text).
    std::vector<uint64_t> names;
    sweep_name_text(&names, 4096);
    std::vector<uint64_t> targets = names;
    { std::lock_guard<std::mutex> l(g.modules_m); targets.insert(targets.end(), g.name_addresses.begin(), g.name_addresses.end()); }
    std::vector<PointerHit> pointers;
    if (!targets.empty()) scan_pointers(targets, 0, &pointers, 8192);
    for (const uint64_t a : names) add_near(a, 256, kByName, start, &found, kMaxCands);
    for (const PointerHit& p : pointers) add_near(p.at, 256, kByName, start, &found, kMaxCands);
    const size_t strong = found.size();
    // Clue: numbers right beside where the game keeps the 3D picture's size.
    size_t pairs = 0;
    for (const hunt::Entry& e : board) {
        if (!hunt::IsSizePair(e.kind) || !holds(e.address, e.kind, start)) continue;
        if (++pairs > 4000) break;
        add_near(e.address, 64, kBySize, start, &found, kMaxCands);
    }
    std::vector<Cand> c;
    c.reserve(found.size());
    for (const auto& kv : found) if (kv.second.source & (kByCode | kByName)) c.push_back(kv.second);
    const size_t first_group = c.size();
    for (const auto& kv : found) if (!(kv.second.source & (kByCode | kByName))) c.push_back(kv.second);
    search_say("Clues gathered in %.1f s: %zu numbers hold the starting value %.1f%% (%zu read by the game's own render-scale code, %zu in objects "
               "that carry a render-scale name [%zu name texts, %zu pointers to names], %zu beside the 3D picture's size [%zu size places]).",
               double(GetTickCount64() - began) / 1000.0, c.size(), home * 100.0, by_code, strong - by_code, names.size(), pointers.size(),
               c.size() - first_group, (std::min<size_t>)(pairs, 4000));
    auto finish = [&](bool ok) {
        g.trial_ended_ms.store(GetTickCount64());
        if (!ok) g.trial_active.store(false);
        write_verdict();
        return ok;
    };
    if (c.empty()) {
        search_end(3, "No number in the game holds the starting value %.1f%% near a render-scale name or beside the 3D picture's size, so there "
                      "was nothing to nudge.", home * 100.0);
        return finish(false);
    }
    // The world must be steady by itself, or a nudge proves nothing.
    {
        hunt::Frame f; int seen = 0, off = 0;
        const uint64_t t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 1000) {
            Sleep(kTickMs);
            if (!live_frame(&f, 0)) continue;
            ++seen;
            if (std::fabs(double(f.world_w) / double(f.out_w) - home) > 0.012) ++off;
        }
        if (seen && off * 4 > seen) {
            search_end(3, "The game changes the size of its 3D picture on its own (it moved in %d of %d checks within a second with nothing "
                          "written): it is driving its own dynamic resolution, so a nudge cannot be told apart.", off, seen);
            return finish(false);
        }
    }
    unsigned steps = 0;
    long long at = hunt_in(c, 0, first_group, start, home, goal, &steps);
    if (at == -1) at = hunt_in(c, first_group, c.size(), start, home, goal, &steps);
    search_say("Nudged to %.0f%% in halving groups: %u nudges in %.1f s, every value put back after each.", goal * 100.0, steps,
               double(GetTickCount64() - began) / 1000.0);
    if (at < 0) {
        if (at == -2)
            search_end(4, "After a nudge the game's 3D picture did not return to its own size, so the Search stopped. Set Render Scale in the "
                          "game's menu to put it right.");
        else if (at == -3) search_end(3, "The game stayed in a menu or on a loading screen, so the nudges could not be judged.");
        else if (at == -4)
            search_end(3, "A group of numbers moved the 3D picture but no single one did when nudged alone: this game needs more than one "
                          "number changed together.");
        else
            search_end(3, "None of the %zu numbers moved the 3D picture when nudged to %.0f%%: this game does not read its render scale from a "
                          "number near its name or its picture size.", c.size(), goal * 100.0);
        return finish(false);
    }
    const Cand& hit = c[size_t(at)];
    search_end(2, "%s at %s is the game's render scale: nudged alone to %.0f%% it moved the 3D picture, three times out of three, and the "
                  "picture came back each time. Found by: %s%s%s. Whole Search: %.1f s, no slider moves.",
               hunt::KindName(hit.kind), where_text(hit.address).c_str(), goal * 100.0, (hit.source & kByCode) ? "[the game's render-scale code] " : "",
               (hit.source & kByName) ? "[an object carrying a render-scale name] " : "", (hit.source & kBySize) ? "[beside the 3D picture's size]" : "",
               double(GetTickCount64() - began) / 1000.0);
    finish(true);
    hunt::Entry e; e.address = hit.address; e.kind = hit.kind;
    run_trial(std::vector<hunt::Entry>{e}); // how it must be held, and the routes to it
    return true;
}
void trial_worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint32_t reported = 0;
    bool searched = false;
    for (;;) {
        Sleep(500);
        if (g.sweeping.load()) continue;
        // The Search: once the world has been steady since the first memory search, with no slider moves.
        if (g.search_wanted.load() && !searched && !g.scale_changes.load() && g.names_done.load()) {
            const uint64_t since = g.last_size_event_ms.load();
            bool have;
            { std::lock_guard<std::mutex> l(g.board_m); have = g.have_target; }
            hunt::Frame f;
            if (have && since && GetTickCount64() - since >= 6000 && live_frame(&f, 0)) {
                searched = true;
                if (run_search()) return;
                continue;
            }
        }
        // The writer watch goes on as soon as a scale number is confirmed, so a further change by the
        // player is caught too; what it has seen so far is reported as it comes.
        if (!watch_on() && !watch_done()) {
            std::vector<hunt::Entry> scales;
            {
                std::lock_guard<std::mutex> l(g.board_m);
                if (g.have_target)
                    for (const auto& e : g.board.Ranked(256)) if (e.Confirmed() && !hunt::IsSizePair(e.kind)) scales.push_back(e);
            }
            if (!scales.empty()) watch_start(scales);
        } else if (watch_count() != reported) { reported = watch_count(); watch_report(false); write_verdict(); }
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

namespace {
// Keeps the game's render scale where the Connection asked: writes the number whenever it does not hold
// the asked value (once is enough for a game that keeps it; a game that puts its own back is corrected
// within a few milliseconds). Asked 0: the game's own value is written back once.
void control_worker()
{
    uint32_t last = 0;
    for (;;) {
        Sleep(4);
        const uint32_t asked = g.control_asked.load(std::memory_order_relaxed);
        if (!asked && !last) { Sleep(30); continue; }
        const double want = asked ? double(asked) / 10000.0 : g.control_home;
        hunt::Target t; t.out_w = 10000; t.out_h = 10000; t.world_w = uint32_t(want * 10000.0 + 0.5); t.world_h = t.world_w;
        if (!holds(g.control_address, g.control_kind, t) || asked != last) {
            if (!write_scale(g.control_address, g.control_kind, want)) {
                line("control: the number at %s can no longer be written; the game's render scale is released", where_text(g.control_address).c_str());
                g.control_ready.store(false); g.control_asked.store(0);
                return;
            }
            if (asked != last) line("control: render scale asked %.1f%%%s", want * 100.0, asked ? "" : " (the game's own value, given back)");
        }
        last = asked;
    }
}
} // namespace
bool ControlReady() { return g.control_ready.load(std::memory_order_relaxed); }
bool RequestScale(double ratio)
{
    if (!ControlReady() || (ratio != 0.0 && !(ratio >= 0.25 && ratio <= 1.0))) return false;
    g.control_asked.store(ratio == 0.0 ? 0u : uint32_t(ratio * 10000.0 + 0.5));
    if (!g.control_thread.exchange(true)) {
        try { std::thread(control_worker).detach(); } catch (...) { g.control_thread.store(false); return false; }
    }
    return true;
}
void SetTrial(bool on) { g.trial_wanted.store(on); }
void SetSearch(bool on) { g.search_wanted.store(on); if (on) g.trial_wanted.store(true); }
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
