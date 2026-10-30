#include "host_dx12/adapters/unreal/screen_percentage.hpp"

#include "host_dx12/ngx_observe/ngx_observe.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

namespace na::unreal {
namespace {

struct Section { const uint8_t* base = nullptr; size_t size = 0; bool exec = false, write = false; };
struct Image { const uint8_t* base = nullptr; size_t size = 0; std::vector<Section> sections; };

bool load_image(Image& img)
{
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    img.base = base;
    img.size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        Section s;
        s.base = base + sh[i].VirtualAddress;
        s.size = sh[i].Misc.VirtualSize;
        s.exec = (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        s.write = (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        img.sections.push_back(s);
    }
    return true;
}

const Section* section_of(const Image& img, const void* p)
{
    auto* b = static_cast<const uint8_t*>(p);
    for (const auto& s : img.sections)
        if (b >= s.base && b < s.base + s.size) return &s;
    return nullptr;
}

bool region_ok(const void* p, size_t n, bool need_write)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD wr = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
    if (!(mbi.Protect & (need_write ? wr : rd))) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

std::vector<const uint8_t*> find_wide(const Image& img, const wchar_t* text)
{
    std::vector<const uint8_t*> hits;
    const size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    for (const auto& s : img.sections) {
        if (s.exec || s.size < n) continue;
        for (size_t i = 0; i + n <= s.size; i += 2)
            if (s.base[i] == static_cast<uint8_t>(text[0]) && memcmp(s.base + i, text, n) == 0) {
                if (i >= 2 && (s.base[i - 2] || s.base[i - 1])) continue; // must start a string
                hits.push_back(s.base + i);
                if (hits.size() >= 4) return hits;
            }
    }
    return hits;
}

struct Lea { const uint8_t* at; const uint8_t* target; unsigned reg; };
bool decode_lea(const uint8_t* p, Lea& out)
{
    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return false;
    int32_t disp;
    memcpy(&disp, p + 3, 4);
    out.at = p;
    out.target = p + 7 + disp;
    out.reg = ((p[0] & 4u) << 1) | ((p[2] >> 3) & 7u);
    return true;
}

bool sane_percent(float v) { return std::isfinite(v) && v >= 1.f && v <= 400.f; }

// Returns the validated TConsoleVariableData<float>* (ShadowedValue[0..1]) for
// the cvar whose static initializer references `name`, or null. `count` gets
// the number of distinct validated candidates (only 1 is accepted).
float* locate_float_cvar(const wchar_t* name, uint32_t* count, char* why, size_t why_cap)
{
    *count = 0;
    Image img;
    if (!load_image(img)) { strncpy_s(why, why_cap, "No 64-bit executable image.", _TRUNCATE); return nullptr; }
    const std::vector<const uint8_t*> strings = find_wide(img, name);
    if (strings.empty()) { strncpy_s(why, why_cap, "r.ScreenPercentage is not in this executable (not an Unreal game).", _TRUNCATE); return nullptr; }
    std::vector<Lea> xrefs;
    for (const auto& s : img.sections) {
        if (!s.exec || s.size < 8) continue;
        for (size_t i = 0; i + 7 <= s.size; ++i) {
            Lea l;
            if (!decode_lea(s.base + i, l)) continue;
            for (const uint8_t* str : strings)
                if (l.target == str && xrefs.size() < 16) xrefs.push_back(l);
        }
    }
    float* found = nullptr;
    for (const Lea& x : xrefs) {
        // The static initializer loads the object's own address into RCX right
        // beside the name (Cort's PBZ probe: +7). Earlier/later objects belong to
        // neighbouring cvars (-63, +87 there), so the window stays narrow.
        const Lea* best = nullptr;
        Lea cand[8];
        uint32_t nc = 0;
        for (const uint8_t* p = x.at - 32; p <= x.at + 48 && nc < 8; ++p) {
            if (p < img.base || p + 7 > img.base + img.size || p == x.at) continue;
            Lea c;
            if (!decode_lea(p, c) || c.reg != 1) continue; // RCX = this
            const Section* cs = section_of(img, c.target);
            if (!cs || !cs->write) continue;
            cand[nc++] = c;
        }
        for (uint32_t i = 0; i < nc; ++i) {
            const uint8_t* obj = cand[i].target;
            if (!region_ok(obj, 24, false)) continue;
            uint64_t q[3];
            memcpy(q, obj, sizeof(q));
            if (!section_of(img, reinterpret_cast<const void*>(q[0]))) continue; // vtable in the image
            auto* target = reinterpret_cast<const uint8_t*>(q[1]);
            if (!region_ok(target, 8, false)) continue;
            uint64_t tv = 0;
            memcpy(&tv, target, 8);
            if (!section_of(img, reinterpret_cast<const void*>(tv))) continue;   // IConsoleVariable has an image vtable
            auto* ref = reinterpret_cast<float*>(q[2]);
            if ((q[2] & 3u) != 0 || !region_ok(ref, 8, true)) continue;
            float v[2];
            memcpy(v, ref, sizeof(v));
            if (!sane_percent(v[0]) || !sane_percent(v[1])) continue;
            if (!best || std::abs(cand[i].at - x.at) < std::abs(best->at - x.at)) best = &cand[i];
        }
        if (!best) continue;
        float* ref = nullptr;
        {
            uint64_t q2 = 0;
            memcpy(&q2, best->target + 16, 8);
            ref = reinterpret_cast<float*>(q2);
        }
        if (found && found != ref) {
            *count = 2;
            strncpy_s(why, why_cap, "r.ScreenPercentage matched more than one storage location; not changing anything.", _TRUNCATE);
            return nullptr;
        }
        found = ref;
        *count = 1;
    }
    if (!found) strncpy_s(why, why_cap, "r.ScreenPercentage's storage could not be validated.", _TRUNCATE);
    return found;
}

std::mutex g_m;
SpStatus g_status;
std::atomic<float*> g_ref{nullptr};
std::atomic<bool> g_started{false};
HANDLE g_wake = nullptr;
struct Pending { bool set = false; float percent = 0.f; uint32_t out_w = 0, out_h = 0; uint64_t id = 0; } g_pending;
uint64_t g_next_id = 0;

void set_state(SpState s, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_m);
    g_status.state = s;
    if (reason) strncpy_s(g_status.reason, reason, _TRUNCATE);
}

float read_value(float* ref)
{
    float v = 0.f;
    __try {
        v = *reinterpret_cast<volatile float*>(ref);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0.f;
    }
    return v;
}

bool write_value(float* ref, float v)
{
    LONG bits;
    memcpy(&bits, &v, 4);
    __try {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(ref), bits);     // ShadowedValue[0], game thread
        InterlockedExchange(reinterpret_cast<volatile LONG*>(ref + 1), bits); // ShadowedValue[1], render thread
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool locate()
{
    char why[160] = "";
    uint32_t n = 0;
    float* ref = locate_float_cvar(L"r.ScreenPercentage", &n, why, sizeof(why));
    std::lock_guard<std::mutex> lock(g_m);
    g_status.candidates = n;
    if (!ref) {
        g_status.state = SpState::NotFound;
        g_status.available = false;
        strncpy_s(g_status.reason, why, _TRUNCATE);
        return false;
    }
    g_ref.store(ref, std::memory_order_release);
    g_status.state = SpState::Ready;
    g_status.available = true;
    strncpy_s(g_status.reason, "Unreal render resolution control is ready.", _TRUNCATE);
    return true;
}

void run_request(const Pending& req)
{
    float* ref = g_ref.load(std::memory_order_acquire);
    const na::ngx::Stats before = na::ngx::GetStats();
    const float now = read_value(ref);
    // Safety: the located value must describe the size the game feeds DLSS now.
    if (before.last_sr_in_w != 0xFFFFFFFFu && before.last_sr_out_w && before.last_sr_out_w != 0xFFFFFFFFu) {
        const uint32_t expect_now = ExpectedExtent(before.last_sr_out_w, now);
        if (expect_now + 2 < before.last_sr_in_w || before.last_sr_in_w + 2 < expect_now) {
            char why[200];
            snprintf(why, sizeof(why), "The located setting (%.1f%%) does not match the game's render size (%ux%u); nothing was changed.",
                     now, before.last_sr_in_w, before.last_sr_in_h);
            set_state(SpState::Refused, why);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_status.requested = req.percent;
        g_status.request_id = req.id;
        g_status.expect_w = ExpectedExtent(req.out_w, req.percent);
        g_status.expect_h = ExpectedExtent(req.out_h, req.percent);
    }
    if (!write_value(ref, req.percent)) {
        set_state(SpState::Refused, "Writing the engine's screen percentage failed; nothing was changed.");
        return;
    }
    set_state(SpState::Applying, "Waiting for the game to rebuild DLSS at the new size...");
    uint32_t ew = 0, eh = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        ew = g_status.expect_w;
        eh = g_status.expect_h;
    }
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 8000) {
        Sleep(100);
        const float cur = read_value(ref);
        const na::ngx::Stats s = na::ngx::GetStats();
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_status.current = cur;
            g_status.seen_w = s.last_sr_in_w;
            g_status.seen_h = s.last_sr_in_h;
        }
        if (std::fabs(cur - req.percent) > 0.01f) {
            set_state(SpState::GameChanged, "The game changed its resolution setting itself; its choice is kept.");
            return;
        }
        const bool w_ok = s.last_sr_in_w + 1 >= ew && s.last_sr_in_w <= ew + 1;
        const bool h_ok = s.last_sr_in_h + 1 >= eh && s.last_sr_in_h <= eh + 1;
        if (s.sr_creates > before.sr_creates || s.creates > before.creates) {
            if (w_ok && h_ok) {
                char why[160];
                snprintf(why, sizeof(why), "Applied: the game now renders %ux%u (%.1f%%).", s.last_sr_in_w, s.last_sr_in_h, req.percent);
                set_state(SpState::Confirmed, why);
                return;
            }
        } else if (w_ok && h_ok && before.last_sr_in_w == s.last_sr_in_w && before.last_sr_in_h == s.last_sr_in_h) {
            set_state(SpState::Confirmed, "This mode is already the game's render size.");
            return;
        }
    }
    char why[200];
    const na::ngx::Stats s = na::ngx::GetStats();
    snprintf(why, sizeof(why), "Set %.1f%%, but the game's DLSS still renders %ux%u (expected %ux%u). The game may be using dynamic resolution or re-applying its own setting.",
             req.percent, s.last_sr_in_w, s.last_sr_in_h, ew, eh);
    set_state(SpState::NotApplied, why);
}

DWORD WINAPI worker(LPVOID)
{
    Sleep(5000); // after engine start: static cvars registered and the game's settings applied
    if (!locate()) return 0;
    for (;;) {
        WaitForSingleObject(g_wake, 1000);
        Pending req;
        {
            std::lock_guard<std::mutex> lock(g_m);
            req = g_pending;
            g_pending.set = false;
            if (float* ref = g_ref.load(std::memory_order_acquire)) g_status.current = read_value(ref);
        }
        if (req.set) run_request(req);
    }
}

} // namespace

