// Vulkan host layer (see vk_layer.hpp and docs/VULKAN-HOOKS.md).
//
// How a frame flows:
//  1. Recording (any thread, per command buffer): every draw, depth clear and
//     read of an image is turned into a frame-model Event and kept with its
//     command buffer, with the camera candidates bound for that draw (vertex
//     uniform buffers and push constants). A "read" is what Vulkan makes the
//     game say before it reads a colour attachment: a barrier that makes
//     colour-attachment writes visible to reads, a render pass that ends with
//     the image in a read layout, or a copy/blit/resolve from it.
//  2. When a read of the selected scene colour carries the tag the model
//     learned last frame (its place in its command buffer), DLSS is recorded
//     right there, before the game's own command: copy the colour out, build
//     camera motion from depth, DLSS (or the copy test), write the result back,
//     restore the layouts and the game's compute bindings.
//  3. vkQueueSubmit: the submitted command buffers' events are replayed into
//     the model in GPU order (like DX11 command lists). The camera's bytes are
//     read from the game's mapped memory then, jitter (optional) is written
//     into them before the GPU can see them, and the DLSS slot's camera motion
//     matrix is filled in.
//  4. vkQueuePresentKHR: the frame ends; the model re-learns selection, camera
//     candidates and the DLSS point.
#include "host_vk/vk_layer.hpp"

// Vulkan structs are filled the usual way, `T x{VK_STRUCTURE_TYPE_...}`, which
// value-initialises the rest; GCC would warn about every one of them.
#if !defined(_MSC_VER)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include "bridge/frame_record.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx11/camera_pick.hpp"
#include "host_dx11/frame_model.hpp"
#include "host_vk/vk_dlss.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <cstdint>
#include "host_vk/shaders/camera_motion_spv.h"
#include "host_vk/shaders/copy_test_spv.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <type_traits>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <share.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

using na::dx11::CameraPick;
using na::dx11::Event;
using na::dx11::FrameModel;
using na::dx11::Id;
using na::dx11::Kind;
using na::dx11::ResourceInfo;
using na::dx11::Selection;

namespace {

// ================================================================ platform bits
std::string Env(const char* name)
{
#if defined(_WIN32)
    char buf[1024] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n && n < sizeof(buf) ? std::string(buf, n) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

uint32_t Pid()
{
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<uint32_t>(getpid());
#endif
}

std::filesystem::path ExeDir()
{
#if defined(_WIN32)
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

std::filesystem::path ModuleDir()
{
#if defined(_WIN32)
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&ModuleDir), &info) && info.dli_fname) return std::filesystem::path(info.dli_fname).parent_path();
    return {};
#endif
}

std::filesystem::path StateDir()
{
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n && n < MAX_PATH) return std::filesystem::path(buf) / L"NeuralAlias" / L"vulkan";
    return std::filesystem::temp_directory_path() / "NeuralAlias" / "vulkan";
#else
    if (const std::string x = Env("XDG_STATE_HOME"); !x.empty()) return std::filesystem::path(x) / "NeuralAlias" / "vulkan";
    if (const std::string h = Env("HOME"); !h.empty()) return std::filesystem::path(h) / ".local" / "state" / "NeuralAlias" / "vulkan";
    return std::filesystem::temp_directory_path() / "NeuralAlias" / "vulkan";
#endif
}

// ================================================================ config
struct Config {
    bool enabled = false;
    bool jitter = false;        // inject sub-pixel camera jitter (and mip bias)
    float mip_bias = -1.f;      // added to new mip-mapped samplers when jitter is on
    uint32_t jitter_phases = 8;
    uint32_t preset = 12;       // DLSS preset L for DLAA (Cort, 2026-11-04), same as DX11
    uint32_t copy_test = 0;     // 1: copy instead of DLSS; tests: 2 swaps red and blue, 3 shows the motion vectors
    std::filesystem::path log_path;
    std::filesystem::path ini_path;
};

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

Config LoadConfig()
{
    Config c;
    if (const std::string e = Env("NEURALALIAS_VK_INI"); !e.empty()) c.ini_path = std::filesystem::u8path(e);
    else c.ini_path = ExeDir() / "NeuralAliasHost.ini";
    std::ifstream in(c.ini_path);
    if (!in) return c;
    std::string line, section;
    while (std::getline(in, line)) {
        if (const size_t sc = line.find(';'); sc != std::string::npos) line.resize(sc);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = Trim(line.substr(1, line.find(']') - 1));
            for (char& ch : section) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            continue;
        }
        if (section != "vulkan") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        const auto num = [&](double def) { char* end = nullptr; const double d = std::strtod(v.c_str(), &end); return end != v.c_str() ? d : def; };
        if (k == "enabled") c.enabled = num(0) != 0;
        else if (k == "jitter") c.jitter = num(0) != 0;
        else if (k == "mip_bias") c.mip_bias = static_cast<float>(num(-1));
        else if (k == "jitter_phases") c.jitter_phases = static_cast<uint32_t>(std::clamp(num(8), 2.0, 64.0));
        else if (k == "preset") c.preset = static_cast<uint32_t>(std::clamp(num(12), 0.0, 20.0));
        else if (k == "copy_test") c.copy_test = static_cast<uint32_t>(std::clamp(num(0), 0.0, 3.0));
        else if (k == "log" && !v.empty()) c.log_path = std::filesystem::u8path(v);
    }
    return c;
}

Config g_cfg;
std::once_flag g_cfg_once;
const Config& Cfg()
{
    std::call_once(g_cfg_once, [] { g_cfg = LoadConfig(); });
    return g_cfg;
}

// ================================================================ log
std::mutex g_log_mu;
FILE* g_log = nullptr;
std::string g_log_path_utf8;

void OpenLog()
{
    std::lock_guard<std::mutex> l(g_log_mu);
    if (g_log) return;
    std::filesystem::path p = Cfg().log_path;
    if (p.empty()) p = StateDir() / ("NeuralAliasVulkan-" + std::to_string(Pid()) + ".log");
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
#if defined(_WIN32)
    g_log = _wfsopen(p.wstring().c_str(), L"w", _SH_DENYWR); // readable while the game runs
#else
    g_log = std::fopen(p.string().c_str(), "w");
#endif
    g_log_path_utf8 = p.u8string();
}

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> l(g_log_mu);
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// ================================================================ ids
uint64_t Mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    return h;
}
template <class T> uint64_t H(T handle) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)); }
#if !defined(VK_USE_64_BIT_PTR_DEFINES) || VK_USE_64_BIT_PTR_DEFINES == 0
// 32-bit builds: non-dispatchable handles are uint64_t.
inline uint64_t H(uint64_t handle) { return handle; }
#endif
inline Id NonZero(uint64_t v) { return v ? v : 1; }

void* Key(const void* dispatchable) { return *reinterpret_cast<void* const*>(dispatchable); }

bool IsDepthFormat(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default: return false;
    }
}
bool HasStencil(VkFormat f) { return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT; }
bool IsFloatFormat(VkFormat f)
{
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16_SFLOAT;
}

// ================================================================ dispatch tables
#define NA_VK_DEVICE_FNS(X)                                                                                          \
    X(DestroyDevice) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) X(CreateBuffer)           \
    X(DestroyBuffer) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(BindBufferMemory)                 \
    X(BindImageMemory) X(GetImageMemoryRequirements) X(GetBufferMemoryRequirements) X(FlushMappedMemoryRanges)       \
    X(CreateSampler) X(DestroySampler) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer)                 \
    X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline)                    \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)      \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets)                \
    X(FreeDescriptorSets) X(UpdateDescriptorSets) X(CreateShaderModule) X(DestroyShaderModule)                       \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)                        \
    X(ResetCommandBuffer) X(ResetCommandPool) X(DestroyCommandPool) X(CmdBeginRenderPass) X(CmdNextSubpass)          \
    X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport)            \
    X(CmdDraw) X(CmdDrawIndexed) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CmdPipelineBarrier) X(CmdCopyImage)  \
    X(CmdBlitImage) X(CmdResolveImage) X(CmdCopyImageToBuffer) X(CmdClearDepthStencilImage) X(CmdCopyBuffer)         \
    X(CmdUpdateBuffer) X(CmdExecuteCommands) X(CmdDispatch) X(QueueSubmit) X(DeviceWaitIdle)

// Newer core functions, each with the extension name that also provides it.
#define NA_VK_DEVICE_FNS_ALIASED(X)                                                                                  \
    X(BindBufferMemory2, "vkBindBufferMemory2KHR") X(CreateRenderPass2, "vkCreateRenderPass2KHR")                    \
    X(CmdBeginRenderPass2, "vkCmdBeginRenderPass2KHR") X(CmdNextSubpass2, "vkCmdNextSubpass2KHR")                    \
    X(CmdEndRenderPass2, "vkCmdEndRenderPass2KHR") X(CmdBeginRendering, "vkCmdBeginRenderingKHR")                    \
    X(CmdEndRendering, "vkCmdEndRenderingKHR") X(CmdPipelineBarrier2, "vkCmdPipelineBarrier2KHR")                    \
    X(CmdCopyImage2, "vkCmdCopyImage2KHR") X(CmdBlitImage2, "vkCmdBlitImage2KHR")                                    \
    X(CmdResolveImage2, "vkCmdResolveImage2KHR") X(CmdCopyImageToBuffer2, "vkCmdCopyImageToBuffer2KHR")              \
    X(CmdCopyBuffer2, "vkCmdCopyBuffer2KHR") X(QueueSubmit2, "vkQueueSubmit2KHR")                                    \
    X(CmdDrawIndirectCount, "vkCmdDrawIndirectCountKHR") X(CmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCountKHR") \
    X(CreateDescriptorUpdateTemplate, "vkCreateDescriptorUpdateTemplateKHR")                                         \
    X(DestroyDescriptorUpdateTemplate, "vkDestroyDescriptorUpdateTemplateKHR")                                       \
    X(UpdateDescriptorSetWithTemplate, "vkUpdateDescriptorSetWithTemplateKHR")

#define NA_VK_DEVICE_FNS_KHR(X) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(QueuePresentKHR)

struct DeviceDispatch {
#define NA_DECL(n) PFN_vk##n n = nullptr;
#define NA_DECL2(n, alt) PFN_vk##n n = nullptr;
    NA_VK_DEVICE_FNS(NA_DECL)
    NA_VK_DEVICE_FNS_ALIASED(NA_DECL2)
    NA_VK_DEVICE_FNS_KHR(NA_DECL)
#undef NA_DECL
#undef NA_DECL2
};

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr; // next layer's
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    bool active = false;
};

struct Host;
struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr; // next layer's
    DeviceDispatch d;
    VkPhysicalDeviceMemoryProperties mem{};
    VkPhysicalDeviceProperties props{};
    std::unique_ptr<Host> host;
};

// Looked up on every device call, from every thread: a per-thread one-entry
// cache (checked against an epoch that changes whenever a table changes) so the
// common case takes no lock at all; a shared lock otherwise.
std::shared_mutex g_tables_mu;
std::atomic<uint64_t> g_tables_epoch{1};
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;

InstanceData* InstanceOf(const void* dispatchable)
{
    thread_local struct { void* key; InstanceData* v; uint64_t epoch; } c{nullptr, nullptr, 0};
    void* const key = Key(dispatchable);
    const uint64_t epoch = g_tables_epoch.load(std::memory_order_acquire);
    if (c.key == key && c.epoch == epoch) return c.v;
    std::shared_lock<std::shared_mutex> l(g_tables_mu);
    auto it = g_instances.find(key);
    InstanceData* v = it == g_instances.end() ? nullptr : it->second.get();
    c = {key, v, epoch};
    return v;
}
DeviceData* DeviceOf(const void* dispatchable)
{
    thread_local struct { void* key; DeviceData* v; uint64_t epoch; } c{nullptr, nullptr, 0};
    void* const key = Key(dispatchable);
    const uint64_t epoch = g_tables_epoch.load(std::memory_order_acquire);
    if (c.key == key && c.epoch == epoch) return c.v;
    std::shared_lock<std::shared_mutex> l(g_tables_mu);
    auto it = g_devices.find(key);
    DeviceData* v = it == g_devices.end() ? nullptr : it->second.get();
    c = {key, v, epoch};
    return v;
}

// ================================================================ object registries (per process)
struct ImageInfo {
    Id cls = 0;          // the image's "kind" (format, size, samples, mips, layers, attachment use)
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, mips = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0; // as created (with what we added)
    bool depth = false, stencil = false;
    std::atomic<int32_t> layout{VK_IMAGE_LAYOUT_UNDEFINED}; // last layout as recorded (record order)
};
struct ViewInfo { VkImage image = VK_NULL_HANDLE; };
struct RenderPassInfo {
    struct Att { VkFormat format; VkAttachmentLoadOp load; VkImageLayout final_layout; };
    struct Sub { uint32_t color0 = VK_ATTACHMENT_UNUSED, depth = VK_ATTACHMENT_UNUSED; };
    std::vector<Att> atts;
    std::vector<Sub> subs;
};
struct FramebufferInfo { std::vector<VkImageView> views; bool imageless = false; };
struct PipelineInfo { bool dynamic_viewport = false; float viewport_h = 1.f; };
struct SetLayoutInfo {
    struct B { uint32_t binding, count; VkDescriptorType type; VkShaderStageFlags stages; };
    std::vector<B> bindings;
    std::vector<B> dynamic; // dynamic-offset bindings only, in binding order (worked out once, not per bind)
    bool vertex_ubo = false; // has a vertex-visible uniform buffer binding
};
struct SetInfo {
    std::vector<SetLayoutInfo::B> bindings;
    std::vector<SetLayoutInfo::B> dynamic;
    struct Ubo { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, range = 0; bool dynamic = false; };
    std::map<uint64_t, Ubo> ubos; // (binding << 32 | element) -> buffer range (vertex-visible uniform buffers only)
};
struct TemplateInfo {
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    bool for_sets = true;
    bool has_ubo = false; // writes a uniform buffer (the only kind we note)
};
struct MemoryInfo { VkDeviceSize size = 0; bool coherent = false; uint8_t* mapped = nullptr; VkDeviceSize map_offset = 0, map_size = 0; };
struct BufferInfo { VkDeviceSize size = 0; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize mem_offset = 0; };
struct UboRef { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, range = 0; };

