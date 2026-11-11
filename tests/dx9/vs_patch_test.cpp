// D5: the vertex-shader jitter patch on hand-built vs_1_1 / vs_2_0 / vs_3_0
// shaders, checked by running original and patched code in a tiny interpreter:
// the patched position must be the original plus jitter * w, nothing else.
#include "host_dx9/vs_patch.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace na::dx9;
using namespace na::dx9::vsp;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

struct V4 { float v[4]; };

// Runs mov/add/mul/mad/dp4/m4x4/call/ret/label/dcl/def (enough for these tests).
struct Vm {
    V4 r[32]{}, in[16]{}, c[256]{}, rast[3]{}, attr[2]{}, out[12]{};
    bool sm2 = true;
    V4* Reg(uint32_t t)
    {
        const uint32_t n = RegNum(t);
        switch (RegType(t)) {
        case kTemp: return &r[n];
        case kInput: return &in[n];
        case kConst: return &c[n];
        case kRastOut: return &rast[n];
        case kOutput: return &out[n];
        case 5: return &attr[n]; // oD#
        default: return nullptr;
        }
    }
    V4 Read(uint32_t t)
    {
        const V4 s = *Reg(t);
        V4 o;
        const uint32_t sw = (t >> 16) & 0xFF;
        for (int k = 0; k < 4; ++k) o.v[k] = s.v[(sw >> (2 * k)) & 3];
        return o;
    }
    void Write(uint32_t t, const V4& v)
    {
        V4* d = Reg(t);
        const uint32_t m = (t >> 16) & 0xF;
        for (int k = 0; k < 4; ++k)
            if (m & (1u << k)) d->v[k] = v.v[k];
    }
    bool Run(const std::vector<uint32_t>& code)
    {
        sm2 = ((code[0] >> 8) & 0xFF) >= 2;
        // Labels first.
        std::vector<size_t> label_at(16, 0);
        std::vector<std::pair<size_t, size_t>> list; // (at, len)
        for (size_t i = 1; code[i] != kEnd;) {
            const uint32_t t = code[i], op = t & 0xFFFF;
            size_t len = op == kOpComment ? (t >> 16) & 0x7FFF : sm2 ? (t >> 24) & 0xF : op == kOpDef ? 5 : 0;
            if (!sm2 && op != kOpComment && op != kOpDef)
                while (code[i + 1 + len] & 0x80000000u) ++len;
            if (op == kOpLabel) label_at[RegNum(code[i + 1])] = list.size();
            list.push_back({i, len});
            i += 1 + len;
        }
        std::vector<size_t> stack;
        for (size_t pc = 0; pc < list.size();) {
            const uint32_t* p = &code[list[pc].first];
            const uint32_t op = p[0] & 0xFFFF;
            ++pc;
            V4 a, b, d;
            switch (op) {
            case kOpComment: case kOpDcl: case 0: break;
            case kOpMov: Write(p[1], Read(p[2])); break;
            case 0x02: a = Read(p[2]); b = Read(p[3]); for (int k = 0; k < 4; ++k) d.v[k] = a.v[k] + b.v[k]; Write(p[1], d); break;
            case 0x05: a = Read(p[2]); b = Read(p[3]); for (int k = 0; k < 4; ++k) d.v[k] = a.v[k] * b.v[k]; Write(p[1], d); break;
            case kOpMad: {
                a = Read(p[2]); b = Read(p[3]); const V4 e = Read(p[4]);
                for (int k = 0; k < 4; ++k) d.v[k] = a.v[k] * b.v[k] + e.v[k];
                Write(p[1], d);
                break;
            }
            case 0x09: { a = Read(p[2]); b = Read(p[3]); float s = 0; for (int k = 0; k < 4; ++k) s += a.v[k] * b.v[k]; for (float& x : d.v) x = s; Write(p[1], d); break; }
            case 0x14: { // m4x4 dst, v, c#: dst.k = dp4(v, c[n + k])
                a = Read(p[2]);
                for (int k = 0; k < 4; ++k) {
                    const V4& row = c[RegNum(p[3]) + k];
                    d.v[k] = 0;
                    for (int j = 0; j < 4; ++j) d.v[k] += a.v[j] * row.v[j];
                }
                Write(p[1], d);
                break;
            }
            case 0x19: stack.push_back(pc); pc = label_at[RegNum(p[1])] + 1; break; // call
            case kOpRet: if (stack.empty()) return true; pc = stack.back(); stack.pop_back(); break;
            case kOpLabel: return true; // fell into a subroutine: main ended
            case kOpDef: std::memcpy(c[RegNum(p[1])].v, p + 2, 16); break;
            default: std::printf("     vm: opcode 0x%x not handled\n", op); return false;
            }
        }
        return true;
    }
};

