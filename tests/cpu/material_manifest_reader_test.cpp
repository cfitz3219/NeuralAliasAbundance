// Runtime reading of a material cache entry (materials/manifest_reader.hpp). The fixture is the
// shape tools/material_lab/cache.py writes, generated with that tool's own DDS header and layout
// code for an 8x8 three-map entry with zeroed block payloads.
#include "test_support.hpp"
#include "materials/manifest_reader.hpp"

#include <string>
#include <vector>

using namespace na::materials;

namespace {
const char kManifest[] = R"MANIFEST({
  "schema_version": 1,
  "material_id": "loader_fixture",
  "algorithm": "na-guided-material-detail",
  "algorithm_version": "1",
  "settings": {
    "strength": 1.0,
    "family": "wood"
  },
  "seed": 12,
  "fixture": true,
  "entry_key": "a27f1b4c6c62808b4e450b448a530cff3a8f8cbfbff671f2f5276221c3b53889",
  "recipe": {
    "schema_version": 1,
    "material_id": "loader_fixture",
    "algorithm": "na-guided-material-detail",
    "algorithm_version": "1",
    "settings": {
      "strength": 1.0,
      "family": "wood"
    },
    "seed": 12,
    "fixture": true,
    "sources": [
      {
        "id": "base_color",
        "role": "base_color",
        "source": {
          "content_sha256": "c212b3ffca5edfa74e06c2840b8186018ed11c3ff7adc5b3810527fe4feffa31",
          "identity_domain": "texture-subresources-v1",
          "width": 8,
          "height": 8,
          "first_mip": 0,
          "mip_count": 4,
          "view_format": 72,
          "complete_observed_range": true,
          "resource_format": 70,
          "first_array_slice": 0,
          "array_size": 1
        },
        "channels": [
          "color_r",
          "color_g",
          "color_b",
          "opacity"
        ],
        "encoding": "srgb",
        "address": "wrap"
      },
      {
        "id": "normal",
        "role": "normal",
        "source": {
          "content_sha256": "ffa6a744d78d28386438b4a8e8ee32f56718633735057b1b5ddfb5d224d45d98",
          "identity_domain": "texture-subresources-v1",
          "width": 8,
          "height": 8,
          "first_mip": 0,
          "mip_count": 4,
          "view_format": 83,
          "complete_observed_range": true,
          "resource_format": 82,
          "first_array_slice": 0,
          "array_size": 1
        },
        "channels": [
          "normal_x",
          "normal_y",
          "unused",
          "unused"
        ],
        "encoding": "tangent_xy_unorm_positive_z",
        "address": "wrap"
      },
      {
        "id": "roughness",
        "role": "roughness",
        "source": {
          "content_sha256": "ada987408d4654e36fc691a5d4383af83cdaf396c088eb6370a5a18235786882",
          "identity_domain": "texture-subresources-v1",
          "width": 8,
          "height": 8,
          "first_mip": 0,
          "mip_count": 4,
          "view_format": 80,
          "complete_observed_range": true,
          "resource_format": 79,
          "first_array_slice": 0,
          "array_size": 1
        },
        "channels": [
          "roughness",
          "unused",
          "unused",
          "unused"
        ],
        "encoding": "perceptual_roughness",
        "address": "wrap"
      }
    ],
    "encoder_sha256": "5fb2ab76ed9bda034b192c48c7069359252fccda168d925acc0ae7d316c0b53e",
    "numpy_version": "2.3.5"
  },
  "maps": [
    {
      "id": "base_color",
      "role": "base_color",
      "source": {
        "content_sha256": "c212b3ffca5edfa74e06c2840b8186018ed11c3ff7adc5b3810527fe4feffa31",
        "identity_domain": "texture-subresources-v1",
        "width": 8,
        "height": 8,
        "first_mip": 0,
        "mip_count": 4,
        "view_format": 72,
        "complete_observed_range": true,
        "resource_format": 70,
        "first_array_slice": 0,
        "array_size": 1
      },
      "channels": [
        "color_r",
        "color_g",
        "color_b",
        "opacity"
      ],
      "encoding": "srgb",
      "address": "wrap",
      "dxgi_format": 99,
      "width": 8,
      "height": 8,
      "file": "base_color.dds",
      "sha256": "12ea837012a6f29295e42646ee49c7d21788b577780582a1c96920d65f963c1c",
      "mips": [
        {
          "level": 0,
          "width": 8,
          "height": 8,
          "offset": 148,
          "row_pitch": 32,
          "byte_size": 64
        },
        {
          "level": 1,
          "width": 4,
          "height": 4,
          "offset": 212,
          "row_pitch": 16,
          "byte_size": 16
        },
        {
          "level": 2,
          "width": 2,
          "height": 2,
          "offset": 228,
          "row_pitch": 16,
          "byte_size": 16
        },
        {
          "level": 3,
          "width": 1,
          "height": 1,
          "offset": 244,
          "row_pitch": 16,
          "byte_size": 16
        }
      ]
    },
    {
      "id": "normal",
      "role": "normal",
      "source": {
        "content_sha256": "ffa6a744d78d28386438b4a8e8ee32f56718633735057b1b5ddfb5d224d45d98",
        "identity_domain": "texture-subresources-v1",
        "width": 8,
        "height": 8,
        "first_mip": 0,
        "mip_count": 4,
        "view_format": 83,
        "complete_observed_range": true,
        "resource_format": 82,
        "first_array_slice": 0,
        "array_size": 1
      },
      "channels": [
        "normal_x",
        "normal_y",
        "unused",
        "unused"
      ],
      "encoding": "tangent_xy_unorm_positive_z",
      "address": "wrap",
      "dxgi_format": 83,
      "width": 8,
      "height": 8,
      "file": "normal.dds",
      "sha256": "858c6057fb7ecff50f791b60222d4f5d7ea94cc7f13ddb955df9a8dd5b96a2b7",
      "mips": [
        {
          "level": 0,
          "width": 8,
          "height": 8,
          "offset": 148,
          "row_pitch": 32,
          "byte_size": 64
        },
        {
          "level": 1,
          "width": 4,
          "height": 4,
          "offset": 212,
          "row_pitch": 16,
          "byte_size": 16
        },
        {
          "level": 2,
          "width": 2,
          "height": 2,
          "offset": 228,
          "row_pitch": 16,
          "byte_size": 16
        },
        {
          "level": 3,
          "width": 1,
          "height": 1,
          "offset": 244,
          "row_pitch": 16,
          "byte_size": 16
        }
      ]
    },
    {
      "id": "roughness",
      "role": "roughness",
      "source": {
        "content_sha256": "ada987408d4654e36fc691a5d4383af83cdaf396c088eb6370a5a18235786882",
        "identity_domain": "texture-subresources-v1",
        "width": 8,
        "height": 8,
        "first_mip": 0,
        "mip_count": 4,
        "view_format": 80,
        "complete_observed_range": true,
        "resource_format": 79,
        "first_array_slice": 0,
        "array_size": 1
      },
      "channels": [
        "roughness",
        "unused",
        "unused",
        "unused"
      ],
      "encoding": "perceptual_roughness",
      "address": "wrap",
      "dxgi_format": 80,
      "width": 8,
      "height": 8,
      "file": "roughness.dds",
      "sha256": "06020d213a4c0420c5fca936e98a765976970989033f35bd5f8d64910a57e9fb",
      "mips": [
        {
          "level": 0,
          "width": 8,
          "height": 8,
          "offset": 148,
          "row_pitch": 16,
          "byte_size": 32
        },
        {
          "level": 1,
          "width": 4,
          "height": 4,
          "offset": 180,
          "row_pitch": 8,
          "byte_size": 8
        },
        {
          "level": 2,
          "width": 2,
          "height": 2,
          "offset": 188,
          "row_pitch": 8,
          "byte_size": 8
        },
        {
          "level": 3,
          "width": 1,
          "height": 1,
          "offset": 196,
          "row_pitch": 8,
          "byte_size": 8
        }
      ]
    }
  ],
  "payload_bytes": 280
}
)MANIFEST";

