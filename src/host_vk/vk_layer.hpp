// Vulkan host: the "ears" for Vulkan games, as a Vulkan layer
// (VK_LAYER_NEURALALIAS_host). The Vulkan twin of src/host_dx11: the same
// game-agnostic frame model (host_dx11/frame_model.hpp) and camera pick
// (host_dx11/camera_pick.hpp), fed from what the game records into its command
// buffers, and DLSS recorded into the game's own command buffer, right before
// the game first reads the finished scene colour (the tonemap or post pass).
//
// Why DLSS runs inside the game here (and not in the DX12 helper as for DX11):
// a Vulkan game records the scene and the tonemap into the same command
// buffer, so there is no point between them where another process could take
// the picture and give it back. NGX Vulkan is called with the next layer's
// function pointers, inside the same command buffer, like the DX11 in-process
// backend (src/bridge/dx11_dlss_inproc.cpp).
//
// Nothing here knows any game. Off unless [vulkan] enabled=1 in
// NeuralAliasHost.ini beside the game's exe (or the file named by the
// NEURALALIAS_VK_INI environment variable). Every run writes a log
// (%LOCALAPPDATA%\NeuralAlias\vulkan\NeuralAliasVulkan-<pid>.log by default).
//
// First stage: DLAA (render size = screen size), jitter behind jitter=1.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define NA_VK_EXPORT extern "C" __declspec(dllexport)
#else
#define NA_VK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// What the layer has seen and done (tests read it through NeuralAliasVk_GetStats).
struct NaVkStats {
    uint32_t size;                 // sizeof(NaVkStats), set by the caller
    uint32_t active;               // the ini turned the layer on for this process
    uint32_t selected, camera;     // scene depth/colour chosen; camera found
    uint64_t frames, recorded_inserts, delivered, refused, no_camera, failed;
    uint64_t motion_dispatches, jitter_writes, samplers_biased, submits;
    uint64_t missed, contradictions, tag_not_unique;
    uint64_t jitter_mismatch;      // insertions whose told jitter the scene did not get (should stay 0)
    uint32_t last_draws, last_scene_draws;
    uint32_t color_w, color_h, color_format, depth_format;
    uint64_t camera_offset;        // byte offset of the ViewProjection in its block
    uint32_t depth_reversed, y_down;
    uint32_t dlss_built;           // built with the NGX library
    float jitter_x, jitter_y;      // the last delivered frame's jitter, render pixels (+X right, +Y down)
    char reason[192];
    char log_path[260];
};

// Copies the current stats; returns 1 when `out->size` matched.
NA_VK_EXPORT int NeuralAliasVk_GetStats(NaVkStats* out);