uint32_t Ins(uint32_t op, uint32_t len, bool sm2) { return op | (sm2 ? (len << 24) : 0u); }

// Runs both, checks pos' = pos + (jx, jy) * w and that nothing else changed.
void Compare(const std::vector<uint32_t>& orig, const char* name, uint32_t pos_type, uint32_t pos_num)
{
    const uint32_t jc = 95;
    const VsPatch p = PatchVertexShader(orig.data(), orig.size(), jc);
    char what[160];
    std::snprintf(what, sizeof(what), "%s: patched (%s)", name, p.ok ? "ok" : p.why);
    Check(p.ok, what);
    if (!p.ok) return;
    Vm a, b;
    for (Vm* vm : {&a, &b}) {
        vm->in[0] = {{0.3f, -0.7f, 2.0f, 1.0f}};
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 4; ++j) vm->c[k].v[j] = static_cast<float>((k * 4 + j) % 7) * 0.25f + (k == j ? 1.f : 0.f);
        vm->c[3] = {{0.f, 0.f, 1.f, 0.f}}; // w = z (perspective)
        vm->c[4] = {{0.2f, 0.4f, 0.6f, 1.f}};
    }
    b.c[jc] = {{0.01f, -0.02f, 0.f, 0.f}};
    std::snprintf(what, sizeof(what), "%s: both run", name);
    Check(a.Run(orig) && b.Run(p.code), what);
    const V4 pa = pos_type == kRastOut ? a.rast[pos_num] : a.out[pos_num];
    const V4 pb = pos_type == kRastOut ? b.rast[pos_num] : b.out[pos_num];
    const bool moved = std::fabs(pb.v[0] - (pa.v[0] + 0.01f * pa.v[3])) < 1e-5f && std::fabs(pb.v[1] - (pa.v[1] - 0.02f * pa.v[3])) < 1e-5f &&
                       pb.v[2] == pa.v[2] && pb.v[3] == pa.v[3] && pa.v[3] != 0.f;
    std::snprintf(what, sizeof(what), "%s: position = original + jitter * w (w %.3f), z/w unchanged", name, pa.v[3]);
    Check(moved, what);
    bool same = true;
    for (int k = 0; k < 12; ++k)
        for (int j = 0; j < 4; ++j)
            if (!(pos_type == kOutput && static_cast<uint32_t>(k) == pos_num) && a.out[k].v[j] != b.out[k].v[j]) same = false;
    for (int k = 1; k < 3; ++k)
        for (int j = 0; j < 4; ++j)
            if (a.rast[k].v[j] != b.rast[k].v[j]) same = false;
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 4; ++j)
            if (a.attr[k].v[j] != b.attr[k].v[j]) same = false;
    std::snprintf(what, sizeof(what), "%s: other outputs unchanged", name);
    Check(same, what);
    // Jitter 0 = the original exactly.
    Vm z;
    z = a;
    z.rast[0] = {}; z.out[pos_num] = {};
    z.c[jc] = {};
    z.Run(p.code);
    const V4 pz = pos_type == kRastOut ? z.rast[pos_num] : z.out[pos_num];
    std::snprintf(what, sizeof(what), "%s: jitter 0 = original position", name);
    Check(pz.v[0] == pa.v[0] && pz.v[1] == pa.v[1] && pz.v[2] == pa.v[2] && pz.v[3] == pa.v[3], what);
}

} // namespace

