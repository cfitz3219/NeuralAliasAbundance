// What a compiled pixel shader reads, found in the program itself (core/dxbc_inputs.hpp).
// The instruction words below are the ones the shader compiler writes.
#include "test_support.hpp"
#include "core/dxbc_inputs.hpp"
#include <vector>
using na::dxbc::PixelInputs;
using na::dxbc::ReadPixelInputs;
namespace {
struct Chunk { const char* name; std::vector<uint32_t> words; };
std::vector<uint8_t> Container(const std::vector<Chunk>& chunks)
{
    std::vector<uint8_t> out(32 + chunks.size() * 4);
    std::memcpy(out.data(), "DXBC", 4);
    const auto put = [&](size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    put(20, 1);
    put(28, uint32_t(chunks.size()));
    for (size_t i = 0; i < chunks.size(); ++i) {
        put(32 + i * 4, uint32_t(out.size()));
        const size_t at = out.size();
        out.resize(at + 8 + chunks[i].words.size() * 4);
        std::memcpy(out.data() + at, chunks[i].name, 4);
        put(at + 4, uint32_t(chunks[i].words.size() * 4));
        if (!chunks[i].words.empty()) std::memcpy(out.data() + at + 8, chunks[i].words.data(), chunks[i].words.size() * 4);
    }
    put(24, uint32_t(out.size()));
    return out;
}
// One input: the pixel position, of which `read` components are read.
Chunk Inputs(uint32_t read, bool newer = false)
{
    const uint32_t masks = 0x0f | (read << 8);
    if (newer) return {"ISG1", {1, 8, /*stream*/ 0, 40, 0, /*system value*/ 1, 3, 0, masks, /*precision*/ 0}};
    return {"ISGN", {1, 8, 32, 0, /*system value*/ 1, 3, 0, masks}};
}
// ps_5_0: flags, the given texture declarations, the position input, one output, a data block, return.
Chunk Program(const std::vector<uint32_t>& texture_slots, const char* name = "SHEX")
{
    std::vector<uint32_t> w = {0x00000050, 0, 0x0100086a};
    for (uint32_t slot : texture_slots) { w.push_back(0x04001858); w.push_back(0x00107000); w.push_back(slot); w.push_back(0x00005555); }
    for (uint32_t v : {0x04002064u, 0x00101032u, 0u, 1u}) w.push_back(v); // dcl_input_ps_siv v0.xy, position
    for (uint32_t v : {0x03000065u, 0x001020f2u, 0u}) w.push_back(v);     // dcl_output o0
    // A data block whose words would read as texture declarations if its own length were not honoured.
    for (uint32_t v : {0x00000035u, 6u, 0x04001858u, 0x00107000u, 99u, 0x00005555u}) w.push_back(v);
    w.push_back(0x0100003e); // ret
    w[1] = uint32_t(w.size());
    return {name, w};
}
bool Slot(const PixelInputs& in, unsigned n) { return (in.slots[n >> 6] >> (n & 63)) & 1; }
} // namespace
int main() { return test::run({
    {"the declared texture slots are found, and only those", [] {
        const auto code = Container({Inputs(3), Program({0, 5, 70})});
        PixelInputs in;
        CHECK(ReadPixelInputs(code.data(), code.size(), in));
        CHECK(Slot(in, 0) && Slot(in, 5) && Slot(in, 70));
        CHECK(!Slot(in, 1) && !Slot(in, 99)); // 99 only appears inside the data block
        CHECK(in.position);
    }},
    {"a shader that does not read its pixel position is told apart", [] {
        const auto code = Container({Inputs(0), Program({2})});
        PixelInputs in;
        CHECK(ReadPixelInputs(code.data(), code.size(), in));
        CHECK(Slot(in, 2) && !in.position);
        // Depth of the position alone (z) is not a place on the screen.
        const auto z = Container({Inputs(4), Program({})});
        CHECK(ReadPixelInputs(z.data(), z.size(), in) && !in.position && !in.slots[0] && !in.slots[1]);
    }},
    {"the newer input table and the older program chunk are read too", [] {
        const auto code = Container({Inputs(1, true), Program({7}, "SHDR")});
        PixelInputs in;
        CHECK(ReadPixelInputs(code.data(), code.size(), in));
        CHECK(Slot(in, 7) && in.position);
    }},
    {"an extended declaration still names its slot", [] {
        // The same declaration with one extension word after the instruction word.
        Chunk p = Program({});
        const std::vector<uint32_t> extended = {0x85001858u, 0x00000001u, 0x00107000u, 11u, 0x00005555u};
        p.words.insert(p.words.begin() + 3, extended.begin(), extended.end());
        p.words[1] = uint32_t(p.words.size());
        const auto code = Container({Inputs(3), p});
        PixelInputs in;
        CHECK(ReadPixelInputs(code.data(), code.size(), in) && Slot(in, 11));
    }},
    {"anything that is not a whole compiled shader is refused, never read past its end", [] {
        PixelInputs in;
        CHECK(!ReadPixelInputs(nullptr, 0, in));
        const uint8_t junk[64] = {'D', 'X', 'B', 'X'};
        CHECK(!ReadPixelInputs(junk, sizeof junk, in));
        auto code = Container({Inputs(3), Program({0, 5})});
        // No program chunk: nothing is known.
        const auto only_inputs = Container({Inputs(3)});
        CHECK(!ReadPixelInputs(only_inputs.data(), only_inputs.size(), in));
        // Cut short at every length: no crash, and nothing invented.
        for (size_t n = 0; n < code.size(); ++n) {
            std::vector<uint8_t> cut(code.begin(), code.begin() + n);
            PixelInputs c;
            ReadPixelInputs(cut.data(), cut.size(), c);
            CHECK(!Slot(c, 99));
        }
        // A program that claims to be longer than its chunk, and an instruction of length zero.
        auto lying = Container({Program({3})});
        uint32_t huge = 0x7fffffff;
        std::memcpy(lying.data() + 36 + 8 + 4, &huge, 4);
        CHECK(ReadPixelInputs(lying.data(), lying.size(), in) && Slot(in, 3));
        Chunk zero = Program({4});
        zero.words[2] = 0x0000086a;
        const auto stuck = Container({zero});
        CHECK(ReadPixelInputs(stuck.data(), stuck.size(), in) && !Slot(in, 4));
    }},
}); }
