#pragma once
// Material preparation contract v1. No hooks or rendering dependencies.
// Disk representation: manifest.json, specified in docs/MATERIAL-CACHE-V1.md.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace na::materials {
inline constexpr uint32_t kSchemaVersion = 1;
enum class Role { BaseColor, Normal, Roughness, Packed };
enum class AddressMode { Wrap, Clamp }; // atlases require an explicit island-aware preparer
struct Source {
    std::string content_sha256; // owned source bytes, never a COM pointer or filename alone
    std::string identity_domain; // e.g. "file-bytes" or "texture-subresources-v1"
    uint32_t width = 0, height = 0;
    uint32_t first_mip = 0, mip_count = 0;
    uint32_t view_format = 0; // original typed DXGI format, zero = not qualified
    bool complete_observed_range = false; // not a claim that all possible streamed mips exist
    // Optional v1 provenance, requested by the runtime owner. Zero format is
    // unknown/file input. The v1 preparer still handles one ordinary 2D slice.
    uint32_t resource_format = 0;
    uint32_t first_array_slice = 0, array_size = 1;
};
struct Mip {
    uint32_t level = 0, width = 0, height = 0;
    uint64_t offset = 0, row_pitch = 0, byte_size = 0; // offset within DDS file
};
struct Map {
    std::string id; // material-local stable name; actual registers belong to the runtime binding
    Role role = Role::BaseColor;
    Source source;
    std::array<std::string, 4> channels; // explicit roles, including untouched/unused channels
    std::string encoding; // sRGB, tangent_xy_unorm_positive_z, perceptual_roughness, etc.
    uint32_t dxgi_format = 0, width = 0, height = 0;
    AddressMode address = AddressMode::Wrap;
    std::string file, sha256; // relative leaf DDS filename and full-file digest
    std::vector<Mip> mips;
};
struct Record {
    uint32_t schema_version = kSchemaVersion;
    std::string material_id; // logical material association, separate from cache entry identity
    std::string entry_key; // SHA256 of canonical recipe; all maps published as this version
    std::string algorithm, algorithm_version, settings_json;
    uint32_t seed = 0;
    std::vector<Map> maps;
    uint64_t payload_bytes = 0; // compressed mip payload only; runtime also budgets allocations/staging
    bool fixture = false; // synthetic input is never game compatibility evidence
};
} // namespace na::materials
