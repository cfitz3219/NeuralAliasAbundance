// Native DX11 route: prepared material textures from the cache into the game (the publisher that
// textures.hpp's swap was built for). The cache is docs/MATERIAL-CACHE-V1.md: one folder per entry,
// named by its entry_key, holding manifest.json and one DDS per map.
//
// How a game texture gets its prepared map:
//  1. At the game's CreateTexture2D: only when the cache holds an entry of this exact shape
//     (format, size, mip count), the creation data is copied (a memory copy) and handed to the
//     loader's thread, which computes the "texture-subresources-v1" digest for the view formats the
//     cache asks about. Empty-created sources of prepared shapes use the existing asynchronous
//     capture after they are bound in the scene: one copy, read on a later Present with DO_NOT_WAIT,
//     then worker hashing. No GPU wait, files or model inference in a game callback.
//  2. At the game's CreateShaderResourceView of a candidate view (texture_policy.hpp): the view
//     waits on, or is matched against, its texture's digest.
//  3. A match names one map of one entry. The loader's thread reads that map's file, checks it
//     against the manifest (header, size, SHA-256), and creates the block-compressed texture.
//  4. An entry is shown as a unit: when every qualified map of it has a live game view and a
//     created texture, all are published in one step, so colour, normal and roughness of a material
//     change on the same Present. Views that arrive later for a shown entry join it at once.
//
// Replacement requires matching source encoding; larger maps also require plain pixel-shader
// sampling. Writes after identification invalidate the source and restore its original views.
// This supports static materials uploaded after creation, not arbitrary mutable texture streaming.
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <vector>

namespace na::native11 {

struct MaterialTextureFacts;

// Reads every entry under `dir` (null: %LOCALAPPDATA%\NeuralAlias\materials) and replaces the
// loaded index, dropping every map made for the previous one (they belong to the device and index
// they were made for). Manifests only; no map file is read until a game texture matches. Textures the game
// created before the call are not matched (their creation data is gone). Returns the entries read.
// Call before the game creates its textures; InstallTextures does, with the default folder.
size_t LoadMaterialCache(const wchar_t* dir);

// From textures.cpp's device hooks, after the game's successful calls.
void MaterialsOnTextureCreated(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& asked, const D3D11_SUBRESOURCE_DATA* initial,
                               ID3D11Texture2D* texture);
bool MaterialsOnViewCreated(ID3D11Resource* resource, ID3D11ShaderResourceView* view, uint64_t serial, const MaterialTextureFacts& facts);
// Reuse the bounded asynchronous source capture for empty-created textures.
bool MaterialsBeginLateRead(ID3D11Resource* resource);
void MaterialsFinishLateRead(ID3D11Resource* resource, std::vector<uint8_t>&& bytes);
void MaterialsOnResourceWritten(ID3D11Resource* resource);

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
