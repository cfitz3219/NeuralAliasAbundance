// Built when the NGX static library is not available: the layer still runs
// (copy test, logging, every decision) and says why DLSS is off.
#include "host_vk/vk_dlss.hpp"

namespace na::vk {

struct VkDlss::Impl {};
VkDlss::VkDlss() : impl_(std::make_unique<Impl>()) {}
VkDlss::~VkDlss() = default;
void VkDlss::RequiredExtensions(std::vector<std::string>*, std::vector<std::string>*) {}
bool VkDlss::BuiltWithDlss() { return false; }
const char* VkDlss::Init(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const wchar_t*,
                         const wchar_t*)
{
    return "This build has no DLSS (built without the NVIDIA NGX library); only copy_test=1 works.";
}
const char* VkDlss::Evaluate(VkCommandBuffer, const DlssResources&, const na::bridge::FrameRecord&, const na::bridge::SessionLimits&)
{
    return "This build has no DLSS.";
}
void VkDlss::Shutdown() {}
bool VkDlss::Ready() const { return false; }

} // namespace na::vk
