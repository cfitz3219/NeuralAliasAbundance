// Reads one material cache entry for the runtime: manifest.json (docs/MATERIAL-CACHE-V1.md) into
// a Record (record.hpp), and checks a map's DDS file against it before any GPU object is made.
// Owner: Fable (runtime loading). The format and its writer are Astra's (tools/material_lab/cache.py).
//
// What is checked: schema version, the entry key's form, every field the runtime acts on, each
// map's role/format pairing, a full floor-halving mip chain laid out contiguously after the 148-byte
// DDS+DX10 header, payload_bytes, and (CheckDds) the file's own header, size and SHA-256.
// What is NOT checked: that entry_key is the hash of `recipe`. That needs the writer's exact
// canonical JSON; the runtime instead trusts an entry only under a folder named by its entry_key
// and with every map file matching its recorded SHA-256.
//
// Anything not understood is refused with a worded reason, never guessed. Portable C++17.
#pragma once

#include "materials/record.hpp"
#include "materials/texture_subresources_v1.hpp" // Sha256

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace na::materials {

// A small JSON value: enough for a manifest. Numbers keep their text so integers stay exact.
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // String: the decoded text. Number: as written.
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* Get(std::string_view key) const
    {
        if (kind != Kind::Object) return nullptr;
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
    // A non-negative integer no larger than `max`, written without sign, fraction or exponent.
    bool Unsigned(uint64_t& out, uint64_t max = ~0ull) const
    {
        if (kind != Kind::Number || text.empty() || text.size() > 20) return false;
        uint64_t v = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const uint64_t d = static_cast<uint64_t>(c - '0');
            if (v > (~0ull - d) / 10) return false;
            v = v * 10 + d;
        }
        if (v > max) return false;
        out = v;
        return true;
    }
};

class JsonParser {
public:
    // False on malformed input, nesting deeper than 32, or anything after the value.
    static bool Parse(std::string_view s, Json& out)
    {
        JsonParser p(s);
        if (!p.Value(out, 0)) return false;
        p.Space();
        return p.i_ == s.size();
    }

private:
    explicit JsonParser(std::string_view s) : s_(s) {}
    void Space()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool Word(std::string_view w)
    {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    static void Utf8(uint32_t c, std::string& out)
    {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    bool Hex4(uint32_t& v)
    {
        if (s_.size() - i_ < 4) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            const uint32_t d = c >= '0' && c <= '9' ? uint32_t(c - '0') : c >= 'a' && c <= 'f' ? uint32_t(c - 'a' + 10)
                             : c >= 'A' && c <= 'F' ? uint32_t(c - 'A' + 10) : 16u;
            if (d > 15) return false;
            v = v * 16 + d;
        }
        return true;
    }
    bool String(std::string& out)
    {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t v = 0;
                if (!Hex4(v)) return false;
                if (v >= 0xD800 && v < 0xDC00) { // a surrogate pair
                    uint32_t low = 0;
                    if (s_.substr(i_, 2) != "\\u") return false;
                    i_ += 2;
                    if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (low - 0xDC00);
                } else if (v >= 0xDC00 && v < 0xE000) {
                    return false;
                }
                Utf8(v, out);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Number(Json& out)
    {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        const size_t digits = i_;
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        if (i_ == digits || (s_[digits] == '0' && i_ - digits > 1)) return false;
        if (i_ < s_.size() && s_[i_] == '.') {
            const size_t frac = ++i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == frac) return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            const size_t exp = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == exp) return false;
        }
        out.kind = Json::Kind::Number;
        out.text.assign(s_.substr(start, i_ - start));
        return true;
    }
    bool Value(Json& out, int depth)
    {
        if (depth > 32) return false;
        Space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            out.kind = Json::Kind::Object;
            Space();
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
            for (;;) {
                Space();
                std::string key;
                if (!String(key)) return false;
                Space();
                if (i_ >= s_.size() || s_[i_++] != ':') return false;
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(v));
                Space();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                return s_[i_++] == '}';
            }
        }
        if (c == '[') {
            ++i_;
            out.kind = Json::Kind::Array;
            Space();
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
            for (;;) {
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.items.push_back(std::move(v));
                Space();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == ',') { ++i_; continue; }
                return s_[i_++] == ']';
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return String(out.text);
        }
        if (c == 't' || c == 'f') {
            out.kind = Json::Kind::Bool;
            out.boolean = c == 't';
            return Word(out.boolean ? "true" : "false");
        }
        if (c == 'n') return Word("null");
        return Number(out);
    }
    std::string_view s_;
    size_t i_ = 0;
};

