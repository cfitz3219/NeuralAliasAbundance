// Native DX11 route: prepared material textures from the cache into the game (the publisher that
// textures.hpp's swap was built for). The cache is docs/MATERIAL-CACHE-V1.md: one folder per entry,
// named by its entry_key, holding manifest.json and one DDS per map.
//
// How a game texture gets its prepared map, without a GPU read-back and without work in a game call:
//  1. At the game's CreateTexture2D: only when the cache holds an entry of this exact shape
//     (format, size, mip count), the creation data is copied (a memory copy) and handed to the
//     loader's thread, which computes the "texture-subresources-v1" digest for the view formats the
//     cache asks about. A texture created empty and filled later has no identity here yet; it is
//     counted (`no_creation_data`) and keeps its own content.
//  2. At the game's CreateShaderResourceView of a candidate view (texture_policy.hpp): the view
//     waits on, or is matched against, its texture's digest.
//  3. A match names one map of one entry. The loader's thread reads that map's file, checks it
//     against the manifest (header, size, SHA-256), and creates the block-compressed texture.
//  4. An entry is shown as a unit: when every qualified map of it has a live game view and a
//     created texture, all are published in one step, so colour, normal and roughness of a material
//     change on the same Present. Views that arrive later for a shown entry join it at once.
//
// v1 replaces only where the prepared map means the same to the game's shader as its own texture
// (materials/cache_index.hpp Qualified): same size and mip count, sRGB colour for sRGB colour, BC5
// for BC5, BC4 for BC4. Everything else is listed in the log with its reason and left alone.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace na::native11 {

struct MaterialTextureFacts;

// Reads every entry under `dir` (null: %LOCALAPPDATA%\NeuralAlias\materials) and replaces the
// loaded index. Manifests only; no map file is read until a game texture matches. Textures the game
// created before the call are not matched (their creation data is gone). Returns the entries read.
// Call before the game creates its textures; InstallTextures does, with the default folder.
size_t LoadMaterialCache(const wchar_t* dir);

// From textures.cpp's device hooks, after the game's successful calls.
void MaterialsOnTextureCreated(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* initial,
                               ID3D11Texture2D* texture);
void MaterialsOnViewCreated(ID3D11Resource* resource, ID3D11ShaderResourceView* view, uint64_t serial, const MaterialTextureFacts& facts);

struct MaterialStats {
    uint64_t entries = 0, findable_maps = 0, refused_maps = 0, unreadable_entries = 0; // the loaded index
    uint64_t textures_hashed = 0, hash_bytes = 0;   // game textures of a prepared shape, digested from creation data
    uint64_t no_creation_data = 0;                  // of a prepared shape but created empty
    uint64_t hash_backlog_skipped = 0;              // not copied: too much creation data already waiting
    uint64_t views_matched = 0;                     // candidate views whose texture is a prepared source
    uint64_t maps_loaded = 0, maps_failed = 0;      // map files read, checked and created / refused with a logged reason
    uint64_t entries_shown = 0, entries_waiting = 0;// published as a unit / matched in part, waiting for the rest
    uint64_t over_budget = 0;                       // entries not loaded: the memory budget was reached
    uint64_t loaded_bytes = 0;
};
MaterialStats GetMaterialStats();
int FormatMaterialStatus(char* out, unsigned long n); // "native11_materials{...}"

} // namespace na::native11