uint32_t ExpectedExtent(uint32_t out, float percent)
{
    return static_cast<uint32_t>(std::ceil(static_cast<double>(out) * static_cast<double>(percent) / 100.0 - 1e-6));
}

void StartScreenPercentage()
{
    if (g_started.exchange(true)) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)) CloseHandle(t);
}

bool RequestScreenPercentage(float percent, uint32_t out_w, uint32_t out_h)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_status.available || !sane_percent(percent) || !out_w || !out_h || out_w == 0xFFFFFFFFu || out_h == 0xFFFFFFFFu) {
        g_status.state = g_status.available ? SpState::Refused : g_status.state;
        if (g_status.available) strncpy_s(g_status.reason, "The request was not valid for this game's output size.", _TRUNCATE);
        return false;
    }
    if (g_status.state == SpState::Applying || g_pending.set) {
        strncpy_s(g_status.reason, "A previous change is still being applied. Try again in a moment.", _TRUNCATE);
        return false;
    }
    g_pending = {true, percent, out_w, out_h, ++g_next_id};
    g_status.state = SpState::Applying;
    strncpy_s(g_status.reason, "Applying...", _TRUNCATE);
    if (g_wake) SetEvent(g_wake);
    return true;
}

SpStatus GetScreenPercentageStatus()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_status;
}

} // namespace na::unreal
