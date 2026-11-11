// DX9 vertex-shader jitter patch (DX9 plan step D5, docs/DX9-PLAN.md).
//
// Many DX9 games upload one combined World x View x Projection per object, so
// there is no single camera matrix to jitter. Instead every vertex shader is
// rewritten at CreateVertexShader: its writes to the position output go to a
// spare temp register, and at the end
//     mad oPos.xy, rT.wwww, cJ, rT     (oPos.xy = rT.xy + jitter * rT.w)
//     mov oPos.zw, rT
// is added, cJ being a constant register the shader never reads. With cJ =
// (2 jx / width, -2 jy / height, 0, 0) the picture moves by exactly (jx, jy)
// render pixels, the same convention as neural_alias::jitter_projection; with
// cJ = 0 the shader is unchanged in effect. Works for vs_1_1 .. vs_3_0
// (oPos, or the o# register declared as position in vs_3_0).
//
// The token format is Microsoft's documented D3D9 shader bytecode. Portable
// C++17 (Linux-tested).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace na::dx9 {

struct VsPatch {
    bool ok = false;
    const char* why = "";
    std::vector<uint32_t> code; // the patched shader (when ok)
    uint32_t major = 0, minor = 0;
    uint32_t temp = 0;          // the spare temp used
    uint32_t position_writes = 0;
    bool relative_constants = false; // the shader indexes constants with a0/aL (cJ assumed out of its range)
};

namespace vsp {

constexpr uint32_t kEnd = 0x0000FFFF;
constexpr uint32_t kOpMov = 0x01, kOpMad = 0x04, kOpRet = 0x1C, kOpLabel = 0x1E, kOpDcl = 0x1F, kOpDef = 0x51, kOpDefi = 0x30, kOpDefb = 0x2F,
                   kOpComment = 0xFFFE;
constexpr uint32_t kTemp = 0, kInput = 1, kConst = 2, kRastOut = 4, kOutput = 6, kConst2 = 11, kConst3 = 12, kConst4 = 13;

inline uint32_t RegType(uint32_t t) { return ((t >> 28) & 7u) | ((t >> 8) & 0x18u); }
inline uint32_t RegNum(uint32_t t) { return t & 0x7FFu; }
inline bool Relative(uint32_t t) { return (t >> 13) & 1u; }
inline uint32_t WithReg(uint32_t t, uint32_t type, uint32_t num)
{
    t &= ~((7u << 28) | (3u << 11) | 0x7FFu);
    return t | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (num & 0x7FFu);
}
inline uint32_t Dst(uint32_t type, uint32_t num, uint32_t mask) { return WithReg(0x80000000u | (mask << 16), type, num); }
inline uint32_t Src(uint32_t type, uint32_t num, uint32_t swizzle) { return WithReg(0x80000000u | (swizzle << 16), type, num); }
constexpr uint32_t kXyzw = 0xE4, kWwww = 0xFF;

// Opcodes with no destination parameter (flow control).
inline bool NoDst(uint32_t op)
{
    switch (op) {
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: // call callnz loop ret endloop label
    case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: // rep endrep if ifc else endif break breakc
    case 0x60: case 0x00: return true; // breakp, nop
    default: return false;
    }
}

} // namespace vsp

