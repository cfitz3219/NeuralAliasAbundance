#pragma once
// Scale witness (Cort 2026-11-28: "make the Referee able to investigate and locate where to
// adjust the slider ... hijack the slider command"; "several different methods ... tries all
// methods at once, deciding which is best"). LOG ONLY: nothing here writes game memory or calls
// game code.
//
// Search methods, all running together on background threads and reporting to ONE board
// (core/scale_hunt.hpp), which ranks what they found:
//   call chain   the game code that sets up the world for drawing, and the numbers it loads
//   creation     the game code that creates the world's pictures (it runs again when the size
//                changes, so it sits closest to the scale calculation)
//   pointers     numbers reached by following the game's own pointers from that code
//   memory       the world's size and scale found by value anywhere in the game's memory
//                (works when the code cannot be read: protected or unusual games)
//   named        numbers beside a setting name such as "render scale" in the game's code
// A candidate that follows the world through a size change is confirmed; one that changes on
// its own, or misses a change, is dropped.
//
// Universal: no game names, no settings files, no fixed addresses; the same on DX11 and DX12.
#include "core/scale_hunt.hpp"
#include "host_dx12/scale/native_locator.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace na::scale::witness {

// API hooks (inline on the game's thread; cheap; rate-limited inside).
// The viewport the game set while the world picture (the Referee's scene colour/depth) is bound.
void OnWorldViewport(const char* api, float w, float h);
// The game created a picture it can draw into (render or depth target) of this size.
void OnTargetCreated(const char* api, uint32_t w, uint32_t h);
// The presented picture's size (swap chain creation / resize).
void NoteOutput(uint32_t w, uint32_t h);
// Starts the witness and its background threads; its log is written to log_path. Call once,
// outside DllMain.
// verdict_path (optional): a short PASS/FAIL file with the reason, rewritten as the run goes.
void Start(const wchar_t* log_path, const wchar_t* verdict_path = nullptr);
bool Active();
// Finder test ([scale] test=1): the witness runs with DLSS/DLAA held off, and writes the verdict file.
void SetTestMode(bool on);
bool TestMode();
// Writing trial ([scale] trial=1, with the finder test; call before Start): once a scale number is confirmed
// and the world is steady, NeuralAlias writes each confirmed number itself, a ResponseJudge watches whether
// the game's world follows and how it must be held, the game's value is put back, and routes from the
// game's program to the working number are searched. Results go to the verdict file ("HIJACK: ...").
void SetTrial(bool on);
// The Search ([scale] search=1, with the finder test; call before Start): at launch, with no slider moves,
// NeuralAlias gathers the numbers that hold the starting value in objects carrying a render-scale name or
// beside the 3D picture's size, nudges them by five points in halving groups, and keeps the one the game
// obeys. The writing trial then runs on it. Results: "SEARCH: ..." and "NEXT STAGE: ..." in the verdict file.
void SetSearch(bool on);
// The handle for The Connection. ControlReady: the Search (or the slider test) found the one number the game
// obeys and the writing trial passed. RequestScale(ratio in 0.25..1.0) keeps the game's 3D picture at that
// share of the screen from then on; RequestScale(0) gives the game its own value back. False: not ready, or
// a ratio outside the range. Nothing is written until the first request.
bool ControlReady();
bool RequestScale(double ratio);

