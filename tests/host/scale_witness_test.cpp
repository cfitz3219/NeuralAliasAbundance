// Scale witness, step 2 (log only): from one game function, find the numbers it loads that could
// be the render scale. Synthetic x64 code over a fake image: a writable global float, a field of an
// object reached through a global pointer, a read-only constant (must be skipped) and a percent int.
#include "host_dx12/scale/scale_witness.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace na::scale::witness::analysis;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    // Layout (RVAs): code 0x000-0x0ff (executable), constants 0x100-0x1ff (read-only),
    // data 0x200-0x3ff (writable): global float at 0x200, object pointer at 0x208,
    // object at 0x300 with its scale at +0x10, percent int at 0x220.
    std::vector<uint8_t> mem(0x400, 0);
    const uint64_t base = reinterpret_cast<uint64_t>(mem.data());
    auto put32 = [&](uint32_t at, uint32_t v) { std::memcpy(&mem[at], &v, 4); };
    auto putf = [&](uint32_t at, float v) { std::memcpy(&mem[at], &v, 4); };
    putf(0x200, 1.0f);
    const uint64_t object = base + 0x300;
    std::memcpy(&mem[0x208], &object, 8);
    putf(0x310, 0.6667f);
    put32(0x220, 67);
    putf(0x100, 1.0f); // constant

    std::vector<uint8_t> code;
    auto rip = [&](uint32_t target) { // disp32 relative to the end of a 4-byte displacement ending the instruction
        const uint32_t end = uint32_t(code.size()) + 4;
        const int32_t d = int32_t(target) - int32_t(end);
        uint8_t b[4]; std::memcpy(b, &d, 4); code.insert(code.end(), b, b + 4);
    };
    // movss xmm0, [rip+0x200]
    code.insert(code.end(), {0xf3, 0x0f, 0x10, 0x05}); rip(0x200);
    // mov rax, [rip+0x208]
    code.insert(code.end(), {0x48, 0x8b, 0x05}); rip(0x208);
    // mulss xmm1, [rax+0x10]
    code.insert(code.end(), {0xf3, 0x0f, 0x59, 0x48, 0x10});
    // movss xmm2, [rip+0x100] (read-only constant)
    code.insert(code.end(), {0xf3, 0x0f, 0x10, 0x15}); rip(0x100);
    // imul eax, [rip+0x220]
    code.insert(code.end(), {0x0f, 0xaf, 0x05}); rip(0x220);
    // mulss xmm3, [rcx+0x20] (rcx unknown: unresolved)
    code.insert(code.end(), {0xf3, 0x0f, 0x59, 0x59, 0x20});
    code.push_back(0xc3);
    std::memcpy(mem.data(), code.data(), code.size());

    Image im;
    im.bytes = mem.data(); im.size = mem.size(); im.address = base;
    im.ranges = {{0x000, 0x100, true}, {0x100, 0x100, false}, {0x200, 0x200, false}};
    im.functions = {{0, uint32_t(code.size())}};

    Reader r;
    r.read = [&](uint64_t a, void* out, size_t n) {
        if (a < base || a + n > base + mem.size()) return false;
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), n);
        return true;
    };
    r.writable = [&](uint64_t a) { return a >= base + 0x200 && a < base + 0x400; };

    ScanStats st;
    const auto found = Scan(im, im.functions[0], 2293.0 / 3440.0, r, &st);
    CHECK(st.loads == 5);
    CHECK(st.unresolved == 1);
    CHECK(st.constants == 1);
    CHECK(found.size() == 3);
    bool global = false, field = false, percent = false;
    for (const auto& c : found) {
        if (c.address == base + 0x200) { global = c.where == Where::Rip && c.type == Type::F32 && c.value == 1.0 && !c.matches; }
        if (c.address == base + 0x310) { field = c.where == Where::GlobalPointer && c.matches; }
        if (c.address == base + 0x220) { percent = c.type == Type::I32 && c.value == 67 && c.matches; }
    }
    CHECK(global);
    CHECK(field);
    CHECK(percent);

    // The slider moved: the object's scale is now 0.5 and the world is half size; the tracked
    // number follows the new size.
    putf(0x310, 0.5f);
    double v = 0;
    CHECK(ReadValue(r, base + 0x310, Type::F32, &v) && Matches(Type::F32, v, 0.5));
    CHECK(!Matches(Type::F32, 1.0, 0.5));

    std::printf(g_fail ? "scale_witness_test: %d failure(s)\n" : "scale_witness_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