std::shared_mutex g_obj_mu;
std::unordered_map<uint64_t, std::unique_ptr<ImageInfo>> g_images;
std::unordered_map<uint64_t, ViewInfo> g_views;
std::unordered_map<uint64_t, RenderPassInfo> g_passes;
std::unordered_map<uint64_t, FramebufferInfo> g_fbs;
std::unordered_map<uint64_t, PipelineInfo> g_pipes;
// Descriptor sets are updated and bound thousands of times a frame from many
// threads, so they do not share g_obj_mu: they live in 64 shards, each with its
// own lock, and a bind or update locks only the one shard its set is in. Layouts
// and templates (rarely created) have their own lock; pool lists theirs.
// Lock order: a set shard, then g_obj_mu (shared) for a buffer's size. Never the other way.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padded on purpose: one cache line per shard
#endif
struct alignas(64) SetShard {
    std::shared_mutex mu;
    std::unordered_map<uint64_t, SetInfo> sets;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
constexpr uint32_t kSetShards = 64;
SetShard g_set_shards[kSetShards];
inline uint32_t SetShardIndex(uint64_t h) { return static_cast<uint32_t>((h * 0x9E3779B97F4A7C15ull) >> 58); }
inline SetShard& SetShardOf(uint64_t h) { return g_set_shards[SetShardIndex(h)]; }
std::shared_mutex g_layout_mu; // g_set_layouts, g_templates
std::unordered_map<uint64_t, SetLayoutInfo> g_set_layouts;
std::unordered_map<uint64_t, TemplateInfo> g_templates;
std::mutex g_pool_mu;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_pool_sets;
std::unordered_map<uint64_t, MemoryInfo> g_mems;
std::unordered_map<uint64_t, BufferInfo> g_buffers;
std::unordered_map<Id, UboRef> g_ubo_ids;                 // camera candidate id -> buffer range
std::unordered_map<Id, uint32_t> g_class_refs;            // image class -> live images

constexpr VkShaderStageFlags kVertexStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                                             VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

// ================================================================ command buffer recording state
constexpr Id kInherit = ~0ull; // secondary draw inside the primary's render pass
constexpr int kMaxSets = 8;
constexpr uint64_t kSecondaryTag = 1ull << 63;

struct RecEvent {
    enum T : uint8_t { Model, Insert, BufCopy, BufUpdate, Exec, Push } t = Model;
    bool y_down = false;          // Model Draw: positive viewport height
    Event ev;                     // Model
    uint32_t a = 0, b = 0;        // Insert: slot; BufUpdate/Push: blob offset, size
    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VkDeviceSize so = 0, dof = 0, size = 0;
    VkCommandBuffer sec = VK_NULL_HANDLE; // Exec
    Id rt = 0, depth = 0;         // Exec: the primary's attachments at that point
};

struct SetBind {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<uint32_t> dyn;
};

struct CbState {
    DeviceData* dev = nullptr;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    bool secondary = false, continue_pass = false;
    // snapshot of the host's decisions at vkBeginCommandBuffer
    Id sel_color = 0, sel_depth = 0;
    uint64_t learned_tag = 0;
    // recording
    std::vector<RecEvent> ev;
    std::vector<uint8_t> blob;
    bool in_pass = false;
    uint32_t passes = 0, color_reads = 0, scene_draws = 0, inserts = 0;
    VkRenderPass rp = VK_NULL_HANDLE;
    uint32_t subpass = 0;
    std::vector<VkImageView> fb_views;
    Id rt_cls = 0, depth_cls = 0;
    VkImage rt_img = VK_NULL_HANDLE, depth_img = VK_NULL_HANDLE;
    std::vector<std::pair<VkImage, VkImageLayout>> pass_finals; // colour attachments' final layouts (render pass end)
    VkImage last_scene_color = VK_NULL_HANDLE, last_scene_depth = VK_NULL_HANDLE;
    // bindings
    VkPipeline gfx_pipe = VK_NULL_HANDLE, comp_pipe = VK_NULL_HANDLE;
    bool pipe_dynamic_viewport = true;
    float pipe_viewport_h = 1.f, dyn_viewport_h = 1.f;
    SetBind gfx_sets[kMaxSets], comp_sets[kMaxSets];
    std::vector<Id> gfx_ubo_ids[kMaxSets];
    struct PushRange { VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t offset; std::vector<uint8_t> bytes; };
    std::vector<PushRange> pushes;
    Id push_id = 0;
    std::vector<uint8_t> last_push;

    void Reset()
    {
        ev.clear();
        blob.clear();
        in_pass = false;
        passes = color_reads = scene_draws = inserts = 0;
        rp = VK_NULL_HANDLE;
        subpass = 0;
        fb_views.clear();
        rt_cls = depth_cls = 0;
        rt_img = depth_img = VK_NULL_HANDLE;
        pass_finals.clear();
        last_scene_color = last_scene_depth = VK_NULL_HANDLE;
        gfx_pipe = comp_pipe = VK_NULL_HANDLE;
        pipe_dynamic_viewport = true;
        pipe_viewport_h = dyn_viewport_h = 1.f;
        for (auto& s : gfx_sets) s = SetBind{};
        for (auto& s : comp_sets) s = SetBind{};
        for (auto& v : gfx_ubo_ids) v.clear();
        pushes.clear();
        push_id = 0;
        last_push.clear();
        continue_pass = false;
    }
    bool YDown() const { return (pipe_dynamic_viewport ? dyn_viewport_h : pipe_viewport_h) > 0.f; }
};

std::mutex g_cb_mu;
std::unordered_map<uint64_t, std::unique_ptr<CbState>> g_cbs;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_pool_cbs;
std::atomic<uint64_t> g_cb_epoch{1};
struct CbCache { VkCommandBuffer cb = VK_NULL_HANDLE; CbState* st = nullptr; uint64_t epoch = 0; };
thread_local CbCache t_cb;

CbState* Cb(VkCommandBuffer cb)
{
    const uint64_t epoch = g_cb_epoch.load(std::memory_order_acquire);
    if (t_cb.cb == cb && t_cb.epoch == epoch) return t_cb.st;
    std::lock_guard<std::mutex> l(g_cb_mu);
    auto it = g_cbs.find(H(cb));
    CbState* st = it == g_cbs.end() ? nullptr : it->second.get();
    if (st) t_cb = {cb, st, epoch}; // only hits are cached, so allocating command buffers never invalidates the caches
    return st;
}

ImageInfo* ImageOf(VkImage img) // under g_obj_mu (shared)
{
    auto it = g_images.find(H(img));
    return it == g_images.end() ? nullptr : it->second.get();
}
VkImage ImageOfView(VkImageView v) // under g_obj_mu (shared)
{
    auto it = g_views.find(H(v));
    return it == g_views.end() ? VK_NULL_HANDLE : it->second.image;
}

// ================================================================ the host (one per device that presents)
constexpr uint32_t kSlots = 64;       // DLSS insertions in flight (camera-motion parameters ring)
constexpr uint32_t kSlotStride = 256; // >= minUniformBufferOffsetAlignment everywhere
constexpr size_t kMaxCandidates = 6;
constexpr size_t kMaxCamBytes = 4096;

struct SlotParams { // std140, matches camera_motion.comp
    float reproject[16];
    float jitter[2];
    float depth_clear;
    uint32_t flags;
    uint32_t size[2];
    uint32_t pad[2];
};
static_assert(sizeof(SlotParams) == 96, "camera motion parameter block");

struct Slot {
    uint64_t frame = 0;
    double jx = 0, jy = 0; // injected jitter, render pixels (+Y down); 0 when not injected
    bool jittered = false, y_down = true;
    uint32_t w = 0, h = 0;
};

struct OwnImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct Host {
    DeviceData* dev = nullptr;
    std::mutex mu;
    FrameModel model;
    NaVkStats stats{};
    uint64_t frame = 0;
    uint32_t swap_w = 0, swap_h = 0;
    // camera
    CameraPick camera;
    bool camera_known = false;
    na::camera::CameraTracker tracker;
    bool tracker_reset = true, reset_pending = true;
    std::unordered_map<Id, std::vector<uint8_t>> push_bytes;   // latest push-constant bytes per id (replayed)
    std::unordered_map<Id, std::vector<uint8_t>> update_bytes; // vkCmdUpdateBuffer bytes per candidate id this frame
    struct CopySrc { VkBuffer src; VkDeviceSize src_off, dst_off, size; };
    std::unordered_map<uint64_t, std::vector<CopySrc>> copies; // dst buffer -> copies into it (this and last frame)
    std::unordered_map<Id, std::vector<uint8_t>> written;      // camera bytes as we last wrote them (jitter)
    std::unordered_map<Id, std::vector<uint8_t>> unjittered;   // ... and as the game had them
    bool jitter_tried_this_frame = false, jitter_written_this_frame = false;
    bool scene_y_down = true;
    // DLSS slots
    Slot slots[kSlots];
    uint32_t next_slot = 0;
    uint64_t insert_counter = 0;
    std::deque<uint32_t> pending; // recorded insertions not yet submitted (oldest first)
    double inj_jx = 0, inj_jy = 0; // the jitter actually written into the game's camera this frame
    std::atomic<VkImage> last_scene_color{VK_NULL_HANDLE}, last_scene_depth{VK_NULL_HANDLE};
    // The selection, readable without h.mu (vkBeginCommandBuffer runs on every recording thread).
    std::atomic<Id> pub_color{0}, pub_depth{0};
    std::atomic<uint64_t> pub_tag{0};
    // resources
    bool init_tried = false, init_ok = false;
    VkShaderModule motion_sm = VK_NULL_HANDLE, copy_sm = VK_NULL_HANDLE;
    VkDescriptorSetLayout motion_dsl = VK_NULL_HANDLE, copy_dsl = VK_NULL_HANDLE;
    VkPipelineLayout motion_pl = VK_NULL_HANDLE, copy_pl = VK_NULL_HANDLE;
    VkPipeline motion_pipe = VK_NULL_HANDLE, copy_pipe = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ring_mem = VK_NULL_HANDLE;
    uint8_t* ring_ptr = nullptr;
    bool ring_coherent = true;
    // per size/format
    uint32_t res_w = 0, res_h = 0;
    VkFormat res_color_format = VK_FORMAT_UNDEFINED;
    OwnImage input, motion, output;
    VkDescriptorSet copy_set = VK_NULL_HANDLE;
    struct DepthSet { VkImageView view = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; };
    std::unordered_map<uint64_t, DepthSet> depth_sets;
    bool resources_ok = false;
    // Resources of an old size: command buffers recorded (or still running) with
    // them may exist, so they are freed some frames later, never in place.
    struct Retired { OwnImage input, motion, output; VkDescriptorSet copy_set = VK_NULL_HANDLE; std::vector<DepthSet> depth; uint64_t free_at = 0; };
    std::vector<Retired> retired;
    // DLSS
    na::vk::VkDlss dlss;
    bool dlss_tried = false, dlss_ok = false;
    // log
    char last_logged_reason[192] = {};
    Selection logged_sel;
    bool logged_camera = false;

