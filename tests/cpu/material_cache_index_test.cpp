// The runtime's material cache index and its v1 replacement rules (materials/cache_index.hpp).
#include "test_support.hpp"
#include "materials/cache_index.hpp"

using namespace na::materials;

namespace {
const std::string kA(64, 'a'), kB(64, 'b'), kC(64, 'c');

Map MakeMap(Role role, const std::string& digest, uint32_t resource_format, uint32_t view_format, uint32_t side = 64)
{
    Map m;
    m.id = role == Role::BaseColor ? "base_color" : role == Role::Normal ? "normal" : "roughness";
    m.role = role;
    m.width = m.height = side;
    for (uint32_t s = side, level = 0; s; s /= 2, ++level) {
        Mip mip;
        mip.level = level;
        mip.width = mip.height = s;
        m.mips.push_back(mip);
    }
    m.source.content_sha256 = digest;
    m.source.identity_domain = "texture-subresources-v1";
    m.source.width = m.source.height = side;
    m.source.mip_count = static_cast<uint32_t>(m.mips.size());
    m.source.resource_format = resource_format;
    m.source.view_format = view_format;
    m.source.complete_observed_range = true;
    return m;
}
Record MakeRecord(const std::string& key, std::vector<Map> maps)
{
    Record r;
    r.entry_key = key;
    r.maps = std::move(maps);
    return r;
}
} // namespace

