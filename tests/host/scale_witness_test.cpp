// Scale witness, code reading (log only): from one game function, find the numbers it loads that
// could be the render scale. Synthetic x64 code over a fake image.
#include "host_dx12/scale/scale_witness.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace na::scale::witness::analysis;
using K = na::scale::hunt::Kind;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
// Layout (RVAs): code 0x000-0x1ff (executable), constants 0x200-0x2ff (read-only),
// data 0x300-0x5ff (writable).
struct World {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x600, 0);
    std::vector<uint8_t> code;
    uint64_t base() const { return reinterpret_cast<uint64_t>(mem.data()); }
    template <class T> void put(uint32_t at, T v) { std::memcpy(&mem[at], &v, sizeof v); }
    void emit(std::initializer_list<uint8_t> b) { code.insert(code.end(), b); }
    // disp32 to `target`, relative to the end of the instruction (`tail` bytes follow the disp).
    void rip(uint32_t target, uint32_t tail = 0)
    {
        const int32_t d = int32_t(target) - int32_t(code.size() + 4 + tail);
        uint8_t b[4]; std::memcpy(b, &d, 4); code.insert(code.end(), b, b + 4);
    }
    Image image()
    {
        std::memcpy(mem.data(), code.data(), code.size());
        Image im;
        im.bytes = mem.data(); im.size = mem.size(); im.address = base();
        im.ranges = {{0x000, 0x200, true}, {0x200, 0x100, false}, {0x300, 0x300, false}};
        im.functions = {{0, uint32_t(code.size())}};
        return im;
    }
    Reader reader()
    {
        Reader r;
        r.read = [this](uint64_t a, void* out, size_t n) {
            if (a < base() || a + n > base() + mem.size()) return false;
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), n);
            return true;
        };
        r.writable = [this](uint64_t a) { return a >= base() + 0x300 && a < base() + 0x600; };
        return r;
    }
};
const Candidate* at_address(const std::vector<Candidate>& v, uint64_t a)
{
    for (const auto& c : v) if (c.address == a) return &c;
    return nullptr;
}
} // namespace

