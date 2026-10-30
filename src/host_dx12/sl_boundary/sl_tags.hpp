// Streamline 2.x resource-tag decoding for the game-facing boundary (sl_boundary).
//
// Pure byte decoding with no Windows dependency, so the layout rules are
// covered by the portable CPU tests. Layouts from the public Streamline
// headers (MIT, include/sl_core_types.h, NVIDIA-RTX/Streamline main 625db07):
//
//   sl::BaseStructure  { BaseStructure* next; StructType (16-byte GUID); size_t structVersion; }  32 bytes
//   sl::Resource       : BaseStructure { ResourceType type (char); void* native; void* memory;
//                        void* view; uint32_t state; uint32_t width, height, nativeFormat,
//                        mipLevels, arrayLayers; uint64_t gpuVirtualAddress; ... }
//   sl::ResourceTag    : BaseStructure { Resource* resource; BufferType type (uint32);
//                        ResourceLifecycle lifecycle (enum); Extent extent {top,left,width,height}; }  64 bytes
//
// Every element is validated by its GUID before any field is used: a layout
// this code does not recognise is reported as undecoded, never guessed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace na::slb::abi {

constexpr size_t kHeaderSize = 32;
constexpr size_t kTagSize = 64;          // sizeof(sl::ResourceTag), array stride
constexpr size_t kResourceReadSize = 88; // through arrayLayers (fields used here)

// {4C6A5AAD-B445-496C-87FF-1AF3845BE653} sl::ResourceTag
constexpr uint8_t kResourceTagGuid[16] = {0xad, 0x5a, 0x6a, 0x4c, 0x45, 0xb4, 0x6c, 0x49,
                                          0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53};
// {3A9D70CF-2418-4B72-8391-13F8721C7261} sl::Resource
constexpr uint8_t kResourceGuid[16] = {0xcf, 0x70, 0x9d, 0x3a, 0x18, 0x24, 0x72, 0x4b,
                                       0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61};

// sl::ResourceLifecycle
constexpr uint32_t kOnlyValidNow = 0;
constexpr uint32_t kValidUntilPresent = 1;
constexpr uint32_t kValidUntilEvaluate = 2;

// sl::BufferType values used by DLSS-G and NR (sl_core_types.h).
constexpr uint32_t kBufferDepth = 0;
constexpr uint32_t kBufferMotionVectors = 1;
constexpr uint32_t kBufferHudlessColor = 2;
constexpr uint32_t kBufferScalingInputColor = 3;
constexpr uint32_t kBufferScalingOutputColor = 4;
constexpr uint32_t kBufferUiColorAndAlpha = 23;
constexpr uint32_t kBufferBidirectionalDistortion = 50;
constexpr uint32_t kBufferBackbuffer = 53;

// One decoded tag. `resource` is the raw sl::Resource::native pointer value
// (an ID3D12Resource* for D3D12); 0 means the game untagged this buffer type.
struct RawTag {
    uint32_t buffer_type = 0;
    uint32_t lifecycle = 0;
    uint64_t resource = 0;
    uint32_t resource_type = 0; // sl::ResourceType (0 = eTex2d)
    uint32_t state = 0xFFFFFFFFu; // D3D12_RESOURCE_STATES the game declares (UINT_MAX = not given)
    uint32_t width = 0, height = 0, native_format = 0;
    uint32_t extent_top = 0, extent_left = 0, extent_width = 0, extent_height = 0;
    bool resource_decoded = false; // the sl::Resource block was present and recognised
};

inline bool HasGuid(const uint8_t* header, size_t len, const uint8_t (&guid)[16])
{
    return header && len >= kHeaderSize && std::memcmp(header + 8, guid, 16) == 0;
}

template <class T> inline T Read(const uint8_t* p, size_t offset)
{
    T v{};
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

// Decodes one ResourceTag element (`tag`, kTagSize bytes). `resource` is the
// copied sl::Resource the tag points at (kResourceReadSize bytes) or null when
// the pointer was null/unreadable. Returns false if the tag is not an
// sl::ResourceTag of the known layout.
inline bool DecodeTag(const uint8_t* tag, size_t tag_len, const uint8_t* resource, size_t resource_len, RawTag& out)
{
    out = RawTag{};
    if (tag_len < kTagSize || !HasGuid(tag, tag_len, kResourceTagGuid)) return false;
    out.buffer_type = Read<uint32_t>(tag, 40);
    out.lifecycle = Read<uint32_t>(tag, 44);
    out.extent_top = Read<uint32_t>(tag, 48);
    out.extent_left = Read<uint32_t>(tag, 52);
    out.extent_width = Read<uint32_t>(tag, 56);
    out.extent_height = Read<uint32_t>(tag, 60);
    if (out.lifecycle > kValidUntilEvaluate) return false;
    if (Read<uint64_t>(tag, 32) == 0) return true; // untag: resource stays 0
    if (!resource || resource_len < kResourceReadSize || !HasGuid(resource, resource_len, kResourceGuid))
        return true; // tag understood, resource block not (resource_decoded=false)
    out.resource_type = static_cast<uint8_t>(resource[32]);
    out.resource = Read<uint64_t>(resource, 40);
    out.state = Read<uint32_t>(resource, 64);
    out.width = Read<uint32_t>(resource, 68);
    out.height = Read<uint32_t>(resource, 72);
    out.native_format = Read<uint32_t>(resource, 76);
    out.resource_decoded = true;
    return true;
}

// Pointer to the sl::Resource inside a tag element (to be copied by the caller).
inline uint64_t TagResourcePointer(const uint8_t* tag) { return Read<uint64_t>(tag, 32); }

inline const char* BufferName(uint32_t t)
{
    switch (t) {
    case kBufferDepth: return "depth";
    case kBufferMotionVectors: return "motion";
    case kBufferHudlessColor: return "hudless";
    case kBufferScalingInputColor: return "scaling_input";
    case kBufferScalingOutputColor: return "scaling_output";
    case kBufferUiColorAndAlpha: return "ui_color_alpha";
    case kBufferBidirectionalDistortion: return "distortion";
    case kBufferBackbuffer: return "backbuffer";
    default: return "other";
    }
}

inline const char* LifecycleName(uint32_t l)
{
    switch (l) {
    case kOnlyValidNow: return "only_valid_now";
    case kValidUntilPresent: return "valid_until_present";
    case kValidUntilEvaluate: return "valid_until_evaluate";
    default: return "unknown";
    }
}

} // namespace na::slb::abi