// `fn` is the game's CreateVertexShader function (ends with 0x0000FFFF).
// `jitter_const`: the constant register to read the jitter from; `max_consts`
// and `max_temps`: what the device allows (0 = the shader model's minimum).
inline VsPatch PatchVertexShader(const uint32_t* fn, size_t max_tokens, uint32_t jitter_const, uint32_t max_temps = 0)
{
    using namespace vsp;
    VsPatch r;
    if (!fn || max_tokens < 2) { r.why = "empty shader"; return r; }
    const uint32_t ver = fn[0];
    if ((ver & 0xFFFF0000u) != 0xFFFE0000u) { r.why = "not a vertex shader"; return r; }
    r.major = (ver >> 8) & 0xFF;
    r.minor = ver & 0xFF;
    if (r.major < 1 || r.major > 3) { r.why = "unknown vertex shader model"; return r; }
    const bool sm2 = r.major >= 2;
    if (!max_temps) max_temps = r.major >= 3 ? 32 : 12;

    // ---- pass 1: find instructions, the position output, temps and constants used.
    struct Ins { size_t at, len; uint32_t op; };
    std::vector<Ins> ins;
    uint32_t pos_type = r.major >= 3 ? kOutput : kRastOut, pos_num = r.major >= 3 ? 0xFFFFFFFFu : 0;
    int max_temp = -1;
    bool reads_jc = false;
    size_t i = 1, end_at = 0;
    while (true) {
        if (i >= max_tokens) { r.why = "no end token"; return r; }
        const uint32_t t = fn[i];
        if (t == kEnd) { end_at = i; break; }
        const uint32_t op = t & 0xFFFF;
        size_t len = 0;
        if (op == kOpComment) {
            len = (t >> 16) & 0x7FFF;
        } else if (sm2) {
            len = (t >> 24) & 0xF;
        } else if (op == kOpDef) {
            len = 5;
        } else {
            while (i + 1 + len < max_tokens && (fn[i + 1 + len] & 0x80000000u)) ++len;
        }
        if (i + 1 + len > max_tokens) { r.why = "truncated shader"; return r; }
        ins.push_back({i, len, op});
        if (op != kOpComment && len) {
            const uint32_t* p = fn + i + 1;
            if (op == kOpDef || op == kOpDefi || op == kOpDefb) {
                if ((RegType(p[0]) == kConst) && RegNum(p[0]) == jitter_const) reads_jc = true;
            } else if (op == kOpDcl) {
                if (len >= 2 && r.major >= 3 && RegType(p[1]) == kOutput && (p[0] & 0x1F) == 0 && ((p[0] >> 16) & 0xF) == 0)
                    pos_num = RegNum(p[1]);
                if (len >= 2 && RegType(p[1]) == kTemp) max_temp = std::max<int>(max_temp, static_cast<int>(RegNum(p[1])));
            } else {
                for (size_t k = 0; k < len; ++k) {
                    const uint32_t pt = p[k];
                    const uint32_t ty = RegType(pt), n = RegNum(pt);
                    if (ty == kTemp) max_temp = std::max<int>(max_temp, static_cast<int>(n));
                    const bool is_dst = k == 0 && !NoDst(op);
                    if (!is_dst && (ty == kConst || ty == kConst2 || ty == kConst3 || ty == kConst4)) {
                        // m4x4/m4x3/m3x4/m3x3/m3x2 read 4/3/4/3/2 registers from their matrix operand.
                        const uint32_t rows = k == 2 ? (op == 0x14 || op == 0x16 ? 4u : op == 0x15 || op == 0x17 ? 3u : op == 0x18 ? 2u : 1u) : 1u;
                        if (Relative(pt)) r.relative_constants = true;
                        else if (ty == kConst && jitter_const >= n && jitter_const < n + rows) reads_jc = true;
                    }
                    // SM2+: a relative-addressing token follows its parameter.
                    if (sm2 && Relative(pt) && k + 1 < len) ++k;
                }
            }
        }
        i += 1 + len;
    }
    if (reads_jc) { r.why = "the shader uses the jitter constant register"; return r; }
    if (r.major >= 3 && pos_num == 0xFFFFFFFFu) { r.why = "vs_3_0 without a position output"; return r; }
    const uint32_t temp = static_cast<uint32_t>(max_temp + 1);
    if (temp >= max_temps) { r.why = "no spare temp register"; return r; }
    r.temp = temp;

    // ---- pass 2: copy, redirecting position writes to the temp; append the jitter before main's end.
    size_t insert_at = end_at; // before END, or before main's ret when there are subroutines
    {
        size_t first_label = 0;
        for (const Ins& n : ins)
            if (n.op == kOpLabel) { first_label = n.at; break; }
        if (first_label)
            for (const Ins& n : ins) {
                if (n.at >= first_label) break;
                if (n.op == kOpRet) insert_at = n.at; // the last top-level ret before the subroutines
            }
    }
    std::vector<uint32_t> out(fn, fn + end_at + 1);
    for (const Ins& n : ins) {
        if (n.op == kOpComment || n.op == kOpDcl || n.op == kOpDef || n.op == kOpDefi || n.op == kOpDefb || NoDst(n.op) || !n.len) continue;
        uint32_t& d = out[n.at + 1];
        if (RegType(d) == pos_type && RegNum(d) == pos_num && !Relative(d)) {
            d = WithReg(d, kTemp, temp);
            ++r.position_writes;
        }
    }
    if (!r.position_writes) { r.why = "the shader never writes its position"; return r; }
    const uint32_t mad = kOpMad | (sm2 ? (4u << 24) : 0u), mov = kOpMov | (sm2 ? (2u << 24) : 0u);
    const uint32_t tail[] = {mad, Dst(pos_type, pos_num, 0x3), Src(kTemp, temp, kWwww), Src(kConst, jitter_const, kXyzw), Src(kTemp, temp, kXyzw),
                             mov, Dst(pos_type, pos_num, 0xC), Src(kTemp, temp, kXyzw)};
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), std::begin(tail), std::end(tail));
    r.code = std::move(out);
    r.ok = true;
    return r;
}

} // namespace na::dx9