int main()
{
    return test::run({
        {"a texture of no prepared shape is never hashed; a prepared one names its view formats", [] {
            CacheIndex index;
            CHECK(index.Add(MakeRecord(kA, {MakeMap(Role::BaseColor, kA, 70, 72), MakeMap(Role::Normal, kB, 82, 83),
                                            MakeMap(Role::Roughness, kC, 79, 80)})));
            CHECK(index.findable() == 3 && index.refusals().empty() && index.QualifiedMaps(0) == 3);
            CHECK(index.WantedViews({70, 64, 64, 7}) == std::vector<uint32_t>{72});
            CHECK(index.WantedViews({70, 128, 128, 8}).empty());
            CHECK(index.WantedViews({70, 64, 64, 6}).empty()); // the game's copy lacks a level
            CHECK(index.WantedViews({71, 64, 64, 7}).empty());
            CacheIndex::Hit hit;
            CHECK(index.Find(kB, {82, 64, 64, 7}, 83, hit) && hit.record == 0 && hit.map == 1);
            CHECK(!index.Find(kB, {82, 64, 64, 7}, 84, hit)); // read as signed
            CHECK(!index.Find(kA, {82, 64, 64, 7}, 83, hit)); // other bytes
        }},
        {"a map that would not mean the same thing to the game's shader is refused with the reason", [] {
            CHECK(Qualified(MakeMap(Role::BaseColor, kA, 70, 72)) == nullptr);
            CHECK(Qualified(MakeMap(Role::BaseColor, kA, 27, 29)) == nullptr);
            CHECK(std::string(Qualified(MakeMap(Role::BaseColor, kA, 70, 71))) == "the game reads this colour texture without sRGB decoding");
            CHECK(std::string(Qualified(MakeMap(Role::Normal, kA, 76, 77))) == "the game's normal texture is not two-channel BC5");
            CHECK(std::string(Qualified(MakeMap(Role::Roughness, kA, 27, 28))) == "the game's roughness texture is not one-channel BC4");
            Map larger = MakeMap(Role::BaseColor, kA, 70, 72, 128);
            larger.source.width = larger.source.height = 64;
            larger.source.mip_count = 7;
            CHECK(std::string(Qualified(larger)) == "prepared larger than the game's texture, which this runtime cannot check per shader");
            larger.width = 96; // not a whole doubling
            CHECK(std::string(Qualified(larger, true)) == "prepared at a size that is not the game's texture or a whole 2x, 4x or 8x of it");
            Map file = MakeMap(Role::BaseColor, kA, 70, 72);
            file.source.identity_domain = "file-bytes";
            CHECK(std::string(Qualified(file)) == "prepared from a file, not from a game texture");
            Map partial = MakeMap(Role::BaseColor, kA, 70, 72);
            partial.source.first_mip = 1;
            CHECK(std::string(Qualified(partial)) == "the source is a partial view or an array");
            Map unknown = MakeMap(Role::BaseColor, kA, 0, 72);
            CHECK(std::string(Qualified(unknown)) == "the source's formats are not recorded");
            Map packed = MakeMap(Role::BaseColor, kA, 70, 72);
            packed.role = Role::Packed;
            CHECK(std::string(Qualified(packed)) == "packed maps are not loadable in v1");
        }},
        {"a record keeps its qualified maps and lists the rest; a second entry for one texture is listed", [] {
            CacheIndex index;
            CHECK(index.Add(MakeRecord(kA, {MakeMap(Role::BaseColor, kA, 70, 72), MakeMap(Role::Normal, kB, 76, 77)})));
            CHECK(index.findable() == 1 && index.QualifiedMaps(0) == 1 && index.refusals().size() == 1);
            CHECK(index.refusals()[0].entry_key == kA && index.refusals()[0].map_id == "normal");
            CHECK(!index.Add(MakeRecord(kA, {MakeMap(Role::BaseColor, kC, 70, 72)}))); // the same entry twice
            CHECK(index.Add(MakeRecord(kB, {MakeMap(Role::BaseColor, kA, 70, 72)})));  // another recipe, same game texture
            CHECK(index.findable() == 1 && index.refusals().size() == 2 &&
                  index.refusals()[1].why == "another entry already covers this game texture");
            CacheIndex::Hit hit;
            CHECK(index.Find(kA, {70, 64, 64, 7}, 72, hit) && hit.record == 0);
            // The same bytes viewed two ways are two sources.
            CHECK(index.Add(MakeRecord(kC, {MakeMap(Role::BaseColor, kA, 70, 72, 32), MakeMap(Role::BaseColor, kA, 97, 99, 32)})));
            CHECK(index.Find(kA, {97, 32, 32, 6}, 99, hit) && hit.record == 2 && hit.map == 1);
        }},
        {"a 2x, 4x or 8x map with the extra levels is findable only in an index that allows larger maps, and marked", [] {
            auto Larger = [](uint32_t side, uint32_t source_side) {
                Map m = MakeMap(Role::BaseColor, kA, 70, 72, side);
                m.source.width = m.source.height = source_side;
                m.source.mip_count = 0;
                for (uint32_t s = source_side; s; s /= 2) ++m.source.mip_count;
                return m;
            };
            CHECK(LargerLog2(Larger(64, 64)) == 0 && LargerLog2(Larger(256, 64)) == 2 && LargerLog2(Larger(512, 64)) == 3);
            CHECK(LargerLog2(Larger(1024, 64)) == -1 && LargerLog2(Larger(32, 64)) == -1);
            Map short_chain = Larger(256, 64);
            short_chain.mips.resize(7); // 4x the size with only the game's level count
            CHECK(LargerLog2(short_chain) == -1);
            Map one_level = Larger(256, 64); // a game texture with one level: the map has it plus two
            one_level.source.mip_count = 1;
            one_level.mips.resize(3);
            CHECK(LargerLog2(one_level) == 2);
            CHECK(std::string(Qualified(Larger(256, 64))) == "prepared larger than the game's texture, which this runtime cannot check per shader");
            CHECK(Qualified(Larger(256, 64), true) == nullptr);
            CacheIndex plain, allowing(true);
            CHECK(plain.Add(MakeRecord(kA, {Larger(256, 64)})) && plain.findable() == 0 && plain.refusals().size() == 1);
            CHECK(allowing.Add(MakeRecord(kA, {Larger(256, 64)})) && allowing.findable() == 1);
            CacheIndex::Hit hit;
            CHECK(allowing.WantedViews({70, 64, 64, 7}) == std::vector<uint32_t>{72}); // keyed by the game's shape
            CHECK(allowing.Find(kA, {70, 64, 64, 7}, 72, hit) && hit.larger_log2 == 2);
        }},
    });
}