int main()
{
    {   // Plain SSE loads: a global, an object's field through a global pointer, a constant, a percent.
        World w;
        w.put<float>(0x300, 1.0f);
        w.put<uint64_t>(0x308, w.base() + 0x400);
        w.put<float>(0x410, 0.6667f);
        w.put<int32_t>(0x320, 67);
        w.put<float>(0x200, 1.0f); // constant
        w.emit({0xf3, 0x0f, 0x10, 0x05}); w.rip(0x300);       // movss xmm0,[rip+global]
        w.emit({0x48, 0x8b, 0x05}); w.rip(0x308);             // mov rax,[rip+pointer]
        w.emit({0xf3, 0x0f, 0x59, 0x48, 0x10});               // mulss xmm1,[rax+0x10]
        w.emit({0xf3, 0x0f, 0x10, 0x15}); w.rip(0x200);       // movss xmm2,[rip+constant]
        w.emit({0x0f, 0xaf, 0x05}); w.rip(0x320);             // imul eax,[rip+percent]
        w.emit({0xf3, 0x0f, 0x59, 0x59, 0x20});               // mulss xmm3,[rcx+0x20]  (rcx = an argument: unknown)
        w.emit({0xc3});
        const Image im = w.image();
        ScanStats st;
        const auto found = Scan(im, im.functions[0], w.reader(), &st);
        CHECK(st.loads == 5 && st.unresolved == 1 && st.constants == 1 && st.undecodable == 0);
        CHECK(found.size() == 3);
        const Candidate* a = at_address(found, w.base() + 0x300);
        const Candidate* b = at_address(found, w.base() + 0x410);
        const Candidate* c = at_address(found, w.base() + 0x320);
        CHECK(a && a->kind == K::ScaleF32 && !a->via_pointer && a->value == 1.0);
        CHECK(b && b->kind == K::ScaleF32 && b->via_pointer);
        CHECK(c && c->kind == K::PercentI32 && c->value == 67);
    }
    {   // AVX (VEX) code, a two-link pointer chain, and a pointer kept in rbx across a call.
        World w;
        w.put<uint64_t>(0x308, w.base() + 0x400);       // global -> renderer
        w.put<uint64_t>(0x458, w.base() + 0x500);       // renderer+0x58 -> settings
        w.put<float>(0x524, 0.5f);                      // settings+0x24 = scale
        w.put<double>(0x530, 0.5);                      // settings+0x30 = scale as a double
        w.emit({0x48, 0x8b, 0x05}); w.rip(0x308);       // mov rax,[rip+global]
        w.emit({0x48, 0x8b, 0x58, 0x58});               // mov rbx,[rax+0x58]
        w.emit({0xc5, 0xf8, 0x77});                     // vzeroupper
        w.emit({0xe8, 0x00, 0x00, 0x00, 0x00});         // call next (rbx survives, rax does not)
        w.emit({0xc5, 0xf2, 0x59, 0x43, 0x24});         // vmulss xmm0,xmm1,[rbx+0x24]
        w.emit({0xc5, 0xfb, 0x10, 0x4b, 0x30});         // vmovsd xmm1,[rbx+0x30]
        w.emit({0xc5, 0xfa, 0x10, 0x50, 0x10});         // vmovss xmm2,[rax+0x10]  (rax lost in the call)
        w.emit({0xc3});
        const Image im = w.image();
        ScanStats st;
        const auto found = Scan(im, im.functions[0], w.reader(), &st);
        CHECK(st.undecodable == 0 && st.loads == 3 && st.unresolved == 1);
        const Candidate* f = at_address(found, w.base() + 0x524);
        const Candidate* d = at_address(found, w.base() + 0x530);
        CHECK(f && f->kind == K::ScaleF32 && f->via_pointer && f->value == 0.5);
        CHECK(d && d->kind == K::ScaleF64 && d->via_pointer);
    }
    {   // A register changed by arithmetic after it was loaded is not trusted.
        World w;
        w.put<uint64_t>(0x308, w.base() + 0x400);
        w.put<float>(0x410, 0.75f);
        w.put<float>(0x420, 0.75f);
        w.emit({0x48, 0x8b, 0x05}); w.rip(0x308);       // mov rax,[rip+global]
        w.emit({0x48, 0x83, 0xc0, 0x10});               // add rax,0x10
        w.emit({0xf3, 0x0f, 0x10, 0x40, 0x10});         // movss xmm0,[rax+0x10]
        w.emit({0xc3});
        const Image im = w.image();
        ScanStats st;
        const auto found = Scan(im, im.functions[0], w.reader(), &st);
        CHECK(found.empty() && st.unresolved == 1);
    }
    {   // An instruction the reader does not know ends the function early but keeps what came before.
        World w;
        w.put<float>(0x300, 0.8f);
        w.emit({0xf3, 0x0f, 0x10, 0x05}); w.rip(0x300); // movss xmm0,[rip+global]
        w.emit({0x62, 0xf1, 0x7c, 0x48, 0x10, 0xc0});   // an AVX-512 instruction
        w.emit({0xc3});
        const Image im = w.image();
        ScanStats st;
        const auto found = Scan(im, im.functions[0], w.reader(), &st);
        CHECK(st.undecodable == 1 && found.size() == 1 && found[0].address == w.base() + 0x300);
    }
    {   // Named: the variable registered beside a setting's name.
        World w;
        w.put<float>(0x340, 1.0f);
        std::memcpy(&w.mem[0x210], "r.RenderScale", 14);
        w.emit({0x48, 0x8d, 0x0d}); w.rip(0x210);       // lea rcx,[rip+name]
        const uint32_t name_ref = uint32_t(w.code.size()) - 4;
        w.emit({0x48, 0x8d, 0x15}); w.rip(0x340);       // lea rdx,[rip+variable]
        w.emit({0xe8, 0x00, 0x00, 0x00, 0x00});         // call register
        w.emit({0xc3});
        const Image im = w.image();
        const auto code = DecodeFunction(im, im.functions[0]);
        const auto found = DataNear(im, 0, code, name_ref, 12, w.reader());
        CHECK(found.size() == 1 && found[0].address == w.base() + 0x340 && found[0].kind == K::ScaleF32);
    }
    std::printf(g_fail ? "scale_witness_test: %d failure(s)\n" : "scale_witness_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
