#pragma once
// Scale witness (Cort 2026-11-28: "make the Referee able to investigate and locate where to
// adjust the slider ... hijack the slider command"). LOG ONLY: nothing here writes game memory
// or calls game code.
//
// Step 1: when the game sets a viewport while the WORLD picture (the Referee's selected scene
//         colour or depth) is bound, record which game code asked (the call chain).
// Step 2: read that game code and list every number it loads from memory that could be the
//         render scale (a float/double near world/output, or a whole percent). A number counts
//         as a candidate only when it lives in writable memory (a setting, not a constant).
//         Later captures re-read every candidate; when the world size changes (the slider
//         moved), a candidate whose value followed the new size is the render scale.
//
// Universal: no game names, no settings files, no fixed addresses; the same on DX11 and DX12.
// Reuses the x64 code readers of scale/native_locator.hpp (Sniper's engine-specific locator).
#include "host_dx12/scale/native_locator.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace na::scale::witness {

// API hooks (inline on the game's thread; cheap; rate-limited inside).
// w/h: the viewport the game just set while the world picture is bound.
void OnWorldViewport(const char* api, float w, float h);
// The presented picture's size (swap chain creation / resize).
void NoteOutput(uint32_t w, uint32_t h);
// Starts the witness; its log is written to log_path. Safe to call once at boot.
void Start(const wchar_t* log_path);
bool Active();

