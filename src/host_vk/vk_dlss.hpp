// DLSS on the game's own Vulkan device (NGX Vulkan), recorded into the game's
// command buffer by the Vulkan layer. Game-agnostic: it takes images and the
// same FrameRecord the DX11 bridge uses (src/bridge/frame_record.hpp).
//
// Built for real only with the NGX SDK headers and static library
// (NA_HAVE_NGX_LIB); otherwise vk_dlss_stub.cpp says why it is unavailable.
// NGX gets the NEXT layer's vkGet*ProcAddr, so the commands it records never
// come back through our own hooks.
#pragma once

#include "bridge/frame_record.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace na::vk {

struct DlssImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

struct DlssResources {
    DlssImage color;  // SHADER_READ_ONLY_OPTIMAL
    DlssImage depth;  // SHADER_READ_ONLY_OPTIMAL
    DlssImage motion; // SHADER_READ_ONLY_OPTIMAL, RG32F, render pixels
    DlssImage output; // GENERAL, storage
};

class VkDlss {
public:
    VkDlss();
    ~VkDlss();
    VkDlss(const VkDlss&) = delete;
    VkDlss& operator=(const VkDlss&) = delete;

    // Extensions NGX needs, added by the layer at vkCreateInstance/vkCreateDevice
    // (only those the system has).
    static void RequiredExtensions(std::vector<std::string>* instance_ext, std::vector<std::string>* device_ext);
    static bool BuiltWithDlss();

    // nullptr when DLSS Super Resolution is ready on this device. `dll_dir`: where
    // nvngx_dlss.dll sits (beside our layer), so nothing goes in the game folder.
    const char* Init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                     PFN_vkGetDeviceProcAddr gdpa, const wchar_t* data_path, const wchar_t* dll_dir);
    // Records DLSS on `cmd`. The feature is (re)created when size, mode, preset or
    // flags change (its creation is recorded on `cmd` too). nullptr on success.
    const char* Evaluate(VkCommandBuffer cmd, const DlssResources& r, const na::bridge::FrameRecord& f,
                         const na::bridge::SessionLimits& limits);
    void Shutdown();
    bool Ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace na::vk