    void SetReason(const char* r)
    {
        std::snprintf(stats.reason, sizeof(stats.reason), "%s", r);
        if (std::strcmp(last_logged_reason, r) != 0) {
            std::snprintf(last_logged_reason, sizeof(last_logged_reason), "%s", r);
            Log("skip: %s", r);
        }
    }
};

Host* HostOf(DeviceData* dd) { return dd ? dd->host.get() : nullptr; }

// Under h.mu: makes the current selection visible to vkBeginCommandBuffer.
void Publish(Host& h)
{
    const Selection& s = h.model.Current();
    h.pub_color.store(s.color, std::memory_order_relaxed);
    h.pub_depth.store(s.depth, std::memory_order_relaxed);
    h.pub_tag.store(h.model.LearnedTag(), std::memory_order_release);
}

// ---------------------------------------------------------------- memory helpers
uint32_t FindMemoryType(const DeviceData& dd, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < dd.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (dd.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

bool FormatHas(const DeviceData& dd, VkFormat f, VkFormatFeatureFlags want)
{
    if (!dd.inst || !dd.inst->GetPhysicalDeviceFormatProperties) return false;
    VkFormatProperties p{};
    dd.inst->GetPhysicalDeviceFormatProperties(dd.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

bool MakeImage(DeviceData& dd, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, OwnImage* out)
{
    const DeviceDispatch& d = dd.d;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.CreateImage(dd.device, &ci, nullptr, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    d.GetImageMemoryRequirements(dd.device, out->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = FindMemoryType(dd, req.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == ~0u || d.AllocateMemory(dd.device, &ai, nullptr, &out->mem) != VK_SUCCESS) return false;
    if (d.BindImageMemory(dd.device, out->image, out->mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (d.CreateImageView(dd.device, &vi, nullptr, &out->view) != VK_SUCCESS) return false;
    out->format = f;
    return true;
}
void FreeImage(DeviceData& dd, OwnImage* i)
{
    if (i->view) dd.d.DestroyImageView(dd.device, i->view, nullptr);
    if (i->image) dd.d.DestroyImage(dd.device, i->image, nullptr);
    if (i->mem) dd.d.FreeMemory(dd.device, i->mem, nullptr);
    *i = OwnImage{};
}

// Pipelines, sampler, descriptor pool and the parameter ring (once per device).
bool InitResources(Host& h)
{
    if (h.init_tried) return h.init_ok;
    h.init_tried = true;
    DeviceData& dd = *h.dev;
    const DeviceDispatch& d = dd.d;
    VkDevice dev = dd.device;
    auto module = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        return d.CreateShaderModule(dev, &ci, nullptr, out) == VK_SUCCESS;
    };
    if (!module(kCameraMotionSpv, sizeof(kCameraMotionSpv), &h.motion_sm) || !module(kCopyTestSpv, sizeof(kCopyTestSpv), &h.copy_sm)) {
        h.SetReason("Could not create the layer's shaders.");
        return false;
    }
    {
        VkDescriptorSetLayoutBinding b[3] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = b;
        if (d.CreateDescriptorSetLayout(dev, &ci, nullptr, &h.motion_dsl) != VK_SUCCESS) return false;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; // copy test: input, output, motion (mode 3)
        if (d.CreateDescriptorSetLayout(dev, &ci, nullptr, &h.copy_dsl) != VK_SUCCESS) return false;
    }
    {
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &h.motion_dsl;
        if (d.CreatePipelineLayout(dev, &ci, nullptr, &h.motion_pl) != VK_SUCCESS) return false;
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
        ci.pSetLayouts = &h.copy_dsl;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        if (d.CreatePipelineLayout(dev, &ci, nullptr, &h.copy_pl) != VK_SUCCESS) return false;
    }
    auto pipeline = [&](VkShaderModule sm, VkPipelineLayout pl, VkPipeline* out) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
        ci.layout = pl;
        return d.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, out) == VK_SUCCESS;
    };
    if (!pipeline(h.motion_sm, h.motion_pl, &h.motion_pipe) || !pipeline(h.copy_sm, h.copy_pl, &h.copy_pipe)) {
        h.SetReason("Could not create the layer's compute pipelines.");
        return false;
    }
    {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = 0.f;
        if (d.CreateSampler(dev, &ci, nullptr, &h.sampler) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ci.maxSets = 64;
        ci.poolSizeCount = 3;
        ci.pPoolSizes = sizes;
        if (d.CreateDescriptorPool(dev, &ci, nullptr, &h.pool) != VK_SUCCESS) return false;
    }
    {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = kSlots * kSlotStride;
        ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (d.CreateBuffer(dev, &ci, nullptr, &h.ring) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        d.GetBufferMemoryRequirements(dev, h.ring, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == ~0u) {
            ai.memoryTypeIndex = FindMemoryType(dd, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            h.ring_coherent = false;
        }
        if (ai.memoryTypeIndex == ~0u || d.AllocateMemory(dev, &ai, nullptr, &h.ring_mem) != VK_SUCCESS) return false;
        if (d.BindBufferMemory(dev, h.ring, h.ring_mem, 0) != VK_SUCCESS) return false;
        void* p = nullptr;
        if (d.MapMemory(dev, h.ring_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
        h.ring_ptr = static_cast<uint8_t*>(p);
        std::memset(h.ring_ptr, 0, kSlots * kSlotStride);
    }
    h.init_ok = true;
    return true;
}

constexpr uint64_t kRetireFrames = 8; // far more frames than any game keeps in flight

void FreeRetired(Host& h, Host::Retired& r)
{
    DeviceData& dd = *h.dev;
    for (auto& ds : r.depth) {
        if (ds.set) dd.d.FreeDescriptorSets(dd.device, h.pool, 1, &ds.set);
        if (ds.view) dd.d.DestroyImageView(dd.device, ds.view, nullptr);
    }
    if (r.copy_set) dd.d.FreeDescriptorSets(dd.device, h.pool, 1, &r.copy_set);
    FreeImage(dd, &r.input);
    FreeImage(dd, &r.motion);
    FreeImage(dd, &r.output);
}

// Frees retired resources whose time has come (all of them when `all`, only once the device is idle).
void ReleaseRetired(Host& h, bool all)
{
    for (auto it = h.retired.begin(); it != h.retired.end();) {
        if (all || h.frame >= it->free_at) {
            FreeRetired(h, *it);
            it = h.retired.erase(it);
        } else ++it;
    }
}

// Retires the per-size resources. No device wait here: this runs inside the
// game's command recording, and vkDeviceWaitIdle is not allowed while another
// game thread may be submitting (and would stall the game).
void FreeSizedResources(Host& h)
{
    Host::Retired r;
    r.input = h.input;
    r.motion = h.motion;
    r.output = h.output;
    r.copy_set = h.copy_set;
    for (auto& kv : h.depth_sets) r.depth.push_back(kv.second);
    r.free_at = h.frame + kRetireFrames;
    if (r.input.image || r.motion.image || r.output.image || r.copy_set || !r.depth.empty()) h.retired.push_back(std::move(r));
    h.depth_sets.clear();
    h.copy_set = VK_NULL_HANDLE;
    h.input = h.motion = h.output = OwnImage{};
    h.resources_ok = false;
    h.res_w = h.res_h = 0;
    h.res_color_format = VK_FORMAT_UNDEFINED;
}

bool EnsureSizedResources(Host& h, uint32_t w, uint32_t hgt, VkFormat color_format)
{
    if (h.resources_ok && h.res_w == w && h.res_h == hgt && h.res_color_format == color_format) return true;
    FreeSizedResources(h);
    DeviceData& dd = *h.dev;
    if (!FormatHas(dd, color_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        h.SetReason("The scene colour's format cannot be copied and sampled on this GPU.");
        return false;
    }
    if (!FormatHas(dd, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !FormatHas(dd, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        h.SetReason("This GPU cannot write the layer's motion or output images.");
        return false;
    }
    if (color_format != VK_FORMAT_R16G16B16A16_SFLOAT && !FormatHas(dd, color_format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        h.SetReason("The scene colour's format cannot be written back by a blit on this GPU.");
        return false;
    }
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!MakeImage(dd, w, hgt, color_format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &h.input) ||
        !MakeImage(dd, w, hgt, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, &h.motion) ||
        !MakeImage(dd, w, hgt, VK_FORMAT_R16G16B16A16_SFLOAT, rw, &h.output)) {
        FreeSizedResources(h);
        h.SetReason("Could not create the layer's images for this size.");
        return false;
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = h.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &h.copy_dsl;
    if (dd.d.AllocateDescriptorSets(dd.device, &ai, &h.copy_set) != VK_SUCCESS) { FreeSizedResources(h); return false; }
    VkDescriptorImageInfo in{h.sampler, h.input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, h.output.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mot{h.sampler, h.motion.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w2[3] = {};
    w2[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, h.copy_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, nullptr, nullptr};
    w2[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, h.copy_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out, nullptr, nullptr};
    w2[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, h.copy_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mot, nullptr, nullptr};
    dd.d.UpdateDescriptorSets(dd.device, 3, w2, 0, nullptr);
    h.res_w = w;
    h.res_h = hgt;
    h.res_color_format = color_format;
    h.resources_ok = true;
    Log("resources: %ux%u, colour format %d, motion RG32F, output RGBA16F", w, hgt, static_cast<int>(color_format));
    return true;
}

// The camera-motion descriptor set for one depth image (made once, never changed while in use).
VkDescriptorSet DepthSetFor(Host& h, VkImage depth, VkFormat fmt)
{
    auto it = h.depth_sets.find(H(depth));
    if (it != h.depth_sets.end()) return it->second.set;
    if (h.depth_sets.size() >= 16) return VK_NULL_HANDLE; // the pool is small: a game rotating more depth buffers than this is refused
    DeviceData& dd = *h.dev;
    Host::DepthSet ds;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = depth;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (dd.d.CreateImageView(dd.device, &vi, nullptr, &ds.view) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = h.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &h.motion_dsl;
    if (dd.d.AllocateDescriptorSets(dd.device, &ai, &ds.set) != VK_SUCCESS) {
        dd.d.DestroyImageView(dd.device, ds.view, nullptr);
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo din{h.sampler, ds.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo mout{VK_NULL_HANDLE, h.motion.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo ub{h.ring, 0, sizeof(SlotParams)};
    VkWriteDescriptorSet w[3] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &din, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds.set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mout, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds.set, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &ub, nullptr};
    dd.d.UpdateDescriptorSets(dd.device, 3, w, 0, nullptr);
    h.depth_sets[H(depth)] = ds;
    return ds.set;
}

// ---------------------------------------------------------------- camera bytes
// Host pointer of a buffer range, if the game can write it from the CPU (mapped
// directly, or the source of a buffer copy into it this frame). Under g_obj_mu (shared).
uint8_t* HostBytes(Host& h, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkDeviceMemory* mem_out, VkDeviceSize* mem_off_out)
{
    for (int hop = 0; hop < 2; ++hop) {
        auto bt = g_buffers.find(H(buf));
        if (bt == g_buffers.end()) return nullptr;
        auto mt = g_mems.find(H(bt->second.mem));
        if (mt != g_mems.end() && mt->second.mapped) {
            const VkDeviceSize at = bt->second.mem_offset + off;
            if (at >= mt->second.map_offset && at + size <= mt->second.map_offset + mt->second.map_size) {
                *mem_out = bt->second.mem;
                *mem_off_out = at;
                return mt->second.mapped + (at - mt->second.map_offset);
            }
        }
        // Device-local: follow the latest copy that covers the range.
        auto ct = h.copies.find(H(buf));
        if (ct == h.copies.end()) return nullptr;
        bool moved = false;
        for (auto c = ct->second.rbegin(); c != ct->second.rend(); ++c) {
            if (off >= c->dst_off && off + size <= c->dst_off + c->size) {
                off = c->src_off + (off - c->dst_off);
                buf = c->src;
                moved = true;
                break;
            }
        }
        if (!moved) return nullptr;
    }
    return nullptr;
}

bool CandidateBytes(Host& h, Id id, std::vector<uint8_t>* out) // under h.mu
{
    if (auto pt = h.push_bytes.find(id); pt != h.push_bytes.end()) { *out = pt->second; return true; }
    if (auto ut = h.update_bytes.find(id); ut != h.update_bytes.end()) { *out = ut->second; return true; }
    std::shared_lock<std::shared_mutex> l(g_obj_mu);
    auto it = g_ubo_ids.find(id);
    if (it == g_ubo_ids.end()) return false;
    const VkDeviceSize n = std::min<VkDeviceSize>(it->second.range, kMaxCamBytes);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize mo = 0;
    const uint8_t* p = HostBytes(h, it->second.buffer, it->second.offset, n, &mem, &mo);
    if (!p) return false;
    out->assign(p, p + n);
    return true;
}

// The camera for this frame (under h.mu). Re-reads the learned block, else searches the candidates.
bool CameraNow(Host& h, CameraPick* out)
{
    const Selection& s = h.model.Current();
    std::vector<uint8_t> bytes;
    if (s.camera_cb && h.camera.found) {
        if (CandidateBytes(h, s.camera_cb, &bytes)) {
            if (na::dx11::CheckLearned(bytes, h.camera, out)) return true;
            const CameraPick p = na::dx11::FindCamera(bytes, h.frame + 1);
            if (p.found) { h.camera = p; *out = p; return true; }
        }
        Log("camera: the learned camera block no longer holds a camera; searching again");
        h.camera = CameraPick{};
        h.model.SetCamera(0, -1);
    }
    for (const auto& c : h.model.CameraCandidates(kMaxCandidates)) {
        if (!CandidateBytes(h, c.first, &bytes)) continue;
        const CameraPick p = na::dx11::FindCamera(bytes, h.frame + 1);
        if (!p.found) continue;
        h.camera = p;
        h.model.SetCamera(c.first, c.second);
        h.tracker_reset = true;
        Log("camera: found in %s (slot %d) at byte %llu (%s, inverse %+lld bytes, %s depth)",
            c.second == 0 ? "push constants" : "a vertex uniform buffer", c.second, static_cast<unsigned long long>(p.offset),
            p.layout == na::camera::Layout::RowMajor ? "row-major" : "column-major", static_cast<long long>(p.inverse_delta),
            p.reversed ? "reversed" : "standard");
        *out = p;
        return true;
    }
    return false;
}

// Can the learned camera block be rewritten from the CPU (mapped, or copied from mapped memory)? Under h.mu.
bool CameraWritable(Host& h)
{
    const Selection& s = h.model.Current();
    if (!s.camera_cb || !h.camera.found) return false;
    std::shared_lock<std::shared_mutex> l(g_obj_mu);
    auto it = g_ubo_ids.find(s.camera_cb);
    if (it == g_ubo_ids.end()) return false; // push constants / inline updates: not rewritten (see docs)
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize mo = 0;
    return HostBytes(h, it->second.buffer, it->second.offset, std::min<VkDeviceSize>(it->second.range, kMaxCamBytes), &mem, &mo) != nullptr;
}

// The jitter the next DLSS insertion will be recorded with (used when the scene
// is submitted before that insertion is recorded). Under h.mu.
Slot NextSlot(Host& h, bool y_down)
{
    Slot s;
    const Selection& sel = h.model.Current();
    s.w = sel.color_info.width;
    s.h = sel.color_info.height;
    s.y_down = y_down;
    const auto js = neural_alias::temporal_jitter(static_cast<uint32_t>(h.insert_counter), Cfg().jitter_phases);
    s.jx = js.x_pixels;
    s.jy = js.y_pixels;
    s.jittered = true;
    return s;
}

// Jitter: rewrites the main camera's ViewProjection (and inverse) in the game's
// mapped memory at submit, before the GPU can read it. Under h.mu.
void InjectJitter(Host& h, const Slot& slot)
{
    const Selection& s = h.model.Current();
    if (!s.camera_cb || !h.camera.found || !slot.jittered || !slot.w || !slot.h) return;
    std::shared_lock<std::shared_mutex> l(g_obj_mu);
    auto it = g_ubo_ids.find(s.camera_cb);
    if (it == g_ubo_ids.end()) return; // push constants / inline updates: not rewritten (see docs)
    const VkDeviceSize n = std::min<VkDeviceSize>(it->second.range, kMaxCamBytes);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize mo = 0;
    uint8_t* p = HostBytes(h, it->second.buffer, it->second.offset, n, &mem, &mo);
    if (!p) return;
    std::vector<uint8_t> now(p, p + n);
    // The game did not rewrite the block since our last write: start from its own bytes.
    auto wt = h.written.find(s.camera_cb);
    if (wt != h.written.end() && wt->second == now) now = h.unjittered[s.camera_cb];
    CameraPick cam;
    if (!na::dx11::CheckLearned(now, h.camera, &cam)) return;
    na::camera::VpGeometry geo{};
    if (!na::camera::DecomposeViewProjection(cam.vp, geo)) return;
    const double aspect = geo.sy / geo.sx, screen = double(slot.w) / slot.h;
    if (std::fabs(aspect - screen) > 0.01 * screen) return; // not the main camera
    // jitter_projection works in D3D's +Y-up clip space; Vulkan's own is +Y down.
    const neural_alias::JitterSample js{slot.jx, slot.y_down ? -slot.jy : slot.jy};
    const auto jittered = neural_alias::jitter_projection(cam.vp, js, slot.w, slot.h);
    na::camera::Matrix inv{};
    if (!na::camera::Invert(jittered, inv)) return;
    std::vector<uint8_t> out = now;
    na::dx11::StoreMatrix(out.data(), out.size(), cam.offset, cam.layout, jittered);
    const int64_t io = static_cast<int64_t>(cam.offset) + cam.inverse_delta;
    if (io >= 0) na::dx11::StoreMatrix(out.data(), out.size(), static_cast<uint64_t>(io), cam.layout, inv);
    std::memcpy(p, out.data(), out.size());
    auto mt = g_mems.find(H(mem));
    if (mt != g_mems.end() && !mt->second.coherent) {
        const VkDeviceSize atom = std::max<VkDeviceSize>(1, h.dev->props.limits.nonCoherentAtomSize);
        VkDeviceSize start = (mo / atom) * atom, end = ((mo + n + atom - 1) / atom) * atom;
        start = std::max(start, mt->second.map_offset);
        end = std::min(end, mt->second.map_offset + mt->second.map_size);
        VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, mem, start, end - start};
        if (mt->second.map_offset + mt->second.map_size == mt->second.size && end == mt->second.size) r.size = VK_WHOLE_SIZE;
        h.dev->d.FlushMappedMemoryRanges(h.dev->device, 1, &r);
    }
    h.unjittered[s.camera_cb] = std::move(now);
    h.written[s.camera_cb] = std::move(out);
    h.jitter_written_this_frame = true;
    h.inj_jx = slot.jx;
    h.inj_jy = slot.jy;
    ++h.stats.jitter_writes;
}

// ---------------------------------------------------------------- recording DLSS into the game's command buffer
void Barrier(const DeviceDispatch& d, VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             std::vector<VkImageMemoryBarrier>* out)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    out->push_back(b);
    (void)d;
    (void)cb;
}
void Flush(const DeviceDispatch& d, VkCommandBuffer cb, std::vector<VkImageMemoryBarrier>* bs)
{
    if (bs->empty()) return;
    d.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(bs->size()), bs->data());
    bs->clear();
}

// NGX Vulkan, once per device. Under h.mu. Tried when the swap chain is made
// (outside any frame), so the first DLSS frame does not pay for it.
void TryInitDlss(Host& h)
{
    if (h.dlss_tried || Cfg().copy_test) return;
    h.dlss_tried = true;
    DeviceData& dd = *h.dev;
    const std::filesystem::path data = StateDir() / "ngx";
    std::error_code ec;
    std::filesystem::create_directories(data, ec);
    const std::wstring data_w = data.wstring(), dll_w = ModuleDir().wstring();
    if (const char* why = h.dlss.Init(dd.inst->instance, dd.phys, dd.device, dd.inst->gipa, dd.gdpa, data_w.c_str(), dll_w.c_str())) {
        Log("dlss: %s", why);
        h.SetReason(why);
    } else {
        h.dlss_ok = true;
        Log("dlss: NGX Vulkan ready");
    }
}

// Records DLSS (or the copy test) right before the game's read of `color`.
// `color_layout` is the colour's exact layout at this point. Returns true if recorded.
bool RecordInsert(CbState& cs, VkImage color, VkImageLayout color_layout)
{
    DeviceData& dd = *cs.dev;
    Host* hp = HostOf(&dd);
    if (!hp) return false;
    Host& h = *hp;
    const DeviceDispatch& d = dd.d;
    std::lock_guard<std::mutex> l(h.mu);
    ++h.stats.recorded_inserts;
    const Selection sel = h.model.Current();
    VkImage depth = cs.last_scene_depth ? cs.last_scene_depth : h.last_scene_depth.load();
    VkFormat cfmt = VK_FORMAT_UNDEFINED, dfmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, hh = 0;
    VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool color_ok = false, depth_ok = false, depth_stencil = false;
    {
        std::shared_lock<std::shared_mutex> ol(g_obj_mu);
        const ImageInfo* ci = ImageOf(color);
        const ImageInfo* di = depth ? ImageOf(depth) : nullptr;
        if (ci && ci->samples == VK_SAMPLE_COUNT_1_BIT && (ci->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (ci->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            color_ok = true;
            cfmt = ci->format;
            w = ci->w;
            hh = ci->h;
        }
        if (di && di->samples == VK_SAMPLE_COUNT_1_BIT && (di->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && di->w == w && di->h == hh) {
            depth_ok = true;
            dfmt = di->format;
            depth_stencil = di->stencil;
            depth_layout = static_cast<VkImageLayout>(di->layout.load());
        }
    }
    if (!color_ok) { ++h.stats.refused; h.SetReason("The scene colour is multisampled or could not be given copy access (it was made before the layer saw it)."); return false; }
    if (!depth_ok) { ++h.stats.refused; h.SetReason("The scene depth is not known here, is multisampled, or cannot be sampled."); return false; }
    if (color_layout == VK_IMAGE_LAYOUT_UNDEFINED || depth_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        ++h.stats.refused;
        h.SetReason("The layout of the scene colour or depth is not known at the DLSS point.");
        return false;
    }
    if (!h.camera_known) { ++h.stats.no_camera; h.SetReason("No camera matrix (with its inverse) was found in the scene's uniform buffers or push constants yet."); return false; }
    if (!InitResources(h) || !EnsureSizedResources(h, w, hh, cfmt)) { ++h.stats.failed; return false; }
    const VkDescriptorSet dset = DepthSetFor(h, depth, dfmt);
    if (!dset) { ++h.stats.refused; h.SetReason("Could not bind the scene depth for camera motion."); return false; }
    const bool copy_test = Cfg().copy_test != 0;
    if (!copy_test && !h.dlss_ok) {
        TryInitDlss(h); // normally done already when the swap chain was made
        if (!h.dlss_ok) { ++h.stats.failed; return false; }
    }

    // The slot: this insertion's jitter and size; its camera motion is filled at submit.
    const uint32_t si = h.next_slot++ % kSlots;
    Slot& slot = h.slots[si];
    slot = Slot{};
    slot.frame = h.frame;
    slot.w = w;
    slot.h = hh;
    slot.y_down = h.scene_y_down;
    // Jitter only when it can really be written into the scene's camera: DLSS must
    // never be told a jitter the picture does not have.
    const bool jitter = Cfg().jitter && h.camera_known && CameraWritable(h);
    if (jitter) {
        const auto js = neural_alias::temporal_jitter(static_cast<uint32_t>(h.insert_counter), Cfg().jitter_phases);
        slot.jx = js.x_pixels;
        slot.jy = js.y_pixels;
        slot.jittered = true;
    }
    ++h.insert_counter;
    {
        SlotParams p{};
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(p.reproject, id, sizeof(id));
        p.size[0] = w;
        p.size[1] = hh;
        std::memcpy(h.ring_ptr + si * kSlotStride, &p, sizeof(p)); // zero motion until the submit fills it
    }
    h.pending.push_back(si);
    if (h.pending.size() > kSlots / 2) h.pending.pop_front();

    // ---- commands
    const VkImageAspectFlags daspect = VK_IMAGE_ASPECT_DEPTH_BIT | (depth_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    std::vector<VkImageMemoryBarrier> bs;
    Barrier(d, cs.cb, color, VK_IMAGE_ASPECT_COLOR_BIT, color_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &bs);
    if (depth_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        Barrier(d, cs.cb, depth, daspect, depth_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &bs);
    Barrier(d, cs.cb, h.input.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bs);
    Barrier(d, cs.cb, h.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, &bs);
    Barrier(d, cs.cb, h.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, &bs);
    Flush(d, cs.cb, &bs);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {w, hh, 1};
    d.CmdCopyImage(cs.cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, h.input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    Barrier(d, cs.cb, h.input.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &bs);
    Flush(d, cs.cb, &bs);

    // Camera motion.
    d.CmdBindPipeline(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, h.motion_pipe);
    const uint32_t dyn = si * kSlotStride;
    d.CmdBindDescriptorSets(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, h.motion_pl, 0, 1, &dset, 1, &dyn);
    d.CmdDispatch(cs.cb, (w + 7) / 8, (hh + 7) / 8, 1);
    ++h.stats.motion_dispatches;
    Barrier(d, cs.cb, h.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &bs);
    Flush(d, cs.cb, &bs);

    bool ok = true;
    if (copy_test) {
        d.CmdBindPipeline(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, h.copy_pipe);
        d.CmdBindDescriptorSets(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, h.copy_pl, 0, 1, &h.copy_set, 0, nullptr);
        const uint32_t pc[3] = {w, hh, Cfg().copy_test};
        d.CmdPushConstants(cs.cb, h.copy_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        d.CmdDispatch(cs.cb, (w + 7) / 8, (hh + 7) / 8, 1);
    } else {
        na::bridge::FrameRecord rec;
        rec.source_frame = h.frame;
        rec.render_w = rec.output_w = w;
        rec.render_h = rec.output_h = hh;
        rec.mode = static_cast<uint32_t>(na::bridge::Mode::DLAA);
        rec.preset = Cfg().preset;
        rec.jitter_x = static_cast<float>(slot.jx);
        rec.jitter_y = static_cast<float>(slot.jy);
        rec.flags = na::bridge::kCameraMotion | na::bridge::kAutoExposure;
        if (IsFloatFormat(cfmt)) rec.flags |= na::bridge::kHdr;
        if (h.camera.reversed) rec.flags |= na::bridge::kDepthInverted;
        if (h.reset_pending) rec.flags |= na::bridge::kReset;
        na::vk::DlssResources r;
        r.color = {h.input.image, h.input.view, cfmt, w, hh, VK_IMAGE_ASPECT_COLOR_BIT};
        r.depth = {depth, h.depth_sets[H(depth)].view, dfmt, w, hh, VK_IMAGE_ASPECT_DEPTH_BIT};
        r.motion = {h.motion.image, h.motion.view, VK_FORMAT_R32G32_SFLOAT, w, hh, VK_IMAGE_ASPECT_COLOR_BIT};
        r.output = {h.output.image, h.output.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, hh, VK_IMAGE_ASPECT_COLOR_BIT};
        if (const char* why = h.dlss.Evaluate(cs.cb, r, rec, na::bridge::SessionLimits{w, hh, w, hh})) {
            ++h.stats.failed;
            h.SetReason(why);
            ok = false;
        }
    }
    h.reset_pending = false;

    // Write back (only when DLSS recorded), then restore the game's layouts.
    Barrier(d, cs.cb, h.output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &bs);
    Barrier(d, cs.cb, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bs);
    Flush(d, cs.cb, &bs);
    if (ok) {
        if (cfmt == VK_FORMAT_R16G16B16A16_SFLOAT) {
            d.CmdCopyImage(cs.cb, h.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        } else {
            VkImageBlit bl{};
            bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.srcOffsets[1] = bl.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(hh), 1};
            d.CmdBlitImage(cs.cb, h.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                           VK_FILTER_NEAREST);
        }
    } else {
        // Nothing to write: the colour goes back unchanged (copy of itself from input).
        d.CmdCopyImage(cs.cb, h.input.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    Barrier(d, cs.cb, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, color_layout, &bs);
    if (depth_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        Barrier(d, cs.cb, depth, daspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_layout, &bs);
    Flush(d, cs.cb, &bs);

    // The game's compute bindings and push constants, as they were.
    if (cs.comp_pipe) d.CmdBindPipeline(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, cs.comp_pipe);
    for (uint32_t i = 0; i < kMaxSets; ++i) {
        const SetBind& b = cs.comp_sets[i];
        if (b.set) d.CmdBindDescriptorSets(cs.cb, VK_PIPELINE_BIND_POINT_COMPUTE, b.layout, i, 1, &b.set, static_cast<uint32_t>(b.dyn.size()), b.dyn.data());
    }
    for (const auto& p : cs.pushes)
        d.CmdPushConstants(cs.cb, p.layout, p.stages, p.offset, static_cast<uint32_t>(p.bytes.size()), p.bytes.data());

    RecEvent e;
    e.t = RecEvent::Insert;
    e.a = si;
    cs.ev.push_back(e);
    ++cs.inserts;
    (void)sel;
    return true;
}

// ---------------------------------------------------------------- recording: events
void AddModel(CbState& cs, const Event& e, bool y_down = false)
{
    RecEvent r;
    r.t = RecEvent::Model;
    r.ev = e;
    r.y_down = y_down;
    cs.ev.push_back(r);
}

// The game is about to read `img` (a barrier, end of a render pass, or a copy source).
// `layout`: its layout at this point. `kind` tells the read sites apart in the tag.
void OnRead(CbState& cs, VkImage img, VkImageLayout layout, uint32_t kind)
{
    Id cls = 0;
    {
        std::shared_lock<std::shared_mutex> l(g_obj_mu);
        if (const ImageInfo* ii = ImageOf(img)) cls = ii->cls;
    }
    if (!cls || cls != cs.sel_color) return; // only the selected scene colour's reads matter
    Host* h = HostOf(cs.dev);
    const VkImage scene = cs.last_scene_color ? cs.last_scene_color : (h ? h->last_scene_color.load() : VK_NULL_HANDLE);
    uint64_t tag = Mix(Mix(Mix(Mix(0x51ab, cs.color_reads), cs.passes), cs.scene_draws ? 1 : 0), Mix(kind, img == scene ? 1 : 0));
    tag = NonZero(cs.secondary ? (tag | kSecondaryTag) : (tag & ~kSecondaryTag));
    ++cs.color_reads;
    if (!cs.secondary && !cs.in_pass && !cs.inserts && cs.learned_tag && tag == cs.learned_tag && img == scene && h)
        RecordInsert(cs, img, layout);
    Event e;
    e.kind = Kind::Read;
    e.resource = cls;
    e.tag = tag;
    AddModel(cs, e);
}

bool IsReadLayout(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
}
bool IsWriteLayout(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_GENERAL;
}
constexpr VkAccessFlags2 kReadAccess = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

// One image barrier: is it "colour writes become visible to a read"?
bool BarrierIsRead(VkImageLayout from, VkImageLayout to, VkAccessFlags2 src, VkAccessFlags2 dst)
{
    if (from == VK_IMAGE_LAYOUT_UNDEFINED || from == VK_IMAGE_LAYOUT_PREINITIALIZED) return false;
    if (from != to) return IsWriteLayout(from) && (IsReadLayout(to) || to == VK_IMAGE_LAYOUT_GENERAL);
    return (src & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT)) && (dst & kReadAccess);
}

void SetLayout(VkImage img, VkImageLayout l)
{
    std::shared_lock<std::shared_mutex> lk(g_obj_mu);
    if (ImageInfo* ii = ImageOf(img)) ii->layout.store(l);
}

// Current subpass attachments from the render pass + framebuffer views.
void LoadSubpass(CbState& cs)
{
    cs.rt_cls = cs.depth_cls = 0;
    cs.rt_img = cs.depth_img = VK_NULL_HANDLE;
    std::shared_lock<std::shared_mutex> l(g_obj_mu);
    auto pt = g_passes.find(H(cs.rp));
    if (pt == g_passes.end() || cs.subpass >= pt->second.subs.size()) return;
    const auto& sub = pt->second.subs[cs.subpass];
    auto img_of = [&](uint32_t att) -> VkImage {
        if (att == VK_ATTACHMENT_UNUSED || att >= cs.fb_views.size()) return VK_NULL_HANDLE;
        return ImageOfView(cs.fb_views[att]);
    };
    cs.rt_img = img_of(sub.color0);
    cs.depth_img = img_of(sub.depth);
    if (const ImageInfo* i = cs.rt_img ? ImageOf(cs.rt_img) : nullptr) cs.rt_cls = i->cls;
    if (const ImageInfo* i = cs.depth_img ? ImageOf(cs.depth_img) : nullptr) cs.depth_cls = i->cls;
}

void BeginPassCommon(CbState& cs, VkRenderPass rp, VkFramebuffer fb, const void* pnext)
{
    cs.in_pass = true;
    ++cs.passes;
    cs.rp = rp;
    cs.subpass = 0;
    cs.fb_views.clear();
    cs.pass_finals.clear();
    std::vector<std::pair<VkImage, VkAttachmentLoadOp>> clears;
    {
        std::shared_lock<std::shared_mutex> l(g_obj_mu);
        auto ft = g_fbs.find(H(fb));
        if (ft != g_fbs.end()) {
            if (ft->second.imageless) {
                for (auto* p = static_cast<const VkBaseInStructure*>(pnext); p; p = p->pNext)
                    if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                        const auto* ab = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
                        cs.fb_views.assign(ab->pAttachments, ab->pAttachments + ab->attachmentCount);
                    }
            } else cs.fb_views = ft->second.views;
        }
        auto pt = g_passes.find(H(rp));
        if (pt != g_passes.end()) {
            for (size_t i = 0; i < pt->second.atts.size() && i < cs.fb_views.size(); ++i) {
                const VkImage img = ImageOfView(cs.fb_views[i]);
                const ImageInfo* ii = img ? ImageOf(img) : nullptr;
                if (!ii) continue;
                if (ii->depth) clears.push_back({img, pt->second.atts[i].load});
                else cs.pass_finals.push_back({img, pt->second.atts[i].final_layout});
            }
        }
    }
    LoadSubpass(cs);
    for (const auto& c : clears) {
        if (c.second != VK_ATTACHMENT_LOAD_OP_CLEAR) continue;
        std::shared_lock<std::shared_mutex> l(g_obj_mu);
        const ImageInfo* ii = ImageOf(c.first);
        if (!ii) continue;
        Event e;
        e.kind = Kind::ClearDepth;
        e.resource = ii->cls;
        AddModel(cs, e);
    }
}

void EndPassCommon(CbState& cs)
{
    cs.in_pass = false;
    std::vector<std::pair<VkImage, VkImageLayout>> finals;
    finals.swap(cs.pass_finals);
    // Final layouts of every attachment (depth too, for the depth's layout at the DLSS point).
    {
        std::shared_lock<std::shared_mutex> l(g_obj_mu);
        auto pt = g_passes.find(H(cs.rp));
        if (pt != g_passes.end())
            for (size_t i = 0; i < pt->second.atts.size() && i < cs.fb_views.size(); ++i) {
                const VkImage img = ImageOfView(cs.fb_views[i]);
                if (ImageInfo* ii = img ? ImageOf(img) : nullptr) ii->layout.store(pt->second.atts[i].final_layout);
            }
    }
    cs.rp = VK_NULL_HANDLE;
    cs.rt_cls = cs.depth_cls = 0;
    cs.rt_img = cs.depth_img = VK_NULL_HANDLE;
    for (const auto& f : finals)
        if (IsReadLayout(f.second)) OnRead(cs, f.first, f.second, 2);
}

void OnDraw(CbState& cs, uint64_t vertices, bool indirect)
{
    Event e;
    e.kind = Kind::Draw;
    if (cs.secondary && cs.continue_pass && !cs.rt_cls && !cs.depth_cls) e.rt0 = e.depth = kInherit;
    else { e.rt0 = cs.rt_cls; e.depth = cs.depth_cls; }
    e.depth_test = e.depth != 0;
    e.vertices = vertices;
    e.indirect = indirect;
    int k = 0;
    e.vs_cb[k++] = cs.push_id;
    for (int s = 0; s < kMaxSets && k < na::dx11::kCbSlots; ++s)
        for (Id id : cs.gfx_ubo_ids[s]) {
            if (k >= na::dx11::kCbSlots) break;
            e.vs_cb[k++] = id;
        }
    if (cs.sel_color && e.rt0 == cs.sel_color && e.depth == cs.sel_depth) {
        ++cs.scene_draws;
        cs.last_scene_color = cs.rt_img;
        cs.last_scene_depth = cs.depth_img;
        if (Host* h = HostOf(cs.dev)) {
            h->last_scene_color.store(cs.rt_img);
            h->last_scene_depth.store(cs.depth_img);
        }
    }
    AddModel(cs, e, cs.YDown());
}

// ---------------------------------------------------------------- submit: replay into the model
void Replay(Host& h, CbState& cs, Id inherit_rt, Id inherit_depth, int depth_guard) // under h.mu
{
    if (depth_guard > 2) return;
    for (const RecEvent& r : cs.ev) {
        switch (r.t) {
        case RecEvent::Model: {
            Event e = r.ev;
            if (e.kind == Kind::Draw && e.rt0 == kInherit) { e.rt0 = inherit_rt; e.depth = inherit_depth; }
            const bool scene = e.kind == Kind::Draw && h.model.IsSceneDraw(e);
            if (scene) {
                h.scene_y_down = r.y_down;
                // Jitter goes in before the first scene draw the GPU will run this frame.
                if (!h.jitter_tried_this_frame && Cfg().jitter && h.camera.found) {
                    h.jitter_tried_this_frame = true; // once per frame, even if it could not be written
                    // The insertion already recorded for this frame, or (scene submitted
                    // before the post-processing is recorded) the one that will be next.
                    InjectJitter(h, h.pending.empty() ? NextSlot(h, r.y_down) : h.slots[h.pending.front()]);
                }
            }
            const uint32_t v = h.model.Feed(e);
            (void)v;
            break;
        }
        case RecEvent::Push: {
            const Id id = r.ev.resource;
            h.push_bytes[id].assign(cs.blob.begin() + r.a, cs.blob.begin() + r.a + r.b);
            Event e;
            e.kind = Kind::CbUpdate;
            e.resource = id;
            h.model.Feed(e);
            break;
        }
        case RecEvent::BufUpdate: {
            // Bytes the game wrote inline; kept for every candidate id that lies inside.
            std::shared_lock<std::shared_mutex> l(g_obj_mu);
            for (const auto& kv : g_ubo_ids) {
                const UboRef& u = kv.second;
                if (u.buffer != r.dst || u.offset < r.dof || u.offset >= r.dof + r.size) continue;
                const VkDeviceSize n = std::min<VkDeviceSize>(u.range, r.dof + r.size - u.offset);
                const size_t at = r.a + static_cast<size_t>(u.offset - r.dof);
                h.update_bytes[kv.first].assign(cs.blob.begin() + at, cs.blob.begin() + at + static_cast<size_t>(std::min<VkDeviceSize>(n, kMaxCamBytes)));
                Event e;
                e.kind = Kind::CbUpdate;
                e.resource = kv.first;
                h.model.Feed(e);
            }
            break;
        }
        case RecEvent::BufCopy: {
            auto& v = h.copies[H(r.dst)];
            if (v.size() > 256) v.erase(v.begin(), v.begin() + 128);
            v.push_back({r.src, r.so, r.dof, r.size});
            break;
        }
        case RecEvent::Exec: {
            CbState* sec = nullptr;
            {
                std::lock_guard<std::mutex> l(g_cb_mu);
                auto it = g_cbs.find(H(r.sec));
                if (it != g_cbs.end()) sec = it->second.get();
            }
            if (sec) Replay(h, *sec, r.rt, r.depth, depth_guard + 1);
            break;
        }
        case RecEvent::Insert: {
            h.model.MarkFired();
            Slot& slot = h.slots[r.a];
            while (!h.pending.empty() && h.pending.front() != r.a) h.pending.pop_front();
            if (!h.pending.empty()) h.pending.pop_front();
            SlotParams p{};
            const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(p.reproject, id, sizeof(id));
            p.size[0] = slot.w;
            p.size[1] = slot.h;
            p.jitter[0] = static_cast<float>(slot.jx);
            p.jitter[1] = static_cast<float>(slot.jy);
            CameraPick cam;
            if (CameraNow(h, &cam)) {
                h.stats.camera = 1;
                h.stats.camera_offset = cam.offset;
                h.stats.depth_reversed = cam.reversed;
                if (h.tracker_reset) h.tracker.Reset();
                // What the scene really got (the tracker removes exactly that from the camera).
                const double injected[2] = {h.inj_jx, slot.y_down ? -h.inj_jy : h.inj_jy};
                const na::camera::CameraFrame f = h.tracker.Update(cam.vp, h.frame, slot.w, slot.h, h.jitter_written_this_frame ? injected : nullptr);
                if (slot.jittered && !(h.jitter_written_this_frame && h.inj_jx == slot.jx && h.inj_jy == slot.jy)) {
                    ++h.stats.jitter_mismatch;
                    if (h.stats.jitter_mismatch <= 5 || h.stats.jitter_mismatch % 600 == 0)
                        Log("jitter: DLSS was told a jitter the scene did not get (%llu times; %s)", static_cast<unsigned long long>(h.stats.jitter_mismatch),
                            h.jitter_written_this_frame ? "a different sample was written" : "nothing was written this frame");
                }
                if (f.valid) {
                    const auto m = na::camera::ComposeReprojectionRow(f.inv_view_proj_current_row, f.view_proj_previous_row);
                    for (int i = 0; i < 16; ++i) p.reproject[i] = static_cast<float>(m[i]);
                    p.depth_clear = cam.reversed ? 0.f : 1.f;
                    p.flags = 2u | (slot.y_down ? 1u : 0u);
                    if (h.tracker_reset || f.camera_cut) h.reset_pending = true; // DLSS's reset is recorded with the next insertion
                    h.tracker_reset = false;
                    ++h.stats.delivered;
                    h.stats.jitter_x = p.jitter[0];
                    h.stats.jitter_y = p.jitter[1];
                    h.stats.reason[0] = 0;
                    h.last_logged_reason[0] = 0;
                } else {
                    ++h.stats.no_camera;
                    h.SetReason("The camera matrix could not be inverted.");
                }
            } else {
                ++h.stats.no_camera;
                h.SetReason("No camera matrix was found at submit; this frame's DLSS ran with zero camera motion.");
            }
            std::memcpy(h.ring_ptr + r.a * kSlotStride, &p, sizeof(p));
            if (!h.ring_coherent) {
                VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, h.ring_mem, 0, VK_WHOLE_SIZE};
                h.dev->d.FlushMappedMemoryRanges(h.dev->device, 1, &mr);
            }
            break;
        }
        }
    }
}

void OnSubmit(DeviceData& dd, const std::vector<VkCommandBuffer>& cbs)
{
    Host* h = HostOf(&dd);
    if (!h) return;
    std::lock_guard<std::mutex> l(h->mu);
    ++h->stats.submits;
    for (VkCommandBuffer c : cbs) {
        CbState* cs = nullptr;
        {
            std::lock_guard<std::mutex> l2(g_cb_mu);
            auto it = g_cbs.find(H(c));
            if (it != g_cbs.end()) cs = it->second.get();
        }
        if (cs) Replay(*h, *cs, 0, 0, 0);
    }
}

void LogFrame(Host& h)
{
    const uint64_t f = h.frame;
    if (!(f <= 600 ? f % 60 == 0 : f % 600 == 0)) return;
    const auto& s = h.model.GetStats();
    Log("frame %llu: draws %u, scene draws %u, colour reads %u, learned tag %s; inserted %llu, delivered %llu, refused %llu, "
        "no camera %llu, failed %llu, missed %llu, contradictions %llu, not unique %llu, camera share %.2f, jitter writes %llu, jitter mismatch %llu",
        static_cast<unsigned long long>(f), s.last_draws, s.last_scene_draws, s.last_reads, h.model.LearnedTag() ? "yes" : "no",
        static_cast<unsigned long long>(h.stats.recorded_inserts), static_cast<unsigned long long>(h.stats.delivered),
        static_cast<unsigned long long>(h.stats.refused), static_cast<unsigned long long>(h.stats.no_camera),
        static_cast<unsigned long long>(h.stats.failed), static_cast<unsigned long long>(s.missed),
        static_cast<unsigned long long>(s.contradictions), static_cast<unsigned long long>(s.tag_not_unique), s.camera_share,
        static_cast<unsigned long long>(h.stats.jitter_writes), static_cast<unsigned long long>(h.stats.jitter_mismatch));
}

void OnPresent(DeviceData& dd)
{
    Host* hp = HostOf(&dd);
    if (!hp) return;
    Host& h = *hp;
    std::lock_guard<std::mutex> l(h.mu);
    ++h.frame;
    h.model.EndFrame();
    const Selection& s = h.model.Current();
    if (s.depth != h.logged_sel.depth || s.color != h.logged_sel.color) {
        if (s.valid())
            Log("selection: scene colour %ux%u format %u, depth %ux%u format %u", s.color_info.width, s.color_info.height,
                s.color_info.format, s.depth_info.width, s.depth_info.height, s.depth_info.format);
        else Log("selection: none");
        h.logged_sel = s;
        h.camera = CameraPick{};
        h.camera_known = false;
        h.tracker_reset = h.reset_pending = true;
    }
    // Camera search with this frame's bytes (still in the game's memory).
    if (s.valid() && !h.camera.found) {
        CameraPick cam;
        if (CameraNow(h, &cam)) h.camera_known = true;
    }
    h.camera_known = h.camera.found;
    if (h.camera.found != h.logged_camera) {
        h.logged_camera = h.camera.found;
        if (!h.camera.found) Log("camera: none");
    }
    h.update_bytes.clear();
    h.jitter_tried_this_frame = h.jitter_written_this_frame = false;
    ReleaseRetired(h, false);
    Publish(h);
    h.stats.frames = h.frame;
    h.stats.selected = s.valid();
    h.stats.camera = h.camera.found;
    h.stats.color_w = s.color_info.width;
    h.stats.color_h = s.color_info.height;
    h.stats.color_format = s.color_info.format;
    h.stats.depth_format = s.depth_info.format;
    h.stats.y_down = h.scene_y_down;
    const auto& ms = h.model.GetStats();
    h.stats.last_draws = ms.last_draws;
    h.stats.last_scene_draws = ms.last_scene_draws;
    h.stats.missed = ms.missed;
    h.stats.contradictions = ms.contradictions;
    h.stats.tag_not_unique = ms.tag_not_unique;
    LogFrame(h);
}

// ================================================================ hooks: objects
#define NA_DEV(obj) DeviceData* dd = DeviceOf(obj)

VKAPI_ATTR VkResult VKAPI_CALL CreateImage_h(VkDevice device, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out)
{
    NA_DEV(device);
    VkImageCreateInfo c = *ci;
    bool external = false;
    for (auto* p = static_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) external = true;
    // Copy access on colour attachments and sampling on depth, so DLSS can read and write them.
    if (!external && c.tiling == VK_IMAGE_TILING_OPTIMAL && c.imageType == VK_IMAGE_TYPE_2D) {
        if ((c.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && FormatHas(*dd, c.format, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
            c.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((c.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && c.samples == VK_SAMPLE_COUNT_1_BIT &&
            FormatHas(*dd, c.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            c.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    VkResult r = dd->d.CreateImage(device, &c, a, out);
    if (r != VK_SUCCESS && c.usage != ci->usage) { c.usage = ci->usage; r = dd->d.CreateImage(device, &c, a, out); }
    if (r != VK_SUCCESS) return r;
    if (!(ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return r;
    auto info = std::make_unique<ImageInfo>();
    info->format = c.format;
    info->w = c.extent.width;
    info->h = c.extent.height;
    info->mips = c.mipLevels;
    info->layers = c.arrayLayers;
    info->samples = c.samples;
    info->usage = c.usage;
    info->depth = IsDepthFormat(c.format);
    info->stencil = HasStencil(c.format);
    info->layout.store(c.initialLayout);
    uint64_t cls = Mix(Mix(Mix(0xc1a55, c.format), (uint64_t(c.extent.width) << 32) | c.extent.height),
                       Mix(Mix(c.samples, c.mipLevels), Mix(c.arrayLayers, ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT))));
    info->cls = NonZero(cls & ~kSecondaryTag);
    ResourceInfo ri;
    ri.width = c.extent.width;
    ri.height = c.extent.height;
    ri.format = static_cast<uint32_t>(c.format);
    ri.samples = static_cast<uint32_t>(c.samples);
    ri.depth = info->depth;
    const Id id = info->cls;
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_images[H(*out)] = std::move(info);
        ++g_class_refs[id];
    }
    if (Host* h = HostOf(dd)) {
        std::lock_guard<std::mutex> l(h->mu);
        h->model.SetInfo(id, ri);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage_h(VkDevice device, VkImage image, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    Id forget = 0;
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        auto it = g_images.find(H(image));
        if (it != g_images.end()) {
            const Id cls = it->second->cls;
            if (--g_class_refs[cls] == 0) { g_class_refs.erase(cls); forget = cls; }
            g_images.erase(it);
        }
    }
    if (Host* h = HostOf(dd)) {
        std::lock_guard<std::mutex> l(h->mu);
        if (forget) {
            h->model.Forget(forget);
            Publish(*h);
        }
        if (h->last_scene_color.load() == image) h->last_scene_color.store(VK_NULL_HANDLE);
        if (h->last_scene_depth.load() == image) h->last_scene_depth.store(VK_NULL_HANDLE);
        auto ds = h->depth_sets.find(H(image));
        if (ds != h->depth_sets.end()) {
            // No device wait: our view is only used inside the game's own command
            // buffers next to the image itself, and the game destroys the image only
            // once the GPU is done with those.
            if (ds->second.set) dd->d.FreeDescriptorSets(device, h->pool, 1, &ds->second.set);
            dd->d.DestroyImageView(device, ds->second.view, nullptr);
            h->depth_sets.erase(ds);
        }
    }
    dd->d.DestroyImage(device, image, a);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView_h(VkDevice device, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateImageView(device, ci, a, out);
    if (r == VK_SUCCESS) {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_views[H(*out)] = ViewInfo{ci->image};
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyImageView_h(VkDevice device, VkImageView v, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_views.erase(H(v));
    }
    dd->d.DestroyImageView(device, v, a);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer_h(VkDevice device, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateBuffer(device, ci, a, out);
    if (r == VK_SUCCESS) {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_buffers[H(*out)] = BufferInfo{ci->size, VK_NULL_HANDLE, 0};
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyBuffer_h(VkDevice device, VkBuffer b, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_buffers.erase(H(b));
    }
    if (Host* h = HostOf(dd)) {
        std::lock_guard<std::mutex> l(h->mu);
        h->copies.erase(H(b));
    }
    dd->d.DestroyBuffer(device, b, a);
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory_h(VkDevice device, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* a, VkDeviceMemory* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.AllocateMemory(device, ai, a, out);
    if (r == VK_SUCCESS) {
        MemoryInfo m;
        m.size = ai->allocationSize;
        if (ai->memoryTypeIndex < dd->mem.memoryTypeCount)
            m.coherent = (dd->mem.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_mems[H(*out)] = m;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL FreeMemory_h(VkDevice device, VkDeviceMemory m, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_mems.erase(H(m));
    }
    dd->d.FreeMemory(device, m, a);
}
VKAPI_ATTR VkResult VKAPI_CALL MapMemory_h(VkDevice device, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags f, void** pp)
{
    NA_DEV(device);
    const VkResult r = dd->d.MapMemory(device, m, off, size, f, pp);
    if (r == VK_SUCCESS) {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        auto it = g_mems.find(H(m));
        if (it != g_mems.end()) {
            it->second.mapped = static_cast<uint8_t*>(*pp);
            it->second.map_offset = off;
            it->second.map_size = size == VK_WHOLE_SIZE ? it->second.size - off : size;
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL UnmapMemory_h(VkDevice device, VkDeviceMemory m)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        auto it = g_mems.find(H(m));
        if (it != g_mems.end()) it->second.mapped = nullptr;
    }
    dd->d.UnmapMemory(device, m);
}
void NoteBind(VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
    std::unique_lock<std::shared_mutex> l(g_obj_mu);
    auto it = g_buffers.find(H(b));
    if (it != g_buffers.end()) { it->second.mem = m; it->second.mem_offset = off; }
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory_h(VkDevice device, VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
    NA_DEV(device);
    const VkResult r = dd->d.BindBufferMemory(device, b, m, off);
    if (r == VK_SUCCESS) NoteBind(b, m, off);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2_h(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos)
{
    NA_DEV(device);
    const VkResult r = dd->d.BindBufferMemory2(device, n, infos);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; ++i) NoteBind(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSampler_h(VkDevice device, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks* a, VkSampler* out)
{
    NA_DEV(device);
    Host* h = HostOf(dd);
    if (h && Cfg().jitter && Cfg().mip_bias != 0.f && (ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR || ci->anisotropyEnable) && ci->maxLod > 0.f) {
        VkSamplerCreateInfo c = *ci;
        c.mipLodBias += Cfg().mip_bias;
        const VkResult r = dd->d.CreateSampler(device, &c, a, out);
        if (r == VK_SUCCESS) {
            std::lock_guard<std::mutex> l(h->mu);
            ++h->stats.samplers_biased;
        }
        return r;
    }
    return dd->d.CreateSampler(device, ci, a, out);
}

template <class AttDesc, class SubDesc>
void NoteRenderPass(VkRenderPass rp, uint32_t na, const AttDesc* atts, uint32_t ns, const SubDesc* subs)
{
    RenderPassInfo info;
    for (uint32_t i = 0; i < na; ++i) {
        info.atts.push_back({atts[i].format, atts[i].loadOp, atts[i].finalLayout});
    }
    for (uint32_t i = 0; i < ns; ++i) {
        RenderPassInfo::Sub s;
        if (subs[i].colorAttachmentCount && subs[i].pColorAttachments) s.color0 = subs[i].pColorAttachments[0].attachment;
        if (subs[i].pDepthStencilAttachment) s.depth = subs[i].pDepthStencilAttachment->attachment;
        info.subs.push_back(s);
    }
    std::unique_lock<std::shared_mutex> l(g_obj_mu);
    g_passes[H(rp)] = std::move(info);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass_h(VkDevice device, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateRenderPass(device, ci, a, out);
    if (r == VK_SUCCESS) NoteRenderPass(*out, ci->attachmentCount, ci->pAttachments, ci->subpassCount, ci->pSubpasses);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2_h(VkDevice device, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateRenderPass2(device, ci, a, out);
    if (r == VK_SUCCESS) NoteRenderPass(*out, ci->attachmentCount, ci->pAttachments, ci->subpassCount, ci->pSubpasses);
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyRenderPass_h(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_passes.erase(H(rp));
    }
    dd->d.DestroyRenderPass(device, rp, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer_h(VkDevice device, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateFramebuffer(device, ci, a, out);
    if (r == VK_SUCCESS) {
        FramebufferInfo f;
        f.imageless = (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
        if (!f.imageless && ci->pAttachments) f.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_fbs[H(*out)] = std::move(f);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer_h(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_fbs.erase(H(fb));
    }
    dd->d.DestroyFramebuffer(device, fb, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines_h(VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                         const VkAllocationCallbacks* a, VkPipeline* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateGraphicsPipelines(device, cache, n, ci, a, out);
    if (r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        for (uint32_t i = 0; i < n; ++i) {
            if (!out[i]) continue;
            PipelineInfo p;
            if (const auto* ds = ci[i].pDynamicState)
                for (uint32_t k = 0; k < ds->dynamicStateCount; ++k)
                    if (ds->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT || ds->pDynamicStates[k] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT)
                        p.dynamic_viewport = true;
            if (!p.dynamic_viewport && ci[i].pViewportState && ci[i].pViewportState->pViewports && ci[i].pViewportState->viewportCount)
                p.viewport_h = ci[i].pViewportState->pViewports[0].height;
            g_pipes[H(out[i])] = p;
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline_h(VkDevice device, VkPipeline p, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_obj_mu);
        g_pipes.erase(H(p));
    }
    dd->d.DestroyPipeline(device, p, a);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout_h(VkDevice device, const VkDescriptorSetLayoutCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkDescriptorSetLayout* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateDescriptorSetLayout(device, ci, a, out);
    if (r == VK_SUCCESS) {
        SetLayoutInfo s;
        for (uint32_t i = 0; i < ci->bindingCount; ++i) {
            const SetLayoutInfo::B b{ci->pBindings[i].binding, ci->pBindings[i].descriptorCount, ci->pBindings[i].descriptorType, ci->pBindings[i].stageFlags};
            s.bindings.push_back(b);
            if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) s.dynamic.push_back(b);
            if ((b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) && (b.stages & kVertexStages))
                s.vertex_ubo = true;
        }
        std::sort(s.dynamic.begin(), s.dynamic.end(), [](const auto& x, const auto& y) { return x.binding < y.binding; });
        std::unique_lock<std::shared_mutex> l(g_layout_mu);
        g_set_layouts[H(*out)] = std::move(s);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout_h(VkDevice device, VkDescriptorSetLayout s, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_layout_mu);
        g_set_layouts.erase(H(s));
    }
    dd->d.DestroyDescriptorSetLayout(device, s, a);
}
// The pool's sets are gone (pool reset or destroyed). The pool is externally synchronised by the game.
void ForgetPoolSets(uint64_t pool)
{
    std::vector<uint64_t> sets;
    {
        std::lock_guard<std::mutex> l(g_pool_mu);
        auto it = g_pool_sets.find(pool);
        if (it == g_pool_sets.end()) return;
        sets.swap(it->second);
        g_pool_sets.erase(it);
    }
    for (uint64_t h : sets) {
        SetShard& sh = SetShardOf(h);
        std::unique_lock<std::shared_mutex> l(sh.mu);
        sh.sets.erase(h);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets_h(VkDevice device, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.AllocateDescriptorSets(device, ai, out);
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) {
            SetInfo s;
            {
                std::shared_lock<std::shared_mutex> l(g_layout_mu);
                auto lt = g_set_layouts.find(H(ai->pSetLayouts[i]));
                if (lt != g_set_layouts.end()) {
                    s.bindings = lt->second.bindings;
                    s.dynamic = lt->second.dynamic;
                }
            }
            SetShard& sh = SetShardOf(H(out[i]));
            std::unique_lock<std::shared_mutex> l(sh.mu);
            sh.sets[H(out[i])] = std::move(s);
        }
        std::lock_guard<std::mutex> l(g_pool_mu);
        auto& pool = g_pool_sets[H(ai->descriptorPool)];
        for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) pool.push_back(H(out[i]));
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL FreeDescriptorSets_h(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* sets)
{
    NA_DEV(device);
    for (uint32_t i = 0; i < n; ++i) {
        if (!sets[i]) continue;
        SetShard& sh = SetShardOf(H(sets[i]));
        std::unique_lock<std::shared_mutex> l(sh.mu);
        sh.sets.erase(H(sets[i]));
    }
    return dd->d.FreeDescriptorSets(device, pool, n, sets);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetDescriptorPool_h(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags f)
{
    NA_DEV(device);
    ForgetPoolSets(H(pool));
    return dd->d.ResetDescriptorPool(device, pool, f);
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool_h(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    ForgetPoolSets(H(pool));
    dd->d.DestroyDescriptorPool(device, pool, a);
}

// Notes one uniform-buffer descriptor (vertex-visible only). Under the set's shard lock (unique).
void NoteUbo(SetInfo& s, uint32_t binding, uint32_t element, VkDescriptorType type, const VkDescriptorBufferInfo& bi)
{
    if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) return;
    bool vertex = false;
    for (const auto& b : s.bindings)
        if (b.binding == binding) vertex = (b.stages & kVertexStages) != 0;
    if (!vertex) return;
    SetInfo::Ubo u;
    u.buffer = bi.buffer;
    u.offset = bi.offset;
    u.range = bi.range;
    if (u.range == VK_WHOLE_SIZE) {
        std::shared_lock<std::shared_mutex> l(g_obj_mu);
        auto bt = g_buffers.find(H(bi.buffer));
        u.range = bt != g_buffers.end() && bt->second.size > bi.offset ? bt->second.size - bi.offset : 0;
    }
    u.dynamic = type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    s.ubos[(uint64_t(binding) << 32) | element] = u;
}

// Descriptor array elements spill into the next bindings (Vulkan's consecutive-binding rule); kept simple: same binding.
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets_h(VkDevice device, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c)
{
    NA_DEV(device);
    // Only uniform-buffer writes matter to us; images, samplers and storage
    // buffers (most of a game's updates) pass straight through, with no lock.
    for (uint32_t i = 0; i < nw; ++i) {
        if (!w[i].pBufferInfo) continue;
        if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        SetShard& sh = SetShardOf(H(w[i].dstSet));
        std::unique_lock<std::shared_mutex> l(sh.mu);
        auto it = sh.sets.find(H(w[i].dstSet));
        if (it == sh.sets.end()) continue;
        for (uint32_t k = 0; k < w[i].descriptorCount; ++k)
            NoteUbo(it->second, w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, w[i].pBufferInfo[k]);
    }
    for (uint32_t i = 0; i < nc; ++i) {
        const uint64_t sh_src = H(c[i].srcSet), sh_dst = H(c[i].dstSet);
        const uint32_t a = SetShardIndex(sh_src), b = SetShardIndex(sh_dst);
        // Both shards, always lower index first (no deadlock with another copy).
        std::unique_lock<std::shared_mutex> l1(g_set_shards[a < b ? a : b].mu);
        std::unique_lock<std::shared_mutex> l2;
        if (a != b) l2 = std::unique_lock<std::shared_mutex>(g_set_shards[a < b ? b : a].mu);
        auto& src_sets = g_set_shards[a].sets;
        auto& dst_sets = g_set_shards[b].sets;
        auto st = src_sets.find(sh_src);
        auto dt = dst_sets.find(sh_dst);
        if (st == src_sets.end() || dt == dst_sets.end() || st->second.ubos.empty()) continue;
        for (uint32_t k = 0; k < c[i].descriptorCount; ++k) {
            auto u = st->second.ubos.find((uint64_t(c[i].srcBinding) << 32) | (c[i].srcArrayElement + k));
            if (u != st->second.ubos.end()) dt->second.ubos[(uint64_t(c[i].dstBinding) << 32) | (c[i].dstArrayElement + k)] = u->second;
        }
    }
    dd->d.UpdateDescriptorSets(device, nw, w, nc, c);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate_h(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* ci,
                                                                const VkAllocationCallbacks* a, VkDescriptorUpdateTemplate* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateDescriptorUpdateTemplate(device, ci, a, out);
    if (r == VK_SUCCESS) {
        TemplateInfo t;
        t.entries.assign(ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount);
        t.for_sets = ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        for (const auto& e : t.entries)
            if (e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) t.has_ubo = true;
        std::unique_lock<std::shared_mutex> l(g_layout_mu);
        g_templates[H(*out)] = std::move(t);
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate_h(VkDevice device, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::unique_lock<std::shared_mutex> l(g_layout_mu);
        g_templates.erase(H(t));
    }
    dd->d.DestroyDescriptorUpdateTemplate(device, t, a);
}
VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate_h(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data)
{
    NA_DEV(device);
    if (data) {
        std::shared_lock<std::shared_mutex> lt(g_layout_mu); // the template stays alive while we read it
        auto tt = g_templates.find(H(t));
        if (tt != g_templates.end() && tt->second.for_sets && tt->second.has_ubo) {
            SetShard& sh = SetShardOf(H(set));
            std::unique_lock<std::shared_mutex> l(sh.mu);
            auto st = sh.sets.find(H(set));
            if (st != sh.sets.end())
                for (const auto& e : tt->second.entries) {
                    if (e.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && e.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
                    for (uint32_t k = 0; k < e.descriptorCount; ++k) {
                        const auto* bi = reinterpret_cast<const VkDescriptorBufferInfo*>(static_cast<const uint8_t*>(data) + e.offset + k * e.stride);
                        NoteUbo(st->second, e.dstBinding, e.dstArrayElement + k, e.descriptorType, *bi);
                    }
                }
        }
    }
    dd->d.UpdateDescriptorSetWithTemplate(device, set, t, data);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR_h(VkDevice device, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.CreateSwapchainKHR(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    {
        std::unique_lock<std::shared_mutex> l(g_tables_mu);
        if (!dd->host) {
            // The first device that presents is the game's.
            bool taken = false;
            for (auto& kv : g_devices) taken = taken || kv.second->host;
            if (!taken) {
                dd->host = std::make_unique<Host>();
                dd->host->dev = dd;
                dd->host->stats.size = sizeof(NaVkStats);
                dd->host->stats.active = 1;
                dd->host->stats.dlss_built = na::vk::VkDlss::BuiltWithDlss();
                Log("device: %s presents (%ux%u); this is the game's device", dd->props.deviceName, ci->imageExtent.width, ci->imageExtent.height);
                // Render targets made before the swap chain.
                std::shared_lock<std::shared_mutex> ol(g_obj_mu);
                for (const auto& kv : g_images) {
                    const ImageInfo& ii = *kv.second;
                    ResourceInfo ri;
                    ri.width = ii.w;
                    ri.height = ii.h;
                    ri.format = static_cast<uint32_t>(ii.format);
                    ri.samples = static_cast<uint32_t>(ii.samples);
                    ri.depth = ii.depth;
                    dd->host->model.SetInfo(ii.cls, ri);
                }
            }
        }
    }
    if (Host* h = HostOf(dd)) {
        std::lock_guard<std::mutex> l(h->mu);
        h->swap_w = ci->imageExtent.width;
        h->swap_h = ci->imageExtent.height;
        h->model.SetSwapChainSize(h->swap_w, h->swap_h);
        Log("swapchain: %ux%u format %d", h->swap_w, h->swap_h, static_cast<int>(ci->imageFormat));
        TryInitDlss(*h);
    }
    return r;
}

// ================================================================ hooks: command buffers
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers_h(VkDevice device, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out)
{
    NA_DEV(device);
    const VkResult r = dd->d.AllocateCommandBuffers(device, ai, out);
    if (r != VK_SUCCESS) return r;
    std::lock_guard<std::mutex> l(g_cb_mu);
    auto& pool = g_pool_cbs[H(ai->commandPool)];
    for (uint32_t i = 0; i < ai->commandBufferCount; ++i) {
        auto st = std::make_unique<CbState>();
        st->dev = dd;
        st->cb = out[i];
        st->pool = ai->commandPool;
        st->secondary = ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        g_cbs[H(out[i])] = std::move(st);
        pool.push_back(H(out[i]));
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers_h(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs)
{
    NA_DEV(device);
    {
        std::lock_guard<std::mutex> l(g_cb_mu);
        auto& list = g_pool_cbs[H(pool)];
        for (uint32_t i = 0; i < n; ++i) {
            if (!cbs[i]) continue;
            g_cbs.erase(H(cbs[i]));
            list.erase(std::remove(list.begin(), list.end(), H(cbs[i])), list.end());
        }
        g_cb_epoch.fetch_add(1, std::memory_order_release);
    }
    dd->d.FreeCommandBuffers(device, pool, n, cbs);
}
VKAPI_ATTR void VKAPI_CALL DestroyCommandPool_h(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* a)
{
    NA_DEV(device);
    {
        std::lock_guard<std::mutex> l(g_cb_mu);
        auto it = g_pool_cbs.find(H(pool));
        if (it != g_pool_cbs.end()) {
            for (uint64_t c : it->second) g_cbs.erase(c);
            g_pool_cbs.erase(it);
        }
        g_cb_epoch.fetch_add(1, std::memory_order_release);
    }
    dd->d.DestroyCommandPool(device, pool, a);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool_h(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags f)
{
    NA_DEV(device);
    {
        std::lock_guard<std::mutex> l(g_cb_mu);
        auto it = g_pool_cbs.find(H(pool));
        if (it != g_pool_cbs.end())
            for (uint64_t c : it->second) {
                auto ct = g_cbs.find(c);
                if (ct != g_cbs.end()) ct->second->Reset();
            }
    }
    return dd->d.ResetCommandPool(device, pool, f);
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer_h(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
    CbState* cs = Cb(cb);
    if (cs) cs->Reset();
    return cs ? cs->dev->d.ResetCommandBuffer(cb, f) : DeviceOf(cb)->d.ResetCommandBuffer(cb, f);
}
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer_h(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi)
{
    CbState* cs = Cb(cb);
    if (!cs) return DeviceOf(cb)->d.BeginCommandBuffer(cb, bi);
    cs->Reset();
    if (Host* h = HostOf(cs->dev)) {
        // No h.mu here: a submit replay can hold it for a while, and every recording thread begins command buffers.
        cs->learned_tag = h->pub_tag.load(std::memory_order_acquire);
        cs->sel_color = h->pub_color.load(std::memory_order_relaxed);
        cs->sel_depth = h->pub_depth.load(std::memory_order_relaxed);
    } else {
        cs->sel_color = cs->sel_depth = 0;
        cs->learned_tag = 0;
    }
    if (cs->secondary && bi && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && bi->pInheritanceInfo) {
        cs->continue_pass = true;
        cs->in_pass = true;
        if (bi->pInheritanceInfo->framebuffer && bi->pInheritanceInfo->renderPass) {
            {
                std::shared_lock<std::shared_mutex> l(g_obj_mu);
                auto ft = g_fbs.find(H(bi->pInheritanceInfo->framebuffer));
                if (ft != g_fbs.end()) cs->fb_views = ft->second.views;
            }
            cs->rp = bi->pInheritanceInfo->renderPass;
            cs->subpass = bi->pInheritanceInfo->subpass;
            LoadSubpass(*cs);
        }
    }
    return cs->dev->d.BeginCommandBuffer(cb, bi);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass_h(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c)
{
    CbState* cs = Cb(cb);
    if (cs) BeginPassCommon(*cs, bi->renderPass, bi->framebuffer, bi->pNext);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBeginRenderPass(cb, bi, c);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2_h(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* sb)
{
    CbState* cs = Cb(cb);
    if (cs) BeginPassCommon(*cs, bi->renderPass, bi->framebuffer, bi->pNext);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBeginRenderPass2(cb, bi, sb);
}
VKAPI_ATTR void VKAPI_CALL CmdNextSubpass_h(VkCommandBuffer cb, VkSubpassContents c)
{
    CbState* cs = Cb(cb);
    if (cs) { ++cs->subpass; LoadSubpass(*cs); }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdNextSubpass(cb, c);
}
VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2_h(VkCommandBuffer cb, const VkSubpassBeginInfo* b, const VkSubpassEndInfo* e)
{
    CbState* cs = Cb(cb);
    if (cs) { ++cs->subpass; LoadSubpass(*cs); }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdNextSubpass2(cb, b, e);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass_h(VkCommandBuffer cb)
{
    CbState* cs = Cb(cb);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdEndRenderPass(cb);
    if (cs) EndPassCommon(*cs); // reads after the pass (and DLSS, if it is the point) come after the game's command
}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2_h(VkCommandBuffer cb, const VkSubpassEndInfo* e)
{
    CbState* cs = Cb(cb);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdEndRenderPass2(cb, e);
    if (cs) EndPassCommon(*cs);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginRendering_h(VkCommandBuffer cb, const VkRenderingInfo* ri)
{
    CbState* cs = Cb(cb);
    if (cs) {
        cs->in_pass = true;
        ++cs->passes;
        cs->rp = VK_NULL_HANDLE;
        cs->fb_views.clear();
        cs->pass_finals.clear();
        cs->rt_cls = cs->depth_cls = 0;
        cs->rt_img = cs->depth_img = VK_NULL_HANDLE;
        Id clear_cls = 0;
        {
            std::shared_lock<std::shared_mutex> l(g_obj_mu);
            if (ri->colorAttachmentCount && ri->pColorAttachments && ri->pColorAttachments[0].imageView) {
                cs->rt_img = ImageOfView(ri->pColorAttachments[0].imageView);
                if (ImageInfo* ii = cs->rt_img ? ImageOf(cs->rt_img) : nullptr) { cs->rt_cls = ii->cls; ii->layout.store(ri->pColorAttachments[0].imageLayout); }
            }
            if (ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
                cs->depth_img = ImageOfView(ri->pDepthAttachment->imageView);
                if (ImageInfo* ii = cs->depth_img ? ImageOf(cs->depth_img) : nullptr) {
                    cs->depth_cls = ii->cls;
                    ii->layout.store(ri->pDepthAttachment->imageLayout);
                    if (ri->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) clear_cls = ii->cls;
                }
            }
        }
        if (clear_cls) {
            Event e;
            e.kind = Kind::ClearDepth;
            e.resource = clear_cls;
            AddModel(*cs, e);
        }
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBeginRendering(cb, ri);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRendering_h(VkCommandBuffer cb)
{
    CbState* cs = Cb(cb);
    if (cs) {
        cs->in_pass = false;
        cs->rt_cls = cs->depth_cls = 0;
        cs->rt_img = cs->depth_img = VK_NULL_HANDLE;
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdEndRendering(cb);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline_h(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    CbState* cs = Cb(cb);
    if (cs) {
        if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) cs->comp_pipe = p;
        else if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
            cs->gfx_pipe = p;
            std::shared_lock<std::shared_mutex> l(g_obj_mu);
            auto it = g_pipes.find(H(p));
            cs->pipe_dynamic_viewport = it == g_pipes.end() || it->second.dynamic_viewport;
            cs->pipe_viewport_h = it == g_pipes.end() ? 1.f : it->second.viewport_h;
        }
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBindPipeline(cb, bp, p);
}
VKAPI_ATTR void VKAPI_CALL CmdSetViewport_h(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v)
{
    CbState* cs = Cb(cb);
    if (cs && first == 0 && n && v) cs->dyn_viewport_h = v[0].height;
    (cs ? cs->dev : DeviceOf(cb))->d.CmdSetViewport(cb, first, n, v);
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets_h(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                                                    const VkDescriptorSet* sets, uint32_t ndyn, const uint32_t* dyn)
{
    CbState* cs = Cb(cb);
    if (cs) {
        // Reused per thread: no heap allocation per bind once warmed up.
        thread_local std::vector<std::pair<Id, UboRef>> fresh; // candidate ids this bind produced
        thread_local std::vector<std::pair<uint64_t, uint32_t>> dyn_of; // (binding << 32 | element) -> dynamic offset
        fresh.clear();
        uint32_t di = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t slot = first + i;
            SetBind* b = nullptr;
            if (slot < kMaxSets) {
                b = bp == VK_PIPELINE_BIND_POINT_COMPUTE ? &cs->comp_sets[slot] : &cs->gfx_sets[slot];
                b->layout = layout;
                b->set = sets[i];
                b->dyn.clear();
            }
            const bool gfx = b && bp == VK_PIPELINE_BIND_POINT_GRAPHICS;
            if (gfx) cs->gfx_ubo_ids[slot].clear();
            if (!sets[i]) continue;
            SetShard& sh = SetShardOf(H(sets[i]));
            std::shared_lock<std::shared_mutex> l(sh.mu);
            auto st = sh.sets.find(H(sets[i]));
            if (st == sh.sets.end()) continue;
            // This set's dynamic offsets, in binding order (Vulkan's rule; the order is worked out at layout creation).
            dyn_of.clear();
            for (const auto& bd : st->second.dynamic)
                for (uint32_t e = 0; e < bd.count && di < ndyn; ++e) {
                    const uint32_t v = dyn[di++];
                    if (b) b->dyn.push_back(v);
                    if (bd.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) dyn_of.push_back({(uint64_t(bd.binding) << 32) | e, v});
                }
            if (!gfx) continue;
            auto& ids = cs->gfx_ubo_ids[slot];
            for (const auto& kv : st->second.ubos) {
                if (ids.size() >= 4) break;
                UboRef ref{kv.second.buffer, kv.second.offset, kv.second.range};
                if (kv.second.dynamic)
                    for (const auto& d : dyn_of)
                        if (d.first == kv.first) { ref.offset += d.second; break; }
                const Id id = NonZero(Mix(Mix(0x0b0, H(ref.buffer)), Mix(ref.offset, ref.range)) & ~kSecondaryTag);
                ids.push_back(id);
                fresh.push_back({id, ref});
            }
        }
        if (!fresh.empty()) {
            bool missing = false;
            {
                std::shared_lock<std::shared_mutex> l(g_obj_mu);
                for (const auto& f : fresh)
                    if (!g_ubo_ids.count(f.first)) { missing = true; break; }
            }
            if (missing) {
                std::unique_lock<std::shared_mutex> l(g_obj_mu);
                if (g_ubo_ids.size() > 200000) g_ubo_ids.clear(); // bounded: re-learned from the next binds
                for (const auto& f : fresh) g_ubo_ids.emplace(f.first, f.second);
            }
        }
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants_h(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                              const void* values)
{
    CbState* cs = Cb(cb);
    if (cs) {
        const auto* b = static_cast<const uint8_t*>(values);
        bool found = false;
        for (auto& p : cs->pushes)
            if (p.layout == layout && p.stages == stages && p.offset == offset) { p.bytes.assign(b, b + size); found = true; }
        if (!found && cs->pushes.size() < 16) cs->pushes.push_back({layout, stages, offset, std::vector<uint8_t>(b, b + size)});
        if ((stages & kVertexStages) && size >= 64) {
            cs->push_id = NonZero(Mix(0x9c, Mix(offset, size)) & ~kSecondaryTag);
            if (cs->last_push.size() != size || std::memcmp(cs->last_push.data(), b, size) != 0) {
                cs->last_push.assign(b, b + size);
                RecEvent r;
                r.t = RecEvent::Push;
                r.ev.resource = cs->push_id;
                r.a = static_cast<uint32_t>(cs->blob.size());
                r.b = std::min<uint32_t>(size, static_cast<uint32_t>(kMaxCamBytes));
                cs->blob.insert(cs->blob.end(), b, b + r.b);
                cs->ev.push_back(r);
            }
        }
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdPushConstants(cb, layout, stages, offset, size, values);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw_h(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, uint64_t(vc) * ic, false);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDraw(cb, vc, ic, fv, fi);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed_h(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi, int32_t vo, uint32_t fin)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, uint64_t(n) * ic, false);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDrawIndexed(cb, n, ic, fi, vo, fin);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect_h(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, 0, true);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDrawIndirect(cb, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect_h(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, 0, true);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDrawIndexedIndirect(cb, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount_h(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, 0, true);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDrawIndirectCount(cb, b, o, cbuf, co, m, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount_h(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m,
                                                         uint32_t s)
{
    CbState* cs = Cb(cb);
    if (cs) OnDraw(*cs, 0, true);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdDrawIndexedIndirectCount(cb, b, o, cbuf, co, m, s);
}

// Barriers: reads of the scene colour (the DLSS point is right before), and layouts.
template <class B> void BarrierImages(CbState& cs, uint32_t n, const B* bs, bool sync2)
{
    for (uint32_t i = 0; i < n; ++i) {
        const VkAccessFlags2 src = bs[i].srcAccessMask, dst = bs[i].dstAccessMask;
        (void)sync2;
        if (!cs.in_pass && BarrierIsRead(bs[i].oldLayout, bs[i].newLayout, src, dst)) OnRead(cs, bs[i].image, bs[i].oldLayout, 1);
    }
    for (uint32_t i = 0; i < n; ++i)
        if (bs[i].oldLayout != bs[i].newLayout) SetLayout(bs[i].image, bs[i].newLayout);
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier_h(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags f, uint32_t nm,
                                                const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
                                                const VkImageMemoryBarrier* im)
{
    CbState* cs = Cb(cb);
    if (cs) BarrierImages(*cs, ni, im, false);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdPipelineBarrier(cb, ss, ds, f, nm, m, nb, b, ni, im);
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2_h(VkCommandBuffer cb, const VkDependencyInfo* di)
{
    CbState* cs = Cb(cb);
    if (cs && di) BarrierImages(*cs, di->imageMemoryBarrierCount, di->pImageMemoryBarriers, true);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdPipelineBarrier2(cb, di);
}

VKAPI_ATTR void VKAPI_CALL CmdCopyImage_h(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, s, sl, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyImage(cb, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImage2_h(VkCommandBuffer cb, const VkCopyImageInfo2* i)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, i->srcImage, i->srcImageLayout, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyImage2(cb, i);
}
VKAPI_ATTR void VKAPI_CALL CmdBlitImage_h(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r,
                                          VkFilter f)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, s, sl, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBlitImage(cb, s, sl, d, dl, n, r, f);
}
VKAPI_ATTR void VKAPI_CALL CmdBlitImage2_h(VkCommandBuffer cb, const VkBlitImageInfo2* i)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, i->srcImage, i->srcImageLayout, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdBlitImage2(cb, i);
}
VKAPI_ATTR void VKAPI_CALL CmdResolveImage_h(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
                                             const VkImageResolve* r)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, s, sl, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdResolveImage(cb, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL CmdResolveImage2_h(VkCommandBuffer cb, const VkResolveImageInfo2* i)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, i->srcImage, i->srcImageLayout, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdResolveImage2(cb, i);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer_h(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy* r)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, s, sl, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyImageToBuffer(cb, s, sl, d, n, r);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer2_h(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* i)
{
    CbState* cs = Cb(cb);
    if (cs) OnRead(*cs, i->srcImage, i->srcImageLayout, 3);
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyImageToBuffer2(cb, i);
}
VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage_h(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n,
                                                       const VkImageSubresourceRange* r)
{
    CbState* cs = Cb(cb);
    if (cs) {
        Id cls = 0;
        {
            std::shared_lock<std::shared_mutex> lk(g_obj_mu);
            if (const ImageInfo* ii = ImageOf(img)) cls = ii->cls;
        }
        if (cls) {
            Event e;
            e.kind = Kind::ClearDepth;
            e.resource = cls;
            AddModel(*cs, e);
        }
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdClearDepthStencilImage(cb, img, l, v, n, r);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer_h(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r)
{
    CbState* cs = Cb(cb);
    if (cs)
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            RecEvent e;
            e.t = RecEvent::BufCopy;
            e.src = s;
            e.dst = d;
            e.so = r[i].srcOffset;
            e.dof = r[i].dstOffset;
            e.size = r[i].size;
            cs->ev.push_back(e);
        }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyBuffer(cb, s, d, n, r);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2_h(VkCommandBuffer cb, const VkCopyBufferInfo2* i)
{
    CbState* cs = Cb(cb);
    if (cs)
        for (uint32_t k = 0; k < i->regionCount && k < 64; ++k) {
            RecEvent e;
            e.t = RecEvent::BufCopy;
            e.src = i->srcBuffer;
            e.dst = i->dstBuffer;
            e.so = i->pRegions[k].srcOffset;
            e.dof = i->pRegions[k].dstOffset;
            e.size = i->pRegions[k].size;
            cs->ev.push_back(e);
        }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdCopyBuffer2(cb, i);
}
VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer_h(VkCommandBuffer cb, VkBuffer d, VkDeviceSize off, VkDeviceSize size, const void* data)
{
    CbState* cs = Cb(cb);
    if (cs && size >= 64 && size <= 65536) {
        RecEvent e;
        e.t = RecEvent::BufUpdate;
        e.dst = d;
        e.dof = off;
        e.size = size;
        e.a = static_cast<uint32_t>(cs->blob.size());
        e.b = static_cast<uint32_t>(size);
        const auto* b = static_cast<const uint8_t*>(data);
        cs->blob.insert(cs->blob.end(), b, b + size);
        cs->ev.push_back(e);
    }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdUpdateBuffer(cb, d, off, size, data);
}
VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands_h(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* secs)
{
    CbState* cs = Cb(cb);
    if (cs)
        for (uint32_t i = 0; i < n; ++i) {
            RecEvent e;
            e.t = RecEvent::Exec;
            e.sec = secs[i];
            e.rt = cs->rt_cls;
            e.depth = cs->depth_cls;
            cs->ev.push_back(e);
            // Scene draws inside secondaries feed the record-time "last scene image" too.
            if (cs->sel_color && cs->rt_cls == cs->sel_color && cs->depth_cls == cs->sel_depth) {
                cs->last_scene_color = cs->rt_img;
                cs->last_scene_depth = cs->depth_img;
                ++cs->scene_draws;
            }
        }
    (cs ? cs->dev : DeviceOf(cb))->d.CmdExecuteCommands(cb, n, secs);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit_h(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{
    DeviceData* dd = DeviceOf(q);
    if (HostOf(dd)) {
        std::vector<VkCommandBuffer> cbs;
        for (uint32_t i = 0; i < n; ++i) cbs.insert(cbs.end(), s[i].pCommandBuffers, s[i].pCommandBuffers + s[i].commandBufferCount);
        OnSubmit(*dd, cbs);
    }
    return dd->d.QueueSubmit(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2_h(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{
    DeviceData* dd = DeviceOf(q);
    if (HostOf(dd)) {
        std::vector<VkCommandBuffer> cbs;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < s[i].commandBufferInfoCount; ++k) cbs.push_back(s[i].pCommandBufferInfos[k].commandBuffer);
        OnSubmit(*dd, cbs);
    }
    return dd->d.QueueSubmit2(q, n, s, f);
}
VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR_h(VkQueue q, const VkPresentInfoKHR* pi)
{
    DeviceData* dd = DeviceOf(q);
    OnPresent(*dd);
    return dd->d.QueuePresentKHR(q, pi);
}

// ================================================================ instance / device
VKAPI_ATTR VkResult VKAPI_CALL CreateInstance_h(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out)
{
    auto* chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    const bool active = Cfg().enabled;
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    std::vector<std::string> want_i, want_d;
    if (active) na::vk::VkDlss::RequiredExtensions(&want_i, &want_d);
    std::vector<std::string> keep;
    if (!want_i.empty()) {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
        uint32_t cnt = 0;
        std::vector<VkExtensionProperties> have;
        if (enumerate && enumerate(nullptr, &cnt, nullptr) == VK_SUCCESS) { have.resize(cnt); enumerate(nullptr, &cnt, have.data()); have.resize(cnt); }
        for (const auto& w : want_i) {
            bool present = false, listed = false;
            for (const auto& hx : have) present = present || w == hx.extensionName;
            for (const char* e : exts) listed = listed || w == e;
            if (present && !listed) keep.push_back(w);
        }
    }
    for (const auto& k : keep) exts.push_back(k.c_str());
    VkInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    c.ppEnabledExtensionNames = exts.data();
    VkResult r = create(&c, a, out);
    if (r != VK_SUCCESS && !keep.empty()) r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;

    auto inst = std::make_unique<InstanceData>();
    inst->instance = *out;
    inst->gipa = gipa;
    inst->active = active;
    inst->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
    inst->GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(*out, "vkGetPhysicalDeviceMemoryProperties"));
    inst->GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa(*out, "vkGetPhysicalDeviceFormatProperties"));
    inst->GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(*out, "vkGetPhysicalDeviceProperties"));
    inst->EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(*out, "vkEnumerateDeviceExtensionProperties"));
    if (active) {
        OpenLog();
        Log("NeuralAlias Vulkan layer: on (%s), pid %u, jitter=%d mip_bias=%.2f preset=%u copy_test=%u, DLSS %s", Cfg().ini_path.u8string().c_str(), Pid(),
            Cfg().jitter ? 1 : 0, Cfg().mip_bias, Cfg().preset, Cfg().copy_test, na::vk::VkDlss::BuiltWithDlss() ? "built in" : "not in this build");
        if (!keep.empty()) Log("instance: added %zu extension(s) DLSS needs", keep.size());
    }
    std::unique_lock<std::shared_mutex> l(g_tables_mu);
    g_instances[Key(*out)] = std::move(inst);
    g_tables_epoch.fetch_add(1, std::memory_order_release);
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance_h(VkInstance instance, const VkAllocationCallbacks* a)
{
    std::unique_ptr<InstanceData> inst;
    {
        std::unique_lock<std::shared_mutex> l(g_tables_mu);
        auto it = g_instances.find(Key(instance));
        if (it == g_instances.end()) return;
        inst = std::move(it->second);
        g_instances.erase(it);
        g_tables_epoch.fetch_add(1, std::memory_order_release);
    }
    inst->DestroyInstance(instance, a);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice_h(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out)
{
    InstanceData* inst = InstanceOf(phys);
    auto* chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    if (!chain || !inst) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    std::vector<std::string> want_i, want_d, keep;
    if (inst->active) na::vk::VkDlss::RequiredExtensions(&want_i, &want_d);
    if (!want_d.empty() && inst->EnumerateDeviceExtensionProperties) {
        uint32_t cnt = 0;
        std::vector<VkExtensionProperties> have;
        if (inst->EnumerateDeviceExtensionProperties(phys, nullptr, &cnt, nullptr) == VK_SUCCESS) {
            have.resize(cnt);
            inst->EnumerateDeviceExtensionProperties(phys, nullptr, &cnt, have.data());
            have.resize(cnt);
        }
        for (const auto& w : want_d) {
            bool present = false, listed = false;
            for (const auto& hx : have) present = present || w == hx.extensionName;
            for (const char* e : exts) listed = listed || w == e;
            if (present && !listed) keep.push_back(w);
        }
    }
    for (const auto& k : keep) exts.push_back(k.c_str());
    VkDeviceCreateInfo c = *ci;
    c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    c.ppEnabledExtensionNames = exts.data();
    VkResult r = create(phys, &c, a, out);
    if (r != VK_SUCCESS && !keep.empty()) { keep.clear(); r = create(phys, ci, a, out); }
    if (r != VK_SUCCESS) return r;

    auto dd = std::make_unique<DeviceData>();
    dd->device = *out;
    dd->phys = phys;
    dd->inst = inst;
    dd->gdpa = gdpa;
#define NA_LOAD(n) dd->d.n = reinterpret_cast<PFN_vk##n>(gdpa(*out, "vk" #n));
#define NA_LOAD2(n, alt)                                                                  \
    dd->d.n = reinterpret_cast<PFN_vk##n>(gdpa(*out, "vk" #n));                          \
    if (!dd->d.n) dd->d.n = reinterpret_cast<PFN_vk##n>(gdpa(*out, alt));
    NA_VK_DEVICE_FNS(NA_LOAD)
    NA_VK_DEVICE_FNS_ALIASED(NA_LOAD2)
    NA_VK_DEVICE_FNS_KHR(NA_LOAD)
#undef NA_LOAD
#undef NA_LOAD2
    if (!dd->d.CmdDrawIndirectCount) dd->d.CmdDrawIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(gdpa(*out, "vkCmdDrawIndirectCountAMD"));
    if (!dd->d.CmdDrawIndexedIndirectCount)
        dd->d.CmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(gdpa(*out, "vkCmdDrawIndexedIndirectCountAMD"));
    if (inst->GetPhysicalDeviceMemoryProperties) inst->GetPhysicalDeviceMemoryProperties(phys, &dd->mem);
    if (inst->GetPhysicalDeviceProperties) inst->GetPhysicalDeviceProperties(phys, &dd->props);
    if (inst->active) {
        Log("device: %s created%s", dd->props.deviceName, keep.empty() ? "" : " (with the extensions DLSS needs)");
    }
    std::unique_lock<std::shared_mutex> l(g_tables_mu);
    g_devices[Key(*out)] = std::move(dd);
    g_tables_epoch.fetch_add(1, std::memory_order_release);
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice_h(VkDevice device, const VkAllocationCallbacks* a)
{
    std::unique_ptr<DeviceData> dd;
    {
        std::unique_lock<std::shared_mutex> l(g_tables_mu);
        auto it = g_devices.find(Key(device));
        if (it == g_devices.end()) return;
        dd = std::move(it->second);
        g_devices.erase(it);
        g_tables_epoch.fetch_add(1, std::memory_order_release);
    }
    if (Host* h = dd->host.get()) {
        dd->d.DeviceWaitIdle(device);
        std::lock_guard<std::mutex> l(h->mu);
        h->dlss.Shutdown();
        FreeSizedResources(*h);
        ReleaseRetired(*h, true); // the device is idle
        const DeviceDispatch& d = dd->d;
        if (h->ring_mem) d.FreeMemory(device, h->ring_mem, nullptr);
        if (h->ring) d.DestroyBuffer(device, h->ring, nullptr);
        if (h->pool) d.DestroyDescriptorPool(device, h->pool, nullptr);
        if (h->sampler) d.DestroySampler(device, h->sampler, nullptr);
        if (h->motion_pipe) d.DestroyPipeline(device, h->motion_pipe, nullptr);
        if (h->copy_pipe) d.DestroyPipeline(device, h->copy_pipe, nullptr);
        if (h->motion_pl) d.DestroyPipelineLayout(device, h->motion_pl, nullptr);
        if (h->copy_pl) d.DestroyPipelineLayout(device, h->copy_pl, nullptr);
        if (h->motion_dsl) d.DestroyDescriptorSetLayout(device, h->motion_dsl, nullptr);
        if (h->copy_dsl) d.DestroyDescriptorSetLayout(device, h->copy_dsl, nullptr);
        if (h->motion_sm) d.DestroyShaderModule(device, h->motion_sm, nullptr);
        if (h->copy_sm) d.DestroyShaderModule(device, h->copy_sm, nullptr);
        Log("device: closed after %llu frames (delivered %llu)", static_cast<unsigned long long>(h->frame), static_cast<unsigned long long>(h->stats.delivered));
    }
    {
        std::lock_guard<std::mutex> l(g_cb_mu);
        for (auto it = g_cbs.begin(); it != g_cbs.end();)
            it = it->second->dev == dd.get() ? g_cbs.erase(it) : std::next(it);
        g_cb_epoch.fetch_add(1, std::memory_order_release);
    }
    dd->d.DestroyDevice(device, a);
}

// ================================================================ proc addr
struct Hook { const char* name; PFN_vkVoidFunction fn; };
#define NA_HOOK(n) {"vk" #n, reinterpret_cast<PFN_vkVoidFunction>(&n##_h)}
#define NA_HOOK_ALT(alt, n) {alt, reinterpret_cast<PFN_vkVoidFunction>(&n##_h)}
const Hook kDeviceHooks[] = {
    NA_HOOK(DestroyDevice), NA_HOOK(CreateImage), NA_HOOK(DestroyImage), NA_HOOK(CreateImageView), NA_HOOK(DestroyImageView),
    NA_HOOK(CreateBuffer), NA_HOOK(DestroyBuffer), NA_HOOK(AllocateMemory), NA_HOOK(FreeMemory), NA_HOOK(MapMemory), NA_HOOK(UnmapMemory),
    NA_HOOK(BindBufferMemory), NA_HOOK(BindBufferMemory2), NA_HOOK_ALT("vkBindBufferMemory2KHR", BindBufferMemory2), NA_HOOK(CreateSampler),
    NA_HOOK(CreateRenderPass), NA_HOOK(CreateRenderPass2), NA_HOOK_ALT("vkCreateRenderPass2KHR", CreateRenderPass2), NA_HOOK(DestroyRenderPass),
    NA_HOOK(CreateFramebuffer), NA_HOOK(DestroyFramebuffer), NA_HOOK(CreateGraphicsPipelines), NA_HOOK(DestroyPipeline),
    NA_HOOK(CreateDescriptorSetLayout), NA_HOOK(DestroyDescriptorSetLayout), NA_HOOK(AllocateDescriptorSets), NA_HOOK(FreeDescriptorSets),
    NA_HOOK(ResetDescriptorPool), NA_HOOK(DestroyDescriptorPool), NA_HOOK(UpdateDescriptorSets), NA_HOOK(CreateDescriptorUpdateTemplate),
    NA_HOOK_ALT("vkCreateDescriptorUpdateTemplateKHR", CreateDescriptorUpdateTemplate), NA_HOOK(DestroyDescriptorUpdateTemplate),
    NA_HOOK_ALT("vkDestroyDescriptorUpdateTemplateKHR", DestroyDescriptorUpdateTemplate), NA_HOOK(UpdateDescriptorSetWithTemplate),
    NA_HOOK_ALT("vkUpdateDescriptorSetWithTemplateKHR", UpdateDescriptorSetWithTemplate), NA_HOOK(CreateSwapchainKHR),
    NA_HOOK(AllocateCommandBuffers), NA_HOOK(FreeCommandBuffers), NA_HOOK(DestroyCommandPool), NA_HOOK(ResetCommandPool),
    NA_HOOK(ResetCommandBuffer), NA_HOOK(BeginCommandBuffer), NA_HOOK(CmdBeginRenderPass), NA_HOOK(CmdBeginRenderPass2),
    NA_HOOK_ALT("vkCmdBeginRenderPass2KHR", CmdBeginRenderPass2), NA_HOOK(CmdNextSubpass), NA_HOOK(CmdNextSubpass2),
    NA_HOOK_ALT("vkCmdNextSubpass2KHR", CmdNextSubpass2), NA_HOOK(CmdEndRenderPass), NA_HOOK(CmdEndRenderPass2),
    NA_HOOK_ALT("vkCmdEndRenderPass2KHR", CmdEndRenderPass2), NA_HOOK(CmdBeginRendering), NA_HOOK_ALT("vkCmdBeginRenderingKHR", CmdBeginRendering),
    NA_HOOK(CmdEndRendering), NA_HOOK_ALT("vkCmdEndRenderingKHR", CmdEndRendering), NA_HOOK(CmdBindPipeline), NA_HOOK(CmdSetViewport),
    NA_HOOK(CmdBindDescriptorSets), NA_HOOK(CmdPushConstants), NA_HOOK(CmdDraw), NA_HOOK(CmdDrawIndexed), NA_HOOK(CmdDrawIndirect),
    NA_HOOK(CmdDrawIndexedIndirect), NA_HOOK(CmdDrawIndirectCount), NA_HOOK_ALT("vkCmdDrawIndirectCountKHR", CmdDrawIndirectCount),
    NA_HOOK_ALT("vkCmdDrawIndirectCountAMD", CmdDrawIndirectCount), NA_HOOK(CmdDrawIndexedIndirectCount),
    NA_HOOK_ALT("vkCmdDrawIndexedIndirectCountKHR", CmdDrawIndexedIndirectCount), NA_HOOK_ALT("vkCmdDrawIndexedIndirectCountAMD", CmdDrawIndexedIndirectCount),
    NA_HOOK(CmdPipelineBarrier), NA_HOOK(CmdPipelineBarrier2), NA_HOOK_ALT("vkCmdPipelineBarrier2KHR", CmdPipelineBarrier2), NA_HOOK(CmdCopyImage),
    NA_HOOK(CmdCopyImage2), NA_HOOK_ALT("vkCmdCopyImage2KHR", CmdCopyImage2), NA_HOOK(CmdBlitImage), NA_HOOK(CmdBlitImage2),
    NA_HOOK_ALT("vkCmdBlitImage2KHR", CmdBlitImage2), NA_HOOK(CmdResolveImage), NA_HOOK(CmdResolveImage2), NA_HOOK_ALT("vkCmdResolveImage2KHR", CmdResolveImage2),
    NA_HOOK(CmdCopyImageToBuffer), NA_HOOK(CmdCopyImageToBuffer2), NA_HOOK_ALT("vkCmdCopyImageToBuffer2KHR", CmdCopyImageToBuffer2),
    NA_HOOK(CmdClearDepthStencilImage), NA_HOOK(CmdCopyBuffer), NA_HOOK(CmdCopyBuffer2), NA_HOOK_ALT("vkCmdCopyBuffer2KHR", CmdCopyBuffer2),
    NA_HOOK(CmdUpdateBuffer), NA_HOOK(CmdExecuteCommands), NA_HOOK(QueueSubmit), NA_HOOK(QueueSubmit2), NA_HOOK_ALT("vkQueueSubmit2KHR", QueueSubmit2),
    NA_HOOK(QueuePresentKHR),
};
#undef NA_HOOK
#undef NA_HOOK_ALT

PFN_vkVoidFunction FindDeviceHook(const char* name)
{
    for (const Hook& h : kDeviceHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

} // namespace

NA_VK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NaVk_GetDeviceProcAddr(VkDevice device, const char* name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&NaVk_GetDeviceProcAddr);
    DeviceData* dd = device ? DeviceOf(device) : nullptr;
    if (!dd) return nullptr;
    const PFN_vkVoidFunction next = dd->gdpa(device, name);
    if (!dd->inst || !dd->inst->active || !next) return next; // off: nothing of ours in the way
    if (PFN_vkVoidFunction h = FindDeviceHook(name)) return h;
    return next;
}

NA_VK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NaVk_GetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&NaVk_GetInstanceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance_h);
    if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance_h);
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice_h);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&NaVk_GetDeviceProcAddr);
    InstanceData* inst = instance ? InstanceOf(instance) : nullptr;
    if (!inst) return nullptr;
    if (inst->active)
        if (PFN_vkVoidFunction h = FindDeviceHook(name)) {
            // Device functions asked for through the instance: ours, if the driver has them.
            if (inst->gipa(instance, name)) return h;
        }
    return inst->gipa(instance, name);
}

// vk_layer.h already declares this one (extern "C", no dllexport), so MSVC
// refuses a dllexport redefinition (C2375); export it through the linker instead.
#if defined(_WIN32)
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion=_vkNegotiateLoaderLayerInterfaceVersion@4")
#else
#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion")
#endif
#define NA_VK_NEGOTIATE_EXPORT extern "C"
#else
#define NA_VK_NEGOTIATE_EXPORT NA_VK_EXPORT
#endif
NA_VK_NEGOTIATE_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v)
{
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = &NaVk_GetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = &NaVk_GetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
        v->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}

NA_VK_EXPORT int NeuralAliasVk_GetStats(NaVkStats* out)
{
    if (!out || out->size != sizeof(NaVkStats)) return 0;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(*out));
    out->size = size;
    out->dlss_built = na::vk::VkDlss::BuiltWithDlss();
    std::unique_lock<std::shared_mutex> l(g_tables_mu);
    for (auto& kv : g_devices) {
        Host* h = kv.second->host.get();
        if (!h) continue;
        std::lock_guard<std::mutex> l2(h->mu);
        *out = h->stats;
        out->size = size;
        out->samplers_biased = h->stats.samplers_biased;
    }
    out->active = Cfg().enabled;
    std::snprintf(out->log_path, sizeof(out->log_path), "%s", g_log_path_utf8.c_str());
    return 1;
}