namespace analysis {
namespace locator = na::scale::native::locator;
using locator::Image;
using locator::Function;
using locator::Ins;

enum class Type : uint8_t { F32, F64, I32 };
enum class Where : uint8_t { Rip, GlobalPointer, StaticObject };
struct Candidate {
    uint32_t at = 0;        // instruction RVA
    uint32_t function = 0;  // function start RVA
    uint64_t address = 0;   // absolute address of the number
    Type type = Type::F32;
    Where where = Where::Rip;
    double value = 0;
    bool writable = false;  // a setting can change; a constant cannot
    bool scale_like = false;
    bool matches = false;   // equals world/output (or its percent)
};
struct Reader {
    // Safe read of n bytes at an absolute address; false when unreadable.
    std::function<bool(uint64_t, void*, size_t)> read;
    // Is the page at this absolute address writable?
    std::function<bool(uint64_t)> writable;
};
struct ScanStats { uint32_t loads = 0, resolved = 0, unresolved = 0, constants = 0; };

inline bool ScaleLike(Type t, double v)
{
    if (!std::isfinite(v)) return false;
    if (t == Type::I32) return v >= 25 && v <= 200;
    return v >= 0.2 && v <= 2.5;
}
inline bool Matches(Type t, double v, double ratio)
{
    if (!(ratio > 0) || !std::isfinite(v)) return false;
    if (t == Type::I32) return std::fabs(v - ratio * 100.0) <= 1.0;
    return std::fabs(v - ratio) <= 0.01;
}
inline bool ReadValue(const Reader& r, uint64_t a, Type t, double* out)
{
    if (t == Type::F64) { double d = 0; if (!r.read(a, &d, 8)) return false; *out = d; return true; }
    if (t == Type::F32) { float f = 0; if (!r.read(a, &f, 4)) return false; *out = f; return true; }
    int32_t i = 0; if (!r.read(a, &i, 4)) return false; *out = i; return true;
}

// Which instructions read a number from memory that could feed a size calculation.
// movss/mulss/divss/cvtss2sd (F3 0F 10/59/5E/5A), movsd/mulsd/divsd (F2 0F 10/59/5E),
// cvtsi2ss/cvtsi2sd from memory (int), imul r32,m32 (int), movd xmm,m32 (float or int).
inline bool LoadKind(const Ins& i, Type* t, bool* also_int)
{
    *also_int = false;
    if (!(i.h.flags & F_MODRM) || i.h.modrm_mod == 3) return false;
    if (i.h.opcode != 0x0f) return false;
    const unsigned op = i.h.opcode2;
    if (i.h.p_rep == 0xf3) {
        if (op == 0x10 || op == 0x59 || op == 0x5e || op == 0x5a) { *t = Type::F32; return true; }
        if (op == 0x2a && !i.h.rex_w) { *t = Type::I32; return true; }
    } else if (i.h.p_rep == 0xf2) {
        if (op == 0x10 || op == 0x59 || op == 0x5e) { *t = Type::F64; return true; }
        if (op == 0x2a && !i.h.rex_w) { *t = Type::I32; return true; }
    } else if (i.h.p_66 && op == 0x6e && !i.h.rex_w) { *t = Type::F32; *also_int = true; return true; }
    else if (!i.h.p_66 && op == 0xaf && !i.h.rex_w) { *t = Type::I32; return true; }
    return false;
}
inline int32_t Disp(const Ins& i)
{
    if (i.h.flags & F_DISP8) return int8_t(i.h.disp.disp8);
    if (i.h.flags & F_DISP32) return int32_t(i.h.disp.disp32);
    return 0;
}
// Base register of a [reg+disp] operand, or -1 (RIP-relative, indexed, or none).
inline int BaseReg(const Ins& i)
{
    if (i.h.modrm_mod == 0 && i.h.modrm_rm == 5) return -1; // RIP-relative
    if (i.h.modrm_rm == 4) { // SIB: only a plain base, no index
        if (!(i.h.flags & F_SIB)) return -1;
        const unsigned index = i.h.sib_index + (i.h.rex_x ? 8u : 0u);
        if (index != 4) return -1;
        if (i.h.modrm_mod == 0 && i.h.sib_base == 5) return -1;
        return int(i.h.sib_base + (i.h.rex_b ? 8u : 0u));
    }
    return int(i.rm());
}
// The last write of a 64-bit register before code[k] in straight-line order.
// Returns: 1 = mov reg,[rip+X] (pointer held in a global), 2 = lea reg,[rip+X] (static object),
// 0 = anything else (unknown at rest).
inline int Origin(const std::vector<Ins>& code, size_t k, int reg, uint32_t* rva)
{
    for (size_t n = k; n-- > 0 && k - n <= 64;) {
        const Ins& i = code[n];
        if (i.h.opcode == 0xe8) return 0; // a call clobbers volatile registers
        if (!(i.h.flags & F_MODRM)) continue;
        const bool writes_reg = (i.h.opcode == 0x8b || i.h.opcode == 0x8d) && int(i.reg()) == reg;
        const bool writes_rm = i.h.modrm_mod == 3 && int(i.rm()) == reg && (i.h.opcode == 0x89 || i.h.opcode == 0x8b);
        if (writes_reg) {
            if (!i.h.rex_w || !i.rip) return 0;
            *rva = i.rip;
            return i.h.opcode == 0x8b ? 1 : 2;
        }
        if (writes_rm) return 0;
    }
    return 0;
}

// Step 2 for one function of the game (an RVA range of the image).
inline std::vector<Candidate> Scan(const Image& im, Function f, double ratio, const Reader& r, ScanStats* stats = nullptr)
{
    std::vector<Candidate> out;
    const auto code = locator::decode(im, f);
    for (size_t k = 0; k < code.size(); ++k) {
        const Ins& i = code[k];
        Type t; bool also_int = false;
        if (!LoadKind(i, &t, &also_int)) continue;
        if (stats) ++stats->loads;
        Candidate c; c.at = i.at; c.function = f.start; c.type = t;
        if (i.rip) { c.where = Where::Rip; c.address = im.address + i.rip; }
        else {
            const int base = BaseReg(i);
            uint32_t rva = 0;
            const int origin = base >= 0 ? Origin(code, k, base, &rva) : 0;
            if (origin == 1) {
                uint64_t pointer = 0;
                if (!r.read(im.address + rva, &pointer, 8) || !pointer) { if (stats) ++stats->unresolved; continue; }
                c.where = Where::GlobalPointer; c.address = pointer + uint64_t(int64_t(Disp(i)));
            } else if (origin == 2) {
                c.where = Where::StaticObject; c.address = im.address + rva + uint64_t(int64_t(Disp(i)));
            } else { if (stats) ++stats->unresolved; continue; }
        }
        if (stats) ++stats->resolved;
        for (int pass = 0; pass < (also_int ? 2 : 1); ++pass) {
            Candidate v = c;
            if (pass) v.type = Type::I32;
            if (!ReadValue(r, v.address, v.type, &v.value)) continue;
            v.writable = r.writable(v.address);
            v.scale_like = ScaleLike(v.type, v.value);
            v.matches = Matches(v.type, v.value, ratio);
            if (!v.scale_like) continue;
            if (!v.writable) { if (stats) ++stats->constants; continue; }
            out.push_back(v);
        }
    }
    return out;
}
} // namespace analysis
} // namespace na::scale::witness