int main()
{
    // vs_1_1: dcl_position v0; m4x4 oPos, v0, c0; mov oD0, c4
    {
        const bool s = false;
        std::vector<uint32_t> vs = {0xFFFE0101, Ins(kOpDcl, 0, s), 0x80000000u, Dst(kInput, 0, 0xF), Ins(0x14, 0, s), Dst(kRastOut, 0, 0xF),
                                    Src(kInput, 0, kXyzw), Src(kConst, 0, kXyzw), Ins(kOpMov, 0, s), Dst(5, 0, 0xF), Src(kConst, 4, kXyzw), kEnd};
        Compare(vs, "vs_1_1 m4x4 oPos", kRastOut, 0);
    }
    // vs_2_0 with temps: dp4 r0.x..w then mov oPos, r0 (split writes) + a fog write; uses r0, r1
    {
        const bool s = true;
        std::vector<uint32_t> vs = {0xFFFE0200, Ins(kOpDcl, 2, s), 0x80000000u, Dst(kInput, 0, 0xF)};
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t d[] = {Ins(0x09, 3, s), Dst(kTemp, 1, 1u << k), Src(kInput, 0, kXyzw), Src(kConst, k, kXyzw)};
            vs.insert(vs.end(), std::begin(d), std::end(d));
        }
        const uint32_t tail[] = {Ins(kOpMov, 2, s), Dst(kRastOut, 0, 0x3), Src(kTemp, 1, kXyzw), Ins(kOpMov, 2, s), Dst(kRastOut, 0, 0xC),
                                 Src(kTemp, 1, kXyzw), Ins(kOpMov, 2, s), Dst(kRastOut, 1, 0x1), Src(kTemp, 1, 0xAA), kEnd};
        vs.insert(vs.end(), std::begin(tail), std::end(tail));
        Compare(vs, "vs_2_0 split oPos writes", kRastOut, 0);
        const VsPatch p = PatchVertexShader(vs.data(), vs.size(), 95);
        Check(p.temp == 2 && p.position_writes == 2, "vs_2_0: spare temp r2, both position writes redirected");
    }
    // vs_3_0 with a subroutine: dcl_position o3; call l0; ret; label l0; m4x4 o3, v0, c0; mov o0, c4; ret
    {
        const bool s = true;
        std::vector<uint32_t> vs = {0xFFFE0300, Ins(kOpDcl, 2, s), 0x80000000u, Dst(kInput, 0, 0xF), Ins(kOpDcl, 2, s), 0x80000000u,
                                    Dst(kOutput, 3, 0xF), Ins(kOpDcl, 2, s), 0x8000000Au, Dst(kOutput, 0, 0xF),
                                    Ins(0x19, 1, s), Src(18, 0, kXyzw), // call l0 (label register type 18)
                                    Ins(kOpRet, 0, s), Ins(kOpLabel, 1, s), Src(18, 0, kXyzw), Ins(0x14, 3, s), Dst(kOutput, 3, 0xF),
                                    Src(kInput, 0, kXyzw), Src(kConst, 0, kXyzw), Ins(kOpMov, 2, s), Dst(kOutput, 0, 0xF), Src(kConst, 4, kXyzw),
                                    Ins(kOpRet, 0, s), kEnd};
        Compare(vs, "vs_3_0 o3 written in a subroutine", kOutput, 3);
    }
    // Refusals.
    {
        const bool s = true;
        std::vector<uint32_t> vs = {0xFFFE0200, Ins(0x14, 3, s), Dst(kRastOut, 0, 0xF), Src(kInput, 0, kXyzw), Src(kConst, 92, kXyzw), kEnd};
        Check(!PatchVertexShader(vs.data(), vs.size(), 95).ok, "a shader whose m4x4 reads c92..c95 (the jitter register) is left alone");
    }
    {
        const bool s = true;
        std::vector<uint32_t> vs = {0xFFFE0200, Ins(kOpMov, 2, s), Dst(kRastOut, 0, 0xF), Src(kConst, 95, kXyzw), kEnd};
        Check(!PatchVertexShader(vs.data(), vs.size(), 95).ok, "a shader reading c95 directly is left alone");
        const uint32_t ps[] = {0xFFFF0200, kEnd};
        Check(!PatchVertexShader(ps, 2, 95).ok, "a pixel shader is refused");
    }
    std::printf("%s: vertex-shader jitter patch\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