namespace analysis {
namespace locator = na::scale::native::locator;
using locator::Image;
using locator::Function;
using locator::Ins;
namespace hunt = na::scale::hunt;

struct Candidate {
    uint32_t at = 0;        // instruction RVA
    uint32_t function = 0;  // function start RVA
    uint64_t address = 0;   // absolute address of the number
    hunt::Kind kind = hunt::Kind::ScaleF32;
    double value = 0;
    bool via_pointer = false; // reached through the game's pointers (an object's field)
};
struct Reader {
    // Safe read of n bytes at an absolute address; false when unreadable.
    std::function<bool(uint64_t, void*, size_t)> read;
    // Is the memory at this absolute address writable (a setting, not a constant)?
    std::function<bool(uint64_t)> writable;
};
struct ScanStats { uint32_t loads = 0, resolved = 0, unresolved = 0, constants = 0, undecodable = 0; };

// ---- decoding: hde64 plus the VEX (AVX) forms hde64 rejects ----
// Fills the same fields the readers use. VEX instructions outside the 0F map are length-decoded
// only (opcode 0xC4, so no reader mistakes them for something else).
inline bool DecodeVex(const uint8_t* b, hde64s* h)
{
    *h = hde64s{};
    size_t p;
    unsigned map = 1, pp;
    if (b[0] == 0xc5) { h->rex_r = uint8_t(!(b[1] & 0x80)); pp = b[1] & 3u; p = 2; }
    else {
        h->rex_r = uint8_t(!(b[1] & 0x80)); h->rex_x = uint8_t(!(b[1] & 0x40)); h->rex_b = uint8_t(!(b[1] & 0x20));
        map = b[1] & 0x1fu; h->rex_w = uint8_t((b[2] >> 7) & 1u); pp = b[2] & 3u; p = 3;
    }
    if (map < 1 || map > 3) return false;
    const uint8_t op = b[p++];
    h->opcode = uint8_t(map == 1 ? 0x0f : 0xc4);
    h->opcode2 = op;
    h->p_rep = uint8_t(pp == 2 ? 0xf3 : pp == 3 ? 0xf2 : 0);
    h->p_66 = uint8_t(pp == 1 ? 0x66 : 0);
    if (map == 1 && op == 0x77) { h->len = uint8_t(p); return true; } // vzeroupper / vzeroall
    h->flags |= F_MODRM;
    h->modrm = b[p++];
    h->modrm_mod = uint8_t(h->modrm >> 6); h->modrm_reg = uint8_t((h->modrm >> 3) & 7); h->modrm_rm = uint8_t(h->modrm & 7);
    if (h->modrm_mod != 3) {
        bool disp32 = h->modrm_mod == 2 || (h->modrm_mod == 0 && h->modrm_rm == 5);
        if (h->modrm_rm == 4) {
            h->flags |= F_SIB;
            h->sib = b[p++];
            h->sib_scale = uint8_t(h->sib >> 6); h->sib_index = uint8_t((h->sib >> 3) & 7); h->sib_base = uint8_t(h->sib & 7);
            if (h->modrm_mod == 0 && h->sib_base == 5) disp32 = true;
        }
        if (h->modrm_mod == 1) { h->flags |= F_DISP8; h->disp.disp8 = b[p++]; }
        else if (disp32) { h->flags |= F_DISP32; std::memcpy(&h->disp.disp32, b + p, 4); p += 4; }
    }
    const bool imm8 = map == 3 || (map == 1 && (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xc2 || op == 0xc4 ||
                                                op == 0xc5 || op == 0xc6));
    if (imm8) { h->flags |= F_IMM8; ++p; }
    if (p > 15) return false;
    h->len = uint8_t(p);
    return true;
}
// One function, in address order. Stops at the first undecodable instruction and keeps what it
// has (locator::decode discards the whole function); *complete says whether it reached the end.
inline std::vector<Ins> DecodeFunction(const Image& im, Function f, bool* complete = nullptr)
{
    std::vector<Ins> out;
    if (complete) *complete = false;
    if (!f.size || !im.contains(f.start, f.size, true)) return out;
    for (uint32_t p = f.start; p < f.start + f.size;) {
        uint8_t bytes[32]{};
        std::memcpy(bytes, im.bytes + p, (std::min<size_t>)(15, f.start + f.size - p));
        Ins i; i.at = p;
        bool ok;
        if (bytes[0] == 0xc5 || bytes[0] == 0xc4) ok = DecodeVex(bytes, &i.h);
        else { hde64_disasm(bytes, &i.h); ok = i.h.len && !(i.h.flags & F_ERROR); }
        if (!ok || i.h.len > f.start + f.size - p) return out;
        if ((i.h.flags & F_MODRM) && i.h.modrm_mod == 0 && i.h.modrm_rm == 5 && !i.h.p_67) {
            const int64_t r = int64_t(p) + i.h.len + int32_t(i.h.disp.disp32);
            if (r >= 0 && uint64_t(r) < im.size) i.rip = uint32_t(r);
        }
        if (i.h.opcode == 0xe8) {
            const int64_t r = int64_t(p) + i.h.len + int32_t(i.h.imm.imm32);
            if (r >= 0 && uint64_t(r) < im.size) i.call = uint32_t(r);
        }
        out.push_back(i);
        p += i.h.len;
    }
    if (complete) *complete = true;
    return out;
}

// Which instructions read a number from memory that could feed a size calculation, legacy or
// VEX encoded: movss/mulss/divss/cvtss2sd, movsd/mulsd/divsd, cvtsi2ss/sd from memory,
// imul r32,m32, movd xmm,m32.
enum class Load : uint8_t { None, F32, F64, I32, F32OrI32 };
inline Load LoadKind(const Ins& i)
{
    if (!(i.h.flags & F_MODRM) || i.h.modrm_mod == 3 || i.h.opcode != 0x0f) return Load::None;
    const unsigned op = i.h.opcode2;
    if (i.h.p_rep == 0xf3) {
        if (op == 0x10 || op == 0x59 || op == 0x5e || op == 0x5a) return Load::F32;
        if (op == 0x2a && !i.h.rex_w) return Load::I32;
    } else if (i.h.p_rep == 0xf2) {
        if (op == 0x10 || op == 0x59 || op == 0x5e) return Load::F64;
        if (op == 0x2a && !i.h.rex_w) return Load::I32;
    } else if (i.h.p_66 && op == 0x6e && !i.h.rex_w) return Load::F32OrI32;
    else if (!i.h.p_66 && op == 0xaf && !i.h.rex_w) return Load::I32;
    return Load::None;
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
// May this instruction change the general register `reg`? Errs towards yes.
inline bool Writes(const Ins& i, int reg)
{
    const unsigned op = i.h.opcode;
    const auto low = [&](unsigned base) { return int(op - base + (i.h.rex_b ? 8u : 0u)); };
    if (!(i.h.flags & F_MODRM)) {
        if (op >= 0xb8 && op <= 0xbf) return low(0xb8) == reg;
        if (op >= 0x58 && op <= 0x5f) return low(0x58) == reg;
        if (op >= 0x90 && op <= 0x97) return reg == 0 || low(0x90) == reg;
        if (op < 0x40 && (op & 7) == 5 && op != 0x3d) return reg == 0; // op eax, imm
        if (op == 0x99 || op == 0x98) return reg == 0 || reg == 2;     // cdq / cdqe
        return false;
    }
    const int r = int(i.reg()), m = i.h.modrm_mod == 3 ? int(i.rm()) : -1;
    if (op == 0xc4) return r == reg || m == reg; // VEX outside the 0F map (BMI and the like)
    if (op == 0x0f) {
        const unsigned o = i.h.opcode2;
        const bool reg_dest = (o >= 0x40 && o <= 0x4f) || o == 0xaf || o == 0xb6 || o == 0xb7 || o == 0xbe || o == 0xbf || o == 0xbc ||
                              o == 0xbd || o == 0xb8 || o == 0x2c || o == 0x2d || o == 0x50 || o == 0xd7 || o == 0xc5;
        if (reg_dest) return r == reg;
        if (o == 0x7e || (o >= 0x90 && o <= 0x9f) || o == 0xc0 || o == 0xc1) return m == reg;
        if (o == 0xb0 || o == 0xb1) return m == reg || reg == 0;
        return false;
    }
    if (op < 0x38 && (op & 6) == 2) return r == reg;  // add/or/adc/sbb/and/sub/xor reg, r/m
    if (op < 0x38 && (op & 6) == 0) return m == reg;  // ... r/m, reg
    switch (op) {
    case 0x8a: case 0x8b: case 0x8d: case 0x63: case 0x69: case 0x6b: return r == reg;
    case 0x86: case 0x87: return r == reg || m == reg;
    case 0x88: case 0x89: case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xc6: case 0xc7: case 0x8f: return m == reg;
    case 0x80: case 0x81: case 0x83: return m == reg && i.h.modrm_reg != 7;
    case 0xf6: case 0xf7:
        if (i.h.modrm_reg == 2 || i.h.modrm_reg == 3) return m == reg;
        return i.h.modrm_reg >= 4 && (reg == 0 || reg == 2);
    case 0xfe: case 0xff: return i.h.modrm_reg <= 1 && m == reg;
    default: return false;
    }
}
// The value a 64-bit register holds just before code[k], when it can be worked out by walking
// straight back: loaded from a global, the address of static data, a field of an object reached
// the same way (up to 4 links), or a copy of such a register. Reads the game's memory as it is
// now. *via_pointer: the value came out of memory (an object), not a fixed address.
inline bool ResolveReg(const Image& im, const std::vector<Ins>& code, size_t k, int reg, const Reader& r, int depth, uint64_t* out,
                       bool* via_pointer)
{
    if (depth > 4) return false;
    size_t steps = 0;
    for (size_t n = k; n-- > 0 && steps < 96; ++steps) {
        const Ins& i = code[n];
        const unsigned op = i.h.opcode;
        const bool modrm = (i.h.flags & F_MODRM) != 0;
        // Straight-line code only: past a return or a jump the bytes before are another path.
        if (op == 0xc3 || op == 0xc2 || op == 0xcc || op == 0xe9 || op == 0xeb) return false;
        if (op == 0xe8 || (op == 0xff && modrm && (i.h.modrm_reg == 2 || i.h.modrm_reg == 3))) {
            // A call keeps rbx, rbp, rsi, rdi, r12-r15 (Windows x64); the rest are lost.
            if (reg == 0 || reg == 1 || reg == 2 || (reg >= 8 && reg <= 11)) return false;
            continue;
        }
        if (modrm && (op == 0x8b || op == 0x8d) && int(i.reg()) == reg) {
            if (!i.h.rex_w) return false;
            if (i.h.modrm_mod == 3) return op == 0x8b && ResolveReg(im, code, n, int(i.rm()), r, depth + 1, out, via_pointer);
            uint64_t address = 0;
            if (i.rip) address = im.address + i.rip;
            else {
                const int base = BaseReg(i);
                uint64_t b = 0;
                if (base < 0 || !ResolveReg(im, code, n, base, r, depth + 1, &b, via_pointer)) return false;
                address = b + uint64_t(int64_t(Disp(i)));
            }
            if (op == 0x8d) { *out = address; return true; }
            uint64_t v = 0;
            if (!r.read(address, &v, 8) || v < 0x10000) return false;
            *via_pointer = true; *out = v;
            return true;
        }
        if (modrm && op == 0x89 && i.h.modrm_mod == 3 && int(i.rm()) == reg)
            return i.h.rex_w && ResolveReg(im, code, n, int(i.reg()), r, depth + 1, out, via_pointer);
        if (Writes(i, reg)) return false;
    }
    return false;
}
// Could this value be a render scale of this kind? (Wide on purpose: 20% to 250%.)
inline bool ScaleLike(hunt::Kind k, double v)
{
    if (!std::isfinite(v)) return false;
    if (k == hunt::Kind::PercentI32 || k == hunt::Kind::PercentF32) return v >= 20 && v <= 250;
    return v >= 0.2 && v <= 2.5;
}
inline bool ReadKind(const Reader& r, uint64_t a, hunt::Kind k, double* out)
{
    if (k == hunt::Kind::ScaleF64) { double d = 0; if (!r.read(a, &d, 8)) return false; *out = d; return true; }
    if (k == hunt::Kind::PercentI32) { int32_t i = 0; if (!r.read(a, &i, 4)) return false; *out = i; return true; }
    float f = 0; if (!r.read(a, &f, 4)) return false; *out = double(f); return true;
}
// Adds the number at `address` when it is a writable, scale-like value of one of `kinds`.
inline void Consider(const Reader& r, uint64_t address, std::initializer_list<hunt::Kind> kinds, const Candidate& proto, ScanStats* stats,
                     std::vector<Candidate>& out)
{
    bool writable_known = false, writable = false;
    for (hunt::Kind k : kinds) {
        Candidate c = proto;
        c.address = address; c.kind = k;
        if (!ReadKind(r, address, k, &c.value) || !ScaleLike(k, c.value)) continue;
        if (!writable_known) { writable = r.writable(address); writable_known = true; }
        if (!writable) { if (stats) ++stats->constants; return; }
        out.push_back(c);
    }
}
// The numbers one decoded function loads that could be the render scale.
inline std::vector<Candidate> ScanCode(const Image& im, uint32_t function, const std::vector<Ins>& code, const Reader& r, ScanStats* stats = nullptr)
{
    std::vector<Candidate> out;
    for (size_t k = 0; k < code.size(); ++k) {
        const Ins& i = code[k];
        const Load load = LoadKind(i);
        if (load == Load::None) continue;
        if (stats) ++stats->loads;
        Candidate c; c.at = i.at; c.function = function;
        uint64_t address = 0;
        if (i.rip) address = im.address + i.rip;
        else {
            const int base = BaseReg(i);
            uint64_t b = 0;
            if (base < 0 || !ResolveReg(im, code, k, base, r, 0, &b, &c.via_pointer)) { if (stats) ++stats->unresolved; continue; }
            address = b + uint64_t(int64_t(Disp(i)));
        }
        if (stats) ++stats->resolved;
        using K = hunt::Kind;
        if (load == Load::F64) Consider(r, address, {K::ScaleF64}, c, stats, out);
        else if (load == Load::I32) Consider(r, address, {K::PercentI32}, c, stats, out);
        else if (load == Load::F32) Consider(r, address, {K::ScaleF32, K::PercentF32}, c, stats, out);
        else Consider(r, address, {K::ScaleF32, K::PercentF32, K::PercentI32}, c, stats, out);
    }
    return out;
}
inline std::vector<Candidate> Scan(const Image& im, Function f, const Reader& r, ScanStats* stats = nullptr)
{
    bool complete = false;
    const auto code = DecodeFunction(im, f, &complete);
    if (!complete && stats) ++stats->undecodable;
    return ScanCode(im, f.start, code, r, stats);
}
// Named method: addresses of writable data taken (lea reg,[rip+X]) within `reach` instructions
// of the instruction at RVA `at` (a reference to a setting name): the variable registered
// under that name, in engines that register settings as name + address.
inline std::vector<Candidate> DataNear(const Image& im, uint32_t function, const std::vector<Ins>& code, uint32_t at, size_t reach,
                                       const Reader& r, ScanStats* stats = nullptr)
{
    std::vector<Candidate> out;
    size_t k = code.size();
    for (size_t n = 0; n < code.size(); ++n)
        if (code[n].at <= at && at < code[n].at + code[n].h.len) { k = n; break; }
    if (k == code.size()) return out;
    const size_t first = k > reach ? k - reach : 0, last = (std::min)(code.size(), k + reach + 1);
    for (size_t n = first; n < last; ++n) {
        const Ins& i = code[n];
        if (n == k || i.h.opcode != 0x8d || !i.rip) continue;
        Candidate c; c.at = i.at; c.function = function;
        using K = hunt::Kind;
        Consider(r, im.address + i.rip, {K::ScaleF32, K::PercentF32, K::PercentI32}, c, stats, out);
    }
    return out;
}
} // namespace analysis
} // namespace na::scale::witness
