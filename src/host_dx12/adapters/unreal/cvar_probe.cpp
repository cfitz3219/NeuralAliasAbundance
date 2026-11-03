#include "host_dx12/adapters/unreal/cvar_probe.hpp"
#include "host_dx12/settings.hpp"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace na::unreal {
namespace {
struct Section { const uint8_t* base = nullptr; size_t size = 0; bool exec = false, write = false; char name[9] = {}; };
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
    auto* sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        Section s;
        s.base = base + sh[i].VirtualAddress;
        s.size = sh[i].Misc.VirtualSize;
        s.exec = (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        s.write = (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        memcpy(s.name, sh[i].Name, 8);
        img.sections.push_back(s);
    }
    return true;
}
const Section* section_of(const Image& img, const void* p)
{
    auto* b = static_cast<const uint8_t*>(p);
    for (const auto& s : img.sections) if (b >= s.base && b < s.base + s.size) return &s;
    return nullptr;
}
bool readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & rd)) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}
// Exact UTF-16 occurrences (with the terminating zero, so "r.ScreenPercentage"
// does not match "r.ScreenPercentage.Mode") in non-executable sections.
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
// RIP-relative LEA r64, [rip+disp32]: REX.W(+R) 8D modrm(mod=00,rm=101) disp32.
bool decode_lea(const uint8_t* p, Lea& out)
{
    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return false;
    int32_t disp; memcpy(&disp, p + 3, 4);
    out.at = p; out.target = p + 7 + disp; out.reg = ((p[0] & 4) << 1) | ((p[2] >> 3) & 7);
    return true;
}
} // namespace

namespace {
const wchar_t* const kNames[] = {
    L"r.ScreenPercentage", L"r.ScreenPercentage.Mode", L"r.SecondaryScreenPercentage.GameViewport",
    L"r.NGX.DLSS.Enable", L"r.Streamline.DLSSG.Enable", L"r.DynamicRes.OperationMode",
};
void probe(FILE* f)
{
    Image img;
    if (!load_image(img)) { fprintf(f, "no PE image\n"); return; }
    fprintf(f, "image base=%p size=0x%zx sections=%zu\n", img.base, img.size, img.sections.size());
    for (const auto& s : img.sections)
        fprintf(f, "  section %-8s %p size=0x%zx exec=%u write=%u\n", s.name, s.base, s.size, s.exec ? 1u : 0u, s.write ? 1u : 0u);
    struct Want { const wchar_t* name; std::vector<const uint8_t*> strings; std::vector<Lea> xrefs; };
    std::vector<Want> wants;
    for (const wchar_t* n : kNames) { Want w{n, find_wide(img, n), {}}; wants.push_back(w); }
    bool any = false;
    for (auto& w : wants) {
        fprintf(f, "string %ls: %zu hit(s)", w.name, w.strings.size());
        for (auto* p : w.strings) fprintf(f, " %p", p);
        fprintf(f, "\n");
        any |= !w.strings.empty();
    }
    if (!any) { fprintf(f, "no Unreal cvar names found; not an Unreal executable or names are not UTF-16\n"); return; }
    // One pass over executable sections: every RIP-relative LEA whose target is a wanted string.
    for (const auto& s : img.sections) {
        if (!s.exec || s.size < 8) continue;
        for (size_t i = 0; i + 7 <= s.size; ++i) {
            Lea l;
            if (!decode_lea(s.base + i, l)) continue;
            for (auto& w : wants) for (auto* str : w.strings)
                if (l.target == str && w.xrefs.size() < 8) w.xrefs.push_back(l);
        }
    }
    for (auto& w : wants) {
        for (const auto& x : w.xrefs) {
            fprintf(f, "xref %ls at %p (rva 0x%llx) reg=%u\n", w.name, x.at,
                    static_cast<unsigned long long>(x.at - img.base), x.reg);
            // A static TAutoConsoleVariable initializer loads the object's own
            // address (this, usually RCX) next to the name string. Candidates:
            // nearby RIP-relative LEAs that point into a writable image section.
            const uint8_t* lo = x.at >= img.base + 96 ? x.at - 96 : img.base;
            const uint8_t* hi = x.at + 96 + 7 <= img.base + img.size ? x.at + 96 : img.base + img.size - 7;
            for (const uint8_t* p = lo; p < hi; ++p) {
                Lea c;
                if (p == x.at || !decode_lea(p, c)) continue;
                const Section* cs = section_of(img, c.target);
                if (!cs || !cs->write) continue;
                fprintf(f, "  candidate object %p (rva 0x%llx, %s) via reg=%u at %+d\n", c.target,
                        static_cast<unsigned long long>(c.target - img.base), cs->name, c.reg, static_cast<int>(p - x.at));
                if (!readable(c.target, 32)) { fprintf(f, "    unreadable\n"); continue; }
                uint64_t q[4]; memcpy(q, c.target, sizeof(q));
                fprintf(f, "    qwords %016llx %016llx %016llx %016llx\n", q[0], q[1], q[2], q[3]);
                // FAutoConsoleObject: [0] vtable, [8] IConsoleVariable* Target;
                // TAutoConsoleVariable<T>: [16] TConsoleVariableData<T>* Ref
                // (ShadowedValue[0] game thread, [1] render thread).
                for (int k = 1; k <= 3; ++k) {
                    auto* ptr = reinterpret_cast<const uint8_t*>(q[k]);
                    if (!readable(ptr, 16)) continue;
                    float fl[4]; int32_t in[4]; memcpy(fl, ptr, 16); memcpy(in, ptr, 16);
                    uint64_t v0 = 0; memcpy(&v0, ptr, 8);
                    const Section* vs = section_of(img, reinterpret_cast<const void*>(v0));
                    fprintf(f, "    [%d] -> %p floats %g %g %g %g ints %d %d %d %d first-qword-in=%s\n", k * 8, ptr,
                            fl[0], fl[1], fl[2], fl[3], in[0], in[1], in[2], in[3], vs ? vs->name : "-");
                }
            }
        }
    }
}
DWORD WINAPI probe_thread(void*)
{
    Sleep(20000); // after engine start: static cvars registered, game settings applied
    wchar_t path[MAX_PATH] = {};
    wcsncpy_s(path, na::settings::UserIniPath(), _TRUNCATE);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"unreal_probe.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return 0;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "NeuralAlias Unreal cvar probe (read-only) %02u:%02u:%02u\n", t.wHour, t.wMinute, t.wSecond);
    __try { probe(f); } __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(f, "probe stopped: exception 0x%08lx\n", GetExceptionCode()); }
    fclose(f);
    return 0;
}
} // namespace

void StartCvarProbe()
{
    HANDLE h = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
} // namespace na::unreal