inline bool IsSha256Hex(std::string_view s)
{
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline constexpr uint64_t kDdsHeaderBytes = 148; // "DDS " + DDS_HEADER + DDS_HEADER_DXT10

// The role's one allowed payload format in v1, and its bytes per 4x4 block. Zero for Packed
// (repacking needs a proved shader interpretation; nothing publishes it yet).
inline uint32_t FormatOf(Role role, uint32_t& block_bytes)
{
    switch (role) {
    case Role::BaseColor: block_bytes = 16; return 99; // BC7_UNORM_SRGB
    case Role::Normal: block_bytes = 16; return 83;    // BC5_UNORM
    case Role::Roughness: block_bytes = 8; return 80;  // BC4_UNORM
    default: block_bytes = 0; return 0;
    }
}

namespace detail {
inline bool Str(const Json& o, const char* key, std::string& out)
{
    const Json* v = o.Get(key);
    if (!v || v->kind != Json::Kind::String) return false;
    out = v->text;
    return true;
}
inline bool U32(const Json& o, const char* key, uint32_t& out)
{
    const Json* v = o.Get(key);
    uint64_t n = 0;
    if (!v || !v->Unsigned(n, 0xffffffffull)) return false;
    out = static_cast<uint32_t>(n);
    return true;
}
inline bool U64(const Json& o, const char* key, uint64_t& out)
{
    const Json* v = o.Get(key);
    return v && v->Unsigned(out);
}
inline bool Fail(std::string& why, std::string text)
{
    why = std::move(text);
    return false;
}
} // namespace detail

// Fills `out` from manifest.json's text. False with a reason; `out` is then not to be used.
inline bool ReadManifest(std::string_view json, Record& out, std::string& why)
{
    using namespace detail;
    out = Record{};
    Json root;
    if (json.size() > (4u << 20)) return Fail(why, "manifest larger than 4 MiB");
    if (!JsonParser::Parse(json, root) || root.kind != Json::Kind::Object) return Fail(why, "manifest is not a JSON object");
    if (!U32(root, "schema_version", out.schema_version) || out.schema_version != kSchemaVersion)
        return Fail(why, "schema_version is not 1");
    if (!Str(root, "material_id", out.material_id) || out.material_id.empty()) return Fail(why, "material_id missing");
    if (!Str(root, "entry_key", out.entry_key) || !IsSha256Hex(out.entry_key)) return Fail(why, "entry_key is not a lowercase SHA-256");
    if (!Str(root, "algorithm", out.algorithm) || !Str(root, "algorithm_version", out.algorithm_version))
        return Fail(why, "algorithm or algorithm_version missing");
    if (!U32(root, "seed", out.seed)) return Fail(why, "seed missing");
    const Json* fixture = root.Get("fixture");
    if (!fixture || fixture->kind != Json::Kind::Bool) return Fail(why, "fixture missing");
    out.fixture = fixture->boolean;
    if (!U64(root, "payload_bytes", out.payload_bytes)) return Fail(why, "payload_bytes missing");
    const Json* maps = root.Get("maps");
    if (!maps || maps->kind != Json::Kind::Array || maps->items.empty() || maps->items.size() > 8)
        return Fail(why, "maps missing, empty or more than eight");
    uint64_t payload = 0;
    for (const Json& jm : maps->items) {
        if (jm.kind != Json::Kind::Object) return Fail(why, "a map is not an object");
        Map m;
        std::string role, address;
        if (!Str(jm, "id", m.id) || m.id.empty() || !Str(jm, "role", role)) return Fail(why, "a map has no id or role");
        const std::string at = "map " + m.id + ": ";
        if (role == "base_color") m.role = Role::BaseColor;
        else if (role == "normal") m.role = Role::Normal;
        else if (role == "roughness") m.role = Role::Roughness;
        else if (role == "packed") m.role = Role::Packed;
        else return Fail(why, at + "unknown role");
        for (const Map& earlier : out.maps)
            if (earlier.id == m.id) return Fail(why, at + "id used twice");
        if (!Str(jm, "address", address) || (address != "wrap" && address != "clamp")) return Fail(why, at + "address is not wrap or clamp");
        m.address = address == "wrap" ? AddressMode::Wrap : AddressMode::Clamp;
        if (!Str(jm, "encoding", m.encoding)) return Fail(why, at + "encoding missing");
        const Json* channels = jm.Get("channels");
        if (!channels || channels->kind != Json::Kind::Array || channels->items.size() != 4) return Fail(why, at + "channels is not four names");
        for (size_t i = 0; i < 4; ++i) {
            if (channels->items[i].kind != Json::Kind::String) return Fail(why, at + "channels is not four names");
            m.channels[i] = channels->items[i].text;
        }
        // The source the map was prepared from: what a game texture must match to take it.
        const Json* js = jm.Get("source");
        if (!js || js->kind != Json::Kind::Object) return Fail(why, at + "source missing");
        Source& s = m.source;
        if (!Str(*js, "content_sha256", s.content_sha256) || !IsSha256Hex(s.content_sha256)) return Fail(why, at + "source digest is not a lowercase SHA-256");
        if (!Str(*js, "identity_domain", s.identity_domain) || s.identity_domain.empty()) return Fail(why, at + "source identity_domain missing");
        if (!U32(*js, "width", s.width) || !U32(*js, "height", s.height) || !U32(*js, "first_mip", s.first_mip) ||
            !U32(*js, "mip_count", s.mip_count) || !U32(*js, "view_format", s.view_format))
            return Fail(why, at + "source extent, mip range or view_format missing");
        const Json* complete = js->Get("complete_observed_range");
        if (!complete || complete->kind != Json::Kind::Bool) return Fail(why, at + "source complete_observed_range missing");
        s.complete_observed_range = complete->boolean;
        // Optional provenance: absent means the documented defaults, present must be well formed.
        if (js->Get("resource_format") && !U32(*js, "resource_format", s.resource_format)) return Fail(why, at + "source resource_format malformed");
        if (js->Get("first_array_slice") && !U32(*js, "first_array_slice", s.first_array_slice)) return Fail(why, at + "source first_array_slice malformed");
        if (js->Get("array_size") && !U32(*js, "array_size", s.array_size)) return Fail(why, at + "source array_size malformed");
        // The payload.
        uint32_t block = 0;
        uint32_t want = FormatOf(m.role, block);
        if (m.role == Role::Normal && m.encoding == "tangent_xy_snorm_positive_z") want = 84;
        if (!U32(jm, "dxgi_format", m.dxgi_format) || !U32(jm, "width", m.width) || !U32(jm, "height", m.height))
            return Fail(why, at + "dxgi_format or extent missing");
        if (!want) return Fail(why, at + "packed maps are not loadable in v1");
        if (m.dxgi_format != want) return Fail(why, at + "format does not belong to the role");
        if (!m.width || !m.height || m.width > 16384 || m.height > 16384 || (m.width & 3) || (m.height & 3))
            return Fail(why, at + "extent is zero, over 16384 or not a multiple of four");
        if (!Str(jm, "file", m.file) || m.file.empty() || m.file.size() > 96 || m.file[0] == '.' ||
            m.file.find_first_of("/\\:") != std::string::npos)
            return Fail(why, at + "file is not a plain leaf name");
        if (!Str(jm, "sha256", m.sha256) || !IsSha256Hex(m.sha256)) return Fail(why, at + "file digest is not a lowercase SHA-256");
        const Json* mips = jm.Get("mips");
        if (!mips || mips->kind != Json::Kind::Array) return Fail(why, at + "mips missing");
        uint32_t w = m.width, h = m.height;
        uint64_t offset = kDdsHeaderBytes;
        for (size_t level = 0; level < mips->items.size(); ++level) {
            const Json& jl = mips->items[level];
            Mip mip;
            if (jl.kind != Json::Kind::Object || !U32(jl, "level", mip.level) || !U32(jl, "width", mip.width) || !U32(jl, "height", mip.height) ||
                !U64(jl, "offset", mip.offset) || !U64(jl, "row_pitch", mip.row_pitch) || !U64(jl, "byte_size", mip.byte_size))
                return Fail(why, at + "a mip is malformed");
            const uint64_t pitch = uint64_t((w + 3) / 4) * block, size = pitch * ((h + 3) / 4);
            if (mip.level != level || mip.width != w || mip.height != h || mip.offset != offset || mip.row_pitch != pitch || mip.byte_size != size)
                return Fail(why, at + "mips are not a contiguous floor-halving chain after the header");
            offset += size;
            m.mips.push_back(mip);
            if (w == 1 && h == 1) {
                if (level + 1 != mips->items.size()) return Fail(why, at + "mips continue past 1x1");
                break;
            }
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        if (m.mips.empty() || m.mips.back().width != 1 || m.mips.back().height != 1) return Fail(why, at + "mip chain does not reach 1x1");
        payload += offset - kDdsHeaderBytes;
        out.maps.push_back(std::move(m));
    }
    if (payload != out.payload_bytes) return Fail(why, "payload_bytes is not the sum of the maps' mips");
    return true;
}

// The whole DDS file of `map`, as read from disk: header fields, exact size and SHA-256.
inline bool CheckDds(const uint8_t* data, size_t size, const Map& map, std::string& why)
{
    using detail::Fail;
    if (!data || map.mips.empty()) return Fail(why, "no data");
    const Mip& last = map.mips.back();
    if (size != last.offset + last.byte_size) return Fail(why, "file size differs from the manifest's layout");
    const auto word = [&](size_t byte) {
        return uint32_t(data[byte]) | (uint32_t(data[byte + 1]) << 8) | (uint32_t(data[byte + 2]) << 16) | (uint32_t(data[byte + 3]) << 24);
    };
    if (std::memcmp(data, "DDS ", 4) != 0 || word(4) != 124 || std::memcmp(data + 84, "DX10", 4) != 0)
        return Fail(why, "not a DDS file with a DX10 header");
    if (word(12) != map.height || word(16) != map.width || word(28) != map.mips.size())
        return Fail(why, "DDS extent or mip count differs from the manifest");
    if (word(128) != map.dxgi_format || word(132) != 3 || word(136) != 0 || word(140) != 1)
        return Fail(why, "DDS is not one 2D slice of the manifest's format");
    Sha256 sha;
    sha.Update(data, size);
    static const char hex[] = "0123456789abcdef";
    std::string got;
    for (uint8_t b : sha.Final()) {
        got.push_back(hex[b >> 4]);
        got.push_back(hex[b & 15]);
    }
    if (got != map.sha256) return Fail(why, "file SHA-256 differs from the manifest");
    return true;
}

// What a game texture must present to take `map` (the lookup key the runtime indexes by).
// Only sources named by "texture-subresources-v1" can match a game texture; file-bytes sources
// (prototype fixtures) never do.
inline bool MatchesGameTexture(const Source& s, std::string_view content_sha256, uint32_t resource_format, uint32_t view_format,
                               uint32_t width, uint32_t height, uint32_t first_mip, uint32_t mip_count)
{
    return s.identity_domain == "texture-subresources-v1" && s.complete_observed_range && s.content_sha256 == content_sha256 &&
           s.resource_format == resource_format && s.view_format == view_format && s.width == width && s.height == height &&
           s.first_mip == first_mip && s.mip_count == mip_count && s.first_array_slice == 0 && s.array_size == 1;
}

} // namespace na::materials
