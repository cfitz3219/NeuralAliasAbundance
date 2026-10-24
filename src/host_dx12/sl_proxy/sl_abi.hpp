// C7b: what the sl.interposer proxy may read from Streamline calls, for BOTH
// major ABIs, decoded from bytes the proxy has already copied (guarded).
//
//  SL 1.x (TLOU ships 1.4.7): slSetConstants(const Constants&, uint32 frame,
//    uint32 viewport) -> bool. Constants has no header; its layout is the one
//    verified for TLOU (fg::Camera, 432 bytes).
//  SL 2.x: slSetConstants(const Constants&, const FrameToken&, const
//    ViewportHandle&) -> sl::Result (0 = ok). Every struct starts with
//    sl::BaseStructure {next*, 16-byte type GUID, size_t version}; Constants
//    has the same fields after that header, except 1.x's
//    notRenderingGameFrames flag, which 2.x dropped.
// Field lists from the public Streamline headers (MIT; v1.1.1 vendored, v2.14.16
// read at C7b). A decoded camera is still only recorded if fg::ValidCamera
// accepts it (finite values, inverse matrix pairs), so a misread layout is
// rejected rather than used.
#pragma once

#include "host_dx12/fg/camera.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace na::sl {

enum class Layout : uint32_t { Unknown = 0, V1 = 1, V2 = 2 };

constexpr size_t kHeaderSize = 32;                 // sl::BaseStructure on x64
constexpr size_t kV1ConstantsSize = sizeof(na::fg::Camera);
constexpr size_t kCameraFloatBytes = offsetof(na::fg::Camera, inverted); // matrices .. motionVectorsInvalidValue
constexpr size_t kV2BoolCount = 7;                 // depthInverted .. motionVectorsJittered
constexpr size_t kV2ConstantsSize = kHeaderSize + kCameraFloatBytes + kV2BoolCount;
static_assert(kCameraFloatBytes == 412, "Camera float block");

// {DCD35AD7-4E4A-4BAD-A90C-E0C49EB23AFE} sl::Constants (2.x)
constexpr uint8_t kConstantsGuid[16] = {0xd7, 0x5a, 0xd3, 0xdc, 0x4a, 0x4e, 0xad, 0x4b,
                                        0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe};
// {830A0F35-DB84-4171-A804-59B206499B18} sl::FrameToken (2.x; has a vptr first)
constexpr uint8_t kFrameTokenGuid[16] = {0x35, 0x0f, 0x0a, 0x83, 0x84, 0xdb, 0x71, 0x41,
                                         0xa8, 0x04, 0x59, 0xb2, 0x06, 0x49, 0x9b, 0x18};
// {171B6435-9B3C-4FC8-9994-FBE52569AAA4} sl::ViewportHandle (2.x)
constexpr uint8_t kViewportGuid[16] = {0x35, 0x64, 0x1b, 0x17, 0x3c, 0x9b, 0xc8, 0x4f,
                                       0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4};

// `header` points at a BaseStructure (next pointer first): is its type `guid`?
inline bool HasType(const uint8_t* header, size_t len, const uint8_t (&guid)[16])
{
    return header && len >= kHeaderSize && std::memcmp(header + 8, guid, 16) == 0;
}

// Decodes a copied Constants block. `len` is how many bytes were readable.
inline Layout DecodeConstants(const uint8_t* p, size_t len, na::fg::Camera& out)
{
    out = na::fg::Camera{};
    if (HasType(p, len, kConstantsGuid)) {
        if (len < kV2ConstantsSize) return Layout::Unknown;
        std::memcpy(&out, p + kHeaderSize, kCameraFloatBytes);
        const uint8_t* b = p + kHeaderSize + kCameraFloatBytes;
        out.inverted = b[0];
        out.camera_motion = b[1];
        out.motion_3d = b[2];
        out.reset = b[3];
        out.not_game = 0; // not part of 2.x Constants
        out.ortho = b[4];
        out.dilated = b[5];
        out.jittered = b[6];
        out.extension = nullptr;
        return Layout::V2;
    }
    if (len < kV1ConstantsSize) return Layout::Unknown;
    std::memcpy(&out, p, kV1ConstantsSize);
    out.extension = nullptr;
    return Layout::V1;
}

// SL 2.x ViewportHandle: BaseStructure then uint32 value.
inline bool ViewportValue(const uint8_t* p, size_t len, uint32_t* out)
{
    if (!HasType(p, len, kViewportGuid) || len < kHeaderSize + 4) return false;
    std::memcpy(out, p + kHeaderSize, 4);
    return true;
}

} // namespace na::sl