std::string Replaced(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    CHECK(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
bool Reads(const std::string& text, std::string* why_out = nullptr)
{
    Record r;
    std::string why;
    const bool ok = ReadManifest(text, r, why);
    CHECK(ok == why.empty());
    if (why_out) *why_out = why;
    return ok;
}
// The file the writer's header code produces for a map whose payload is zeros.
std::vector<uint8_t> ZeroDds(const Map& m)
{
    std::vector<uint8_t> d(static_cast<size_t>(m.mips.back().offset + m.mips.back().byte_size), 0);
    const auto put = [&](size_t byte, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[byte + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    d[0] = 'D'; d[1] = 'D'; d[2] = 'S'; d[3] = ' ';
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | 0x20000);
    put(12, m.height);
    put(16, m.width);
    put(20, static_cast<uint32_t>(m.mips[0].byte_size));
    put(28, static_cast<uint32_t>(m.mips.size()));
    put(76, 32);
    put(80, 4);
    d[84] = 'D'; d[85] = 'X'; d[86] = '1'; d[87] = '0';
    put(108, 0x1000 | 0x8 | 0x400000);
    put(128, m.dxgi_format);
    put(132, 3);
    put(140, 1);
    put(144, m.dxgi_format == 99 ? 1u : 3u);
    return d;
}
} // namespace

int main()
{
    return test::run({
        {"a written entry reads into the record the runtime acts on", [] {
            Record r;
            std::string why;
            CHECK(ReadManifest(kManifest, r, why));
            CHECK(r.schema_version == 1 && r.material_id == "loader_fixture" && r.fixture && r.seed == 12);
            CHECK(r.entry_key == "a27f1b4c6c62808b4e450b448a530cff3a8f8cbfbff671f2f5276221c3b53889");
            CHECK(r.maps.size() == 3 && r.payload_bytes == 280);
            CHECK(r.maps[0].role == Role::BaseColor && r.maps[0].dxgi_format == 99 && r.maps[0].file == "base_color.dds");
            CHECK(r.maps[1].role == Role::Normal && r.maps[1].dxgi_format == 83);
            CHECK(r.maps[2].role == Role::Roughness && r.maps[2].dxgi_format == 80 && r.maps[2].channels[0] == "roughness");
            CHECK(r.maps[0].mips.size() == 4 && r.maps[0].mips[0].offset == 148 && r.maps[0].mips[0].byte_size == 64 &&
                  r.maps[0].mips[3].offset == 148 + 64 + 16 + 16 && r.maps[2].mips[0].byte_size == 32);
            const Source& s = r.maps[0].source;
            CHECK(s.identity_domain == "texture-subresources-v1" && s.view_format == 72 && s.resource_format == 70 && s.mip_count == 4 &&
                  s.complete_observed_range && s.array_size == 1);
        }},
        {"each map's file is checked by header, size and digest before use", [] {
            Record r;
            std::string why;
            CHECK(ReadManifest(kManifest, r, why));
            for (const Map& m : r.maps) {
                auto file = ZeroDds(m);
                CHECK(CheckDds(file.data(), file.size(), m, why));
                file.back() ^= 1; // one payload bit
                CHECK(!CheckDds(file.data(), file.size(), m, why) && why == "file SHA-256 differs from the manifest");
                file.back() ^= 1;
                file[128] = 98; // BC7 without sRGB
                CHECK(!CheckDds(file.data(), file.size(), m, why) && why == "DDS is not one 2D slice of the manifest's format");
                file[128] = static_cast<uint8_t>(m.dxgi_format);
                CHECK(!CheckDds(file.data(), file.size() - 1, m, why) && why == "file size differs from the manifest's layout");
                file.push_back(0);
                CHECK(!CheckDds(file.data(), file.size(), m, why));
            }
            // A map's file never passes as another map's.
            const auto colour = ZeroDds(r.maps[0]);
            CHECK(!CheckDds(colour.data(), colour.size(), r.maps[1], why));
        }},
        {"only a game texture with the recorded identity and view takes a map", [] {
            Record r;
            std::string why;
            CHECK(ReadManifest(kManifest, r, why));
            const Source& s = r.maps[0].source;
            CHECK(MatchesGameTexture(s, s.content_sha256, 70, 72, 8, 8, 0, 4));
            CHECK(!MatchesGameTexture(s, r.maps[1].source.content_sha256, 70, 72, 8, 8, 0, 4)); // other bytes
            CHECK(!MatchesGameTexture(s, s.content_sha256, 70, 71, 8, 8, 0, 4));                // read without sRGB
            CHECK(!MatchesGameTexture(s, s.content_sha256, 70, 72, 8, 8, 1, 3));                // a narrower view
            Source file_bytes = s;
            file_bytes.identity_domain = "file-bytes"; // prototype inputs never match a game texture
            CHECK(!MatchesGameTexture(file_bytes, s.content_sha256, 70, 72, 8, 8, 0, 4));
            Source partial = s;
            partial.complete_observed_range = false;
            CHECK(!MatchesGameTexture(partial, s.content_sha256, 70, 72, 8, 8, 0, 4));
        }},
        {"optional source provenance defaults when absent and is refused when malformed", [] {
            Record r;
            std::string why;
            // In the fixture the recipe's copy of each source comes first; the map's own copy second.
            const auto second = [](std::string text, const std::string& from, const std::string& to) {
                const size_t first = text.find(from);
                CHECK(first != std::string::npos);
                const size_t at = text.find(from, first + 1);
                CHECK(at != std::string::npos);
                return text.replace(at, from.size(), to);
            };
            const std::string absent = second(kManifest, "\"resource_format\": 70,", "");
            CHECK(ReadManifest(absent, r, why) && r.maps[0].source.resource_format == 0 && r.maps[0].source.array_size == 1);
            CHECK(!ReadManifest(second(kManifest, "\"resource_format\": 70,", "\"resource_format\": \"BC1\","), r, why) &&
                  why == "map base_color: source resource_format malformed");
            // The recipe is not interpreted, so removing a field there alone changes nothing.
            CHECK(Reads(Replaced(kManifest, "\"resource_format\": 70,", "\"resource_format\": 71,")));
        }},
        {"what the runtime cannot act on is refused with the reason", [] {
            std::string why;
            CHECK(!Reads(Replaced(kManifest, "\"schema_version\": 1,", "\"schema_version\": 2,"), &why) && why == "schema_version is not 1");
            CHECK(!Reads(Replaced(kManifest, "\"entry_key\": \"a27f", "\"entry_key\": \"A27f"), &why) && why == "entry_key is not a lowercase SHA-256");
            CHECK(!Reads(Replaced(kManifest, "\"payload_bytes\": 280", "\"payload_bytes\": 281"), &why) && why == "payload_bytes is not the sum of the maps' mips");
            CHECK(!Reads(Replaced(kManifest, "\"file\": \"base_color.dds\"", "\"file\": \"..\\\\base_color.dds\""), &why) &&
                  why == "map base_color: file is not a plain leaf name");
            CHECK(!Reads(Replaced(kManifest, "\"file\": \"normal.dds\"", "\"file\": \"sub/normal.dds\""), &why) &&
                  why == "map normal: file is not a plain leaf name");
            CHECK(!Reads(Replaced(kManifest, "\"dxgi_format\": 83", "\"dxgi_format\": 80"), &why) && why == "map normal: format does not belong to the role");
            CHECK(!Reads(Replaced(kManifest, "\"dxgi_format\": 99", "\"dxgi_format\": 99.0"), &why));
            CHECK(!Reads(Replaced(kManifest, "\"seed\": 12", "\"seed\": -12"), &why) && why == "seed missing");
            CHECK(!Reads(std::string(kManifest) + "x", &why) && why == "manifest is not a JSON object");
            CHECK(!Reads("[]", &why) && !Reads("", &why) && !Reads("{", &why));
            CHECK(!Reads(std::string(64, '[') + std::string(64, ']'), &why)); // nesting limit, no deep recursion
        }},
        {"a mip chain that is not the full contiguous chain is refused", [] {
            std::string why;
            // First map's first level: offset 148, then 212.
            CHECK(!Reads(Replaced(kManifest, "\"offset\": 212", "\"offset\": 216"), &why) &&
                  why == "map base_color: mips are not a contiguous floor-halving chain after the header");
            CHECK(!Reads(Replaced(kManifest, "\"byte_size\": 64", "\"byte_size\": 60"), &why));
        }},
        {"the JSON reader decodes escapes and keeps integers exact", [] {
            Json v;
            CHECK(JsonParser::Parse(R"({"a":"x\n\u00e9\ud83d\ude00\"","n":18446744073709551615,"neg":-1,"list":[true,false,null,1.5e3]})", v));
            CHECK(v.Get("a")->text == "x\n\xC3\xA9\xF0\x9F\x98\x80\"");
            uint64_t n = 0;
            CHECK(v.Get("n")->Unsigned(n) && n == 18446744073709551615ull);
            CHECK(!v.Get("neg")->Unsigned(n) && !v.Get("list")->items[3].Unsigned(n));
            CHECK(v.Get("list")->items.size() == 4 && v.Get("list")->items[0].boolean && v.Get("missing") == nullptr);
            Json bad;
            CHECK(!JsonParser::Parse(R"({"n":18446744073709551616})", bad) || !bad.Get("n")->Unsigned(n));
            for (const char* text : {"{\"a\":}", "{\"a\" 1}", "[1,]", "\"\\ud83d\"", "\"\\x\"", "01", "1.", "tru", "{\"a\":1,}", "\"open"}) {
                Json j;
                CHECK(!JsonParser::Parse(text, j));
            }
        }},
    });
}
