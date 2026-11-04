// A small Vulkan "game" under the real NeuralAlias layer (software Vulkan
// driver on CI: Mesa lavapipe), in the shape of a real one:
//   shadow pass (512x512 depth, its own camera) -> scene pass A (RGBA16F colour
//   + D32 depth, camera block with ViewProjection at byte 64 and its inverse at
//   192, per-object block with dynamic offsets) -> a mid-scene copy of the
//   colour (refraction) -> scene pass B -> the tonemap read (barrier + copy)
//   -> HUD on the swap chain -> present.
// The layer runs with copy_test=2 (red and blue swapped instead of DLSS), so the
// test can see exactly which read got the processed picture.
//
//   vk_layer_test <layer manifest dir> <work dir> plain|jitter|motion
// (motion: copy_test=3 shows the camera motion vectors, checked against the truth;
//  flip: the same, with a D3D-style projection and a negative viewport height;
//  dynamic: Vulkan 1.3 dynamic rendering, half the scene from a secondary command buffer)
// Exit 77 = skipped (no Vulkan device / no headless surface).
#include "host_vk/vk_layer.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include "game_frag_spv.h"
#include "game_vert_spv.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if !defined(_MSC_VER)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

int g_failed = 0;
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failed;
}
#define VKCHECK(x)                                                                         \
    do {                                                                                   \
        const VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                            \
            std::printf("FAIL %s -> %d (line %d)\n", #x, static_cast<int>(r_), __LINE__); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (0)

void SetEnv(const char* k, const std::string& v)
{
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

using Mat = std::array<double, 16>; // row-major storage, column-vector algebra
Mat Mul(const Mat& a, const Mat& b)
{
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
bool Invert(const Mat& m, Mat& out)
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
    return true;
}
// Vulkan clip space (+Y down, depth 0..1), camera at (cx, cy, cz) looking down +Z.
// `flip`: D3D-style clip space (+Y up), drawn with a negative viewport height,
// as many ported engines do.
bool g_flip = false;
// `dynamic`: Vulkan 1.3 dynamic rendering (no render passes), and the second
// half of the scene drawn from a secondary command buffer.
bool g_dynamic = false;
Mat ViewProj(double cx, double cy, double cz, double aspect)
{
    const double fy = 1.0 / std::tan(0.5), fx = fy / aspect, n = 0.1, f = 1000.0;
    const Mat P{fx, 0, 0, 0, 0, g_flip ? fy : -fy, 0, 0, 0, 0, f / (f - n), -n * f / (f - n), 0, 0, 1, 0};
    const Mat V{1, 0, 0, -cx, 0, 1, 0, -cy, 0, 0, 1, -cz, 0, 0, 0, 1};
    return Mul(P, V);
}
void StoreColumnMajor(uint8_t* dst, const Mat& m)
{
    float f[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) f[c * 4 + r] = static_cast<float>(m[r * 4 + c]);
    std::memcpy(dst, f, 64);
}

struct Vk {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    uint32_t qf = 0;
    VkPhysicalDeviceMemoryProperties mem{};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
};

uint32_t MemType(const Vk& v, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < v.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (v.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    std::printf("FAIL no memory type\n");
    std::exit(1);
}

struct Img { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
Img MakeImage(const Vk& v, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    Img i;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VKCHECK(vkCreateImage(v.dev, &ci, nullptr, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v.dev, i.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(v, req.memoryTypeBits, 0);
    VKCHECK(vkAllocateMemory(v.dev, &ai, nullptr, &i.mem));
    VKCHECK(vkBindImageMemory(v.dev, i.image, i.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(v.dev, &vi, nullptr, &i.view));
    return i;
}
void FreeImage(const Vk& v, Img& i)
{
    vkDestroyImageView(v.dev, i.view, nullptr);
    vkDestroyImage(v.dev, i.image, nullptr);
    vkFreeMemory(v.dev, i.mem, nullptr);
    i = Img{};
}

struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* ptr = nullptr; };
Buf MakeBuffer(const Vk& v, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buf b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VKCHECK(vkCreateBuffer(v.dev, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v.dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(v.dev, &ai, nullptr, &b.mem));
    VKCHECK(vkBindBufferMemory(v.dev, b.buf, b.mem, 0));
    void* p = nullptr;
    VKCHECK(vkMapMemory(v.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p));
    b.ptr = static_cast<uint8_t*>(p);
    return b;
}

void ImageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

float Half(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

// ---------------------------------------------------------------- the game
struct Game {
    Vk v;
    uint32_t w = 320, h = 180;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    Img color, depth, shadow;
    Buf cam, shadow_cam, obj, refr_rb, tone_rb;
    VkRenderPass rp_clear = VK_NULL_HANDLE, rp_load = VK_NULL_HANDLE, rp_shadow = VK_NULL_HANDLE, rp_hud = VK_NULL_HANDLE;
    VkFramebuffer fb_scene = VK_NULL_HANDLE, fb_shadow = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> fb_hud;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe_scene = VK_NULL_HANDLE, pipe_shadow = VK_NULL_HANDLE, pipe_hud = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_main = VK_NULL_HANDLE, set_shadow = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE, sec = VK_NULL_HANDLE;
    VkCommandBuffer rec = VK_NULL_HANDLE; // the one being recorded
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    uint64_t frame = 0;
    Mat last_vp{}, prev_vp{};
    std::vector<uint8_t> cam_written; // what the game wrote this frame (before the layer)
};

constexpr uint32_t kObjects = 40;

VkRenderPass MakePass(VkDevice dev, VkFormat cf, VkFormat df, VkAttachmentLoadOp load, VkImageLayout color_initial, VkImageLayout color_final)
{
    VkAttachmentDescription a[2] = {};
    uint32_t n = 0;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (cf != VK_FORMAT_UNDEFINED) {
        a[n] = {0, cf, VK_SAMPLE_COUNT_1_BIT, load, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                color_initial, color_final};
        cref.attachment = n++;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
    }
    if (df != VK_FORMAT_UNDEFINED) {
        a[n] = {0, df, VK_SAMPLE_COUNT_1_BIT, load, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        dref.attachment = n++;
        sub.pDepthStencilAttachment = &dref;
    }
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = n;
    ci.pAttachments = a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass rp;
    VKCHECK(vkCreateRenderPass(dev, &ci, nullptr, &rp));
    return rp;
}

VkPipeline MakePipeline(Game& g, VkRenderPass rp, bool depth, bool color, VkFormat cf = VK_FORMAT_UNDEFINED, VkFormat df = VK_FORMAT_UNDEFINED)
{
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(kGameVertSpv);
    sm.pCode = kGameVertSpv;
    VKCHECK(vkCreateShaderModule(g.v.dev, &sm, nullptr, &vs));
    sm.codeSize = sizeof(kGameFragSpv);
    sm.pCode = kGameFragSpv;
    VKCHECK(vkCreateShaderModule(g.v.dev, &sm, nullptr, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = depth;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = color ? 1 : 0;
    cbs.pAttachments = &cba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = g.pl;
    ci.renderPass = rp;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (!rp) { // dynamic rendering
        pr.colorAttachmentCount = cf != VK_FORMAT_UNDEFINED ? 1 : 0;
        pr.pColorAttachmentFormats = &cf;
        pr.depthAttachmentFormat = df;
        ci.pNext = &pr;
    }
    VkPipeline p;
    VKCHECK(vkCreateGraphicsPipelines(g.v.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    vkDestroyShaderModule(g.v.dev, vs, nullptr);
    vkDestroyShaderModule(g.v.dev, fs, nullptr);
    return p;
}

void MakeSized(Game& g)
{
    // The game asks for no copy or sampling access: the layer adds what DLSS needs.
    g.color = MakeImage(g.v, g.w, g.h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    g.depth = MakeImage(g.v, g.w, g.h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    g.refr_rb = MakeBuffer(g.v, VkDeviceSize(g.w) * g.h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    g.tone_rb = MakeBuffer(g.v, VkDeviceSize(g.w) * g.h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkImageView att[2] = {g.color.view, g.depth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.rp_clear;
    fi.attachmentCount = 2;
    fi.pAttachments = att;
    fi.width = g.w;
    fi.height = g.h;
    fi.layers = 1;
    VKCHECK(vkCreateFramebuffer(g.v.dev, &fi, nullptr, &g.fb_scene));

    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.v.phys, g.v.surface, &caps));
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = g.v.surface;
    sc.minImageCount = std::max(2u, caps.minImageCount);
    sc.imageFormat = g.swap_format;
    sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc.imageExtent = {g.w, g.h};
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.oldSwapchain = g.swap;
    VkSwapchainKHR ns;
    VKCHECK(vkCreateSwapchainKHR(g.v.dev, &sc, nullptr, &ns));
    if (g.swap) vkDestroySwapchainKHR(g.v.dev, g.swap, nullptr);
    g.swap = ns;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g.v.dev, g.swap, &n, nullptr);
    g.swap_images.resize(n);
    vkGetSwapchainImagesKHR(g.v.dev, g.swap, &n, g.swap_images.data());
    for (VkImage im : g.swap_images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.swap_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VKCHECK(vkCreateImageView(g.v.dev, &vi, nullptr, &view));
        g.swap_views.push_back(view);
        VkFramebufferCreateInfo hf{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        hf.renderPass = g.rp_hud;
        hf.attachmentCount = 1;
        hf.pAttachments = &view;
        hf.width = g.w;
        hf.height = g.h;
        hf.layers = 1;
        VkFramebuffer fb;
        VKCHECK(vkCreateFramebuffer(g.v.dev, &hf, nullptr, &fb));
        g.fb_hud.push_back(fb);
    }
}

void FreeSized(Game& g)
{
    vkDeviceWaitIdle(g.v.dev);
    for (VkFramebuffer f : g.fb_hud) vkDestroyFramebuffer(g.v.dev, f, nullptr);
    for (VkImageView v : g.swap_views) vkDestroyImageView(g.v.dev, v, nullptr);
    g.fb_hud.clear();
    g.swap_views.clear();
    vkDestroyFramebuffer(g.v.dev, g.fb_scene, nullptr);
    FreeImage(g.v, g.color);
    FreeImage(g.v, g.depth);
    for (Buf* b : {&g.refr_rb, &g.tone_rb}) {
        vkDestroyBuffer(g.v.dev, b->buf, nullptr);
        vkFreeMemory(g.v.dev, b->mem, nullptr);
        *b = Buf{};
    }
}

bool Init(Game& g)
{
    // NA_VK_TEST_VALIDATION=1: Khronos validation BELOW our layer, so it checks the commands we add too.
    const char* layers[] = {"VK_LAYER_NEURALALIAS_host", "VK_LAYER_KHRONOS_validation"};
    const char* val = std::getenv("NA_VK_TEST_VALIDATION");
    const uint32_t layer_count = val && *val == '1' ? 2 : 1;
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "na-synthetic-vulkan-game";
    app.apiVersion = g_dynamic ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = layer_count;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    const VkResult r = vkCreateInstance(&ici, nullptr, &g.v.inst);
    if (r != VK_SUCCESS) {
        std::printf("SKIP: no Vulkan instance with the layer and a headless surface (%d)\n", static_cast<int>(r));
        return false;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.v.inst, &n, nullptr);
    if (!n) { std::printf("SKIP: no Vulkan device\n"); return false; }
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g.v.inst, &n, pds.data());
    g.v.phys = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.v.phys, &props);
    std::printf("device: %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(g.v.phys, &g.v.mem);
    VkHeadlessSurfaceCreateInfoEXT hs{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    auto create_headless = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(g.v.inst, "vkCreateHeadlessSurfaceEXT"));
    if (!create_headless || create_headless(g.v.inst, &hs, nullptr, &g.v.surface) != VK_SUCCESS) { std::printf("SKIP: no headless surface\n"); return false; }
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    if (g_dynamic) {
        if (props.apiVersion < VK_API_VERSION_1_3) { std::printf("SKIP: no Vulkan 1.3 device for dynamic rendering\n"); return false; }
        f13.dynamicRendering = VK_TRUE;
        dci.pNext = &f13;
    }
    VKCHECK(vkCreateDevice(g.v.phys, &dci, nullptr, &g.v.dev));
    vkGetDeviceQueue(g.v.dev, 0, 0, &g.v.q);

    // Passes: shadow (depth only), scene clear, scene load (after the mid-scene copy), HUD.
    g.rp_shadow = MakePass(g.v.dev, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED);
    g.rp_clear = MakePass(g.v.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    g.rp_load = MakePass(g.v.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_LOAD_OP_LOAD,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    g.rp_hud = MakePass(g.v.dev, g.swap_format, VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(g.v.dev, &dl, nullptr, &g.dsl));
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &g.dsl;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &pc;
    VKCHECK(vkCreatePipelineLayout(g.v.dev, &plc, nullptr, &g.pl));
    g.pipe_scene = g_dynamic ? MakePipeline(g, VK_NULL_HANDLE, true, true, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT)
                             : MakePipeline(g, g.rp_clear, true, true);
    g.pipe_shadow = g_dynamic ? MakePipeline(g, VK_NULL_HANDLE, true, false, VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT)
                              : MakePipeline(g, g.rp_shadow, true, false);
    g.pipe_hud = MakePipeline(g, g.rp_hud, false, true);

    g.cam = MakeBuffer(g.v, 512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    g.shadow_cam = MakeBuffer(g.v, 512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    g.obj = MakeBuffer(g.v, 256 * kObjects, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for (uint32_t i = 0; i < kObjects; ++i) {
        const float off[4] = {0.f, 0.f, 0.05f * i, 0.f};
        std::memcpy(g.obj.ptr + 256 * i, off, 16);
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VKCHECK(vkCreateDescriptorPool(g.v.dev, &pci, nullptr, &g.pool));
    VkDescriptorSetLayout two[2] = {g.dsl, g.dsl};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = g.pool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = two;
    VKCHECK(vkAllocateDescriptorSets(g.v.dev, &dai, sets));
    g.set_main = sets[0];
    g.set_shadow = sets[1];
    for (int s = 0; s < 2; ++s) {
        VkDescriptorBufferInfo cbi{s == 0 ? g.cam.buf : g.shadow_cam.buf, 0, 512}, obi{g.obj.buf, 0, 16};
        VkWriteDescriptorSet wr[2] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[s], 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cbi, nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[s], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &obi, nullptr}};
        vkUpdateDescriptorSets(g.v.dev, 2, wr, 0, nullptr);
    }
    g.shadow = MakeImage(g.v, 512, 512, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkFramebufferCreateInfo sf{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    sf.renderPass = g.rp_shadow;
    sf.attachmentCount = 1;
    sf.pAttachments = &g.shadow.view;
    sf.width = sf.height = 512;
    sf.layers = 1;
    VKCHECK(vkCreateFramebuffer(g.v.dev, &sf, nullptr, &g.fb_shadow));

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(g.v.dev, &cpi, nullptr, &g.cpool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.cpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(g.v.dev, &cai, &g.cb));
    cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    VKCHECK(vkAllocateCommandBuffers(g.v.dev, &cai, &g.sec));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(g.v.dev, &fci, nullptr, &g.fence));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(g.v.dev, &sci, nullptr, &g.acquire));
    VKCHECK(vkCreateSemaphore(g.v.dev, &sci, nullptr, &g.done));
    MakeSized(g);
    return true;
}

void Draws(Game& g, VkDescriptorSet set, uint32_t first, uint32_t count, const float color[4])
{
    vkCmdPushConstants(g.rec, g.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color);
    for (uint32_t i = first; i < first + count; ++i) {
        const uint32_t off = 256 * i;
        vkCmdBindDescriptorSets(g.rec, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 1, &off);
        vkCmdDraw(g.rec, 3, 1, 0, 0);
    }
}

void Viewport(Game& g, uint32_t w, uint32_t h)
{
    VkViewport vp{0, 0, float(w), float(h), 0, 1};
    if (g_flip) { vp.y = float(h); vp.height = -float(h); }
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(g.rec, 0, 1, &vp);
    vkCmdSetScissor(g.rec, 0, 1, &sc);
}

void MidSceneCopy(Game& g, const VkBufferImageCopy& bc)
{
    ImageBarrier(g.cb, g.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(g.cb, g.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.refr_rb.buf, 1, &bc);
    ImageBarrier(g.cb, g.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

// The same frame with dynamic rendering; scene part B comes from a secondary command buffer.
void DynamicScene(Game& g, const float red[4], const float grey[4])
{
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    // Shadow.
    ImageBarrier(g.cb, g.shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    depth.imageView = g.shadow.view;
    ri.renderArea = {{0, 0}, {512, 512}};
    vkCmdBeginRendering(g.cb, &ri);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_shadow);
    Viewport(g, 512, 512);
    Draws(g, g.set_shadow, 0, 30, grey);
    vkCmdEndRendering(g.cb);
    // Scene A.
    ImageBarrier(g.cb, g.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ImageBarrier(g.cb, g.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = g.color.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.1f, 0.2f, 0.3f, 1.f}};
    depth.imageView = g.depth.view;
    ri.renderArea = {{0, 0}, {g.w, g.h}};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(g.cb, &ri);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_scene);
    Viewport(g, g.w, g.h);
    Draws(g, g.set_main, 0, kObjects / 2, red);
    vkCmdEndRendering(g.cb);
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {g.w, g.h, 1};
    MidSceneCopy(g, bc);
    // Scene B, recorded in a secondary command buffer.
    const VkFormat cf = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkCommandBufferInheritanceRenderingInfo ir{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    ir.colorAttachmentCount = 1;
    ir.pColorAttachmentFormats = &cf;
    ir.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    ir.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkCommandBufferInheritanceInfo ii{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    ii.pNext = &ir;
    VkCommandBufferBeginInfo sb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    sb.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    sb.pInheritanceInfo = &ii;
    VKCHECK(vkResetCommandBuffer(g.sec, 0));
    VKCHECK(vkBeginCommandBuffer(g.sec, &sb));
    g.rec = g.sec;
    vkCmdBindPipeline(g.sec, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_scene);
    Viewport(g, g.w, g.h);
    Draws(g, g.set_main, kObjects / 2, kObjects / 2, red);
    VKCHECK(vkEndCommandBuffer(g.sec));
    g.rec = g.cb;
    color.loadOp = depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(g.cb, &ri);
    vkCmdExecuteCommands(g.cb, 1, &g.sec);
    vkCmdEndRendering(g.cb);
}

// One frame. Returns false if the swap chain could not be used.
void Frame(Game& g)
{
    ++g.frame;
    // The game writes its cameras (main: moving slowly; shadow: square, fixed).
    // The camera moves right and up a little every frame.
    g.prev_vp = g.last_vp;
    const Mat vp = ViewProj(0.02 * double(g.frame), 1.0 + 0.01 * double(g.frame), 0.0, double(g.w) / g.h);
    Mat inv{};
    Invert(vp, inv);
    std::memset(g.cam.ptr, 0, 512);
    const float junk[4] = {0.5f, 0.25f, 3.f, 1.f};
    std::memcpy(g.cam.ptr, junk, 16);
    StoreColumnMajor(g.cam.ptr + 64, vp);
    StoreColumnMajor(g.cam.ptr + 192, inv);
    g.cam_written.assign(g.cam.ptr, g.cam.ptr + 512);
    g.last_vp = vp;
    const Mat svp = ViewProj(3.0, 1.0, -2.0, 1.0);
    Mat sinv{};
    Invert(svp, sinv);
    StoreColumnMajor(g.shadow_cam.ptr + 64, svp);
    StoreColumnMajor(g.shadow_cam.ptr + 192, sinv);

    uint32_t img = 0;
    VKCHECK(vkAcquireNextImageKHR(g.v.dev, g.swap, UINT64_MAX, g.acquire, VK_NULL_HANDLE, &img));
    VKCHECK(vkResetCommandBuffer(g.cb, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(g.cb, &bi));
    g.rec = g.cb;
    const float red[4] = {1.f, 0.25f, 0.f, 1.f}, grey[4] = {0.5f, 0.5f, 0.5f, 1.f};
    if (g_dynamic) {
        DynamicScene(g, red, grey);
    } else {

    // Shadow pass.
    VkClearValue cd{};
    cd.depthStencil = {1.f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.rp_shadow;
    rb.framebuffer = g.fb_shadow;
    rb.renderArea = {{0, 0}, {512, 512}};
    rb.clearValueCount = 1;
    rb.pClearValues = &cd;
    vkCmdBeginRenderPass(g.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_shadow);
    Viewport(g, 512, 512);
    Draws(g, g.set_shadow, 0, 30, grey);
    vkCmdEndRenderPass(g.cb);

    // Scene pass A.
    VkClearValue cv[2]{};
    cv[0].color = {{0.1f, 0.2f, 0.3f, 1.f}};
    cv[1].depthStencil = {1.f, 0};
    rb.renderPass = g.rp_clear;
    rb.framebuffer = g.fb_scene;
    rb.renderArea = {{0, 0}, {g.w, g.h}};
    rb.clearValueCount = 2;
    rb.pClearValues = cv;
    vkCmdBeginRenderPass(g.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_scene);
    Viewport(g, g.w, g.h);
    Draws(g, g.set_main, 0, kObjects / 2, red);
    vkCmdEndRenderPass(g.cb);

    // Mid-scene read (refraction copy): must NOT get the processed picture.
    {
        VkBufferImageCopy mc{};
        mc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        mc.imageExtent = {g.w, g.h, 1};
        MidSceneCopy(g, mc);
    }

    // Scene pass B.
    rb.renderPass = g.rp_load;
    vkCmdBeginRenderPass(g.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_scene);
    Viewport(g, g.w, g.h);
    Draws(g, g.set_main, kObjects / 2, kObjects / 2, red);
    vkCmdEndRenderPass(g.cb);
    }
    VkBufferImageCopy bc{};
    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bc.imageExtent = {g.w, g.h, 1};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderArea = {{0, 0}, {g.w, g.h}};

    // Tonemap read: <- the DLSS point.
    ImageBarrier(g.cb, g.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(g.cb, g.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.tone_rb.buf, 1, &bc);

    // HUD on the swap chain.
    VkClearValue hc{};
    hc.color = {{0.f, 0.f, 0.f, 1.f}};
    rb.renderPass = g.rp_hud;
    rb.framebuffer = g.fb_hud[img];
    rb.clearValueCount = 1;
    rb.pClearValues = &hc;
    vkCmdBeginRenderPass(g.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_hud);
    Draws(g, g.set_main, 0, 4, grey);
    vkCmdEndRenderPass(g.cb);
    VKCHECK(vkEndCommandBuffer(g.cb));

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.acquire;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.done;
    VKCHECK(vkQueueSubmit(g.v.q, 1, &si, g.fence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swap;
    pi.pImageIndices = &img;
    VKCHECK(vkQueuePresentKHR(g.v.q, &pi));
    VKCHECK(vkWaitForFences(g.v.dev, 1, &g.fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(g.v.dev, 1, &g.fence));
}

void Pixel(const Buf& b, uint32_t w, uint32_t x, uint32_t y, float out[4])
{
    uint16_t px[4];
    std::memcpy(px, b.ptr + (size_t(y) * w + x) * 8, 8);
    for (int i = 0; i < 4; ++i) out[i] = Half(px[i]);
}
// Where the surface at world z = 5 (the nearest object) seen at pixel (x, y) was
// last frame, minus where it is now: the camera motion vector DLSS should get.
std::array<double, 2> ExpectedMotion(const Game& g, double x, double y)
{
    Mat inv{};
    Invert(g.last_vp, inv);
    const double nx = x / g.w * 2 - 1, ny = g_flip ? 1 - y / g.h * 2 : y / g.h * 2 - 1;
    auto unproject = [&](double z) {
        std::array<double, 4> p{nx, ny, z, 1}, r{};
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) r[i] += inv[i * 4 + k] * p[k];
        for (int i = 0; i < 3; ++i) r[i] /= r[3];
        return r;
    };
    const auto a = unproject(0.2), b = unproject(0.9);
    const double t = (5.0 - a[2]) / (b[2] - a[2]);
    const std::array<double, 4> w{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), 5.0, 1.0};
    std::array<double, 4> c{};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) c[i] += g.prev_vp[i * 4 + k] * w[k];
    const double px = (c[0] / c[3] + 1) * 0.5 * g.w, py = g_flip ? (1 - c[1] / c[3]) * 0.5 * g.h : (c[1] / c[3] + 1) * 0.5 * g.h;
    return {px - x, py - y};
}

bool Near(const float a[4], float r, float g, float b) { return std::fabs(a[0] - r) < 0.01f && std::fabs(a[1] - g) < 0.01f && std::fabs(a[2] - b) < 0.01f; }

using GetStatsFn = int (*)(NaVkStats*);
GetStatsFn g_get_stats = nullptr;
NaVkStats Stats()
{
    NaVkStats s{};
    s.size = sizeof(s);
    if (!g_get_stats || !g_get_stats(&s)) std::printf("(no stats from the layer)\n");
    return s;
}
void Print(const char* when, const NaVkStats& s)
{
    std::printf("%s: frames=%llu selected=%u %ux%u fmt=%u depth fmt=%u camera=%u at %llu y_down=%u draws=%u scene=%u inserts=%llu "
                "delivered=%llu refused=%llu no_camera=%llu failed=%llu motion=%llu jitter=%llu biased=%llu missed=%llu contra=%llu "
                "not_unique=%llu reason='%s'\n",
                when, (unsigned long long)s.frames, s.selected, s.color_w, s.color_h, s.color_format, s.depth_format, s.camera,
                (unsigned long long)s.camera_offset, s.y_down, s.last_draws, s.last_scene_draws, (unsigned long long)s.recorded_inserts,
                (unsigned long long)s.delivered, (unsigned long long)s.refused, (unsigned long long)s.no_camera, (unsigned long long)s.failed,
                (unsigned long long)s.motion_dispatches, (unsigned long long)s.jitter_writes, (unsigned long long)s.samplers_biased,
                (unsigned long long)s.missed, (unsigned long long)s.contradictions, (unsigned long long)s.tag_not_unique, s.reason);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: vk_layer_test <layer dir> <work dir> plain|jitter|motion|flip|dynamic\n");
        return 2;
    }
    const std::string layer_dir = argv[1], work = argv[2], mode = argv[3];
    g_flip = mode == "flip";
    g_dynamic = mode == "dynamic";                          // motion, drawn D3D-style with a flipped viewport
    const bool motion = mode == "motion" || g_flip; // copy_test=3: the tonemap read shows the motion vectors
    const bool jitter = mode == "jitter" || motion;
    const std::string ini = work + "/NeuralAliasHost-" + mode + ".ini", log = work + "/NeuralAliasVulkan-" + mode + ".log";
    {
        std::ofstream f(ini);
        f << "[vulkan]\nenabled=1\njitter=" << (jitter ? 1 : 0) << "\nmip_bias=-1.0\ncopy_test=" << (motion ? 3 : 2) << "\nlog=" << log << "\n";
    }
    {
        // VK_LAYER_PATH replaces the system's layer folders; keep them when validation is asked for.
        const char* val = std::getenv("NA_VK_TEST_VALIDATION");
        if (!(val && *val == '1')) SetEnv("VK_LAYER_PATH", layer_dir);
    }
    SetEnv("VK_ADD_LAYER_PATH", layer_dir);
    SetEnv("NEURALALIAS_VK_INI", ini);

    Game g;
    if (!Init(g)) return 77;
#if defined(_WIN32)
    HMODULE m = GetModuleHandleA("NeuralAliasVulkan.dll");
    g_get_stats = m ? reinterpret_cast<GetStatsFn>(GetProcAddress(m, "NeuralAliasVk_GetStats")) : nullptr;
#else
    void* m = dlopen((layer_dir + "/libNeuralAliasVulkan.so").c_str(), RTLD_NOW | RTLD_NOLOAD);
    g_get_stats = m ? reinterpret_cast<GetStatsFn>(dlsym(m, "NeuralAliasVk_GetStats")) : nullptr;
#endif
    expect(g_get_stats != nullptr, "the layer is loaded into the game");

    // A mip-mapped sampler made after the device: biased when jitter is on.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = 8.f;
    VkSampler smp;
    VKCHECK(vkCreateSampler(g.v.dev, &sci, nullptr, &smp));

    uint32_t jitter_rows_ok = 0, jitter_rows_bad = 0, jitter_sign_ok = 0, jitter_sign_bad = 0;
    for (int i = 0; i < 40; ++i) {
        Frame(g);
        if (jitter && i >= 5) {
            // What the GPU saw: the camera block after the layer's submit-time write.
            float game[16], seen[16];
            std::memcpy(game, g.cam_written.data() + 64, 64);
            std::memcpy(seen, g.cam.ptr + 64, 64);
            bool rows01 = false, rows23 = true;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    const bool diff = std::fabs(game[c * 4 + r] - seen[c * 4 + r]) > 1e-7f;
                    if (r < 2 && diff) rows01 = true;
                    if (r >= 2 && diff) rows23 = false;
                }
            if (rows01 && rows23) ++jitter_rows_ok;
            else ++jitter_rows_bad;
            // The picture must move by exactly the jitter DLSS is told (pixels, +Y down), in either clip convention.
            const NaVkStats js = Stats();
            Mat seen_m{};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) seen_m[r * 4 + c] = seen[c * 4 + r];
            const std::array<double, 4> wp{0.7, 1.3, 9.0, 1.0};
            auto to_px = [&](const Mat& m) {
                std::array<double, 4> c{};
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) c[r] += m[r * 4 + k] * wp[k];
                const double nx = c[0] / c[3], ny = c[1] / c[3];
                return std::array<double, 2>{(nx + 1) * 0.5 * g.w, g_flip ? (1 - ny) * 0.5 * g.h : (ny + 1) * 0.5 * g.h};
            };
            const auto a = to_px(g.last_vp), b = to_px(seen_m);
            const double dx = b[0] - a[0], dy = b[1] - a[1];
            if (std::fabs(dx - js.jitter_x) < 1e-3 && std::fabs(dy - js.jitter_y) < 1e-3 && (std::fabs(dx) + std::fabs(dy)) > 1e-3) ++jitter_sign_ok;
            else {
                ++jitter_sign_bad;
                if (jitter_sign_bad <= 2) std::printf("jitter: picture moved %.4f %.4f px, DLSS told %.4f %.4f\n", dx, dy, js.jitter_x, js.jitter_y);
            }
        }
    }
    NaVkStats s = Stats();
    Print("phase 1", s);
    expect(s.active == 1, "the ini turned the layer on");
    expect(s.selected && s.color_w == 320 && s.color_h == 180 && s.color_format == VK_FORMAT_R16G16B16A16_SFLOAT &&
               s.depth_format == VK_FORMAT_D32_SFLOAT,
           "scene colour RGBA16F 320x180 and its D32 depth are selected (the 512x512 shadow map is not)");
    expect(s.camera && s.camera_offset == 64, "the camera is found in the vertex uniform buffer at byte 64");
    expect(s.y_down == (g_flip ? 0u : 1u), g_flip ? "the flipped viewport (D3D-style +Y up) is seen" : "the Vulkan clip space (+Y down) is seen from the viewport");
    expect(s.delivered >= 25, "DLSS (copy test) ran at the learned point in most frames");
    expect(s.motion_dispatches >= s.delivered, "camera motion was built for every delivered frame");
    expect(s.contradictions == 0 && s.tag_not_unique == 0, "no scene draw after DLSS, and the point is unique");
    expect(s.last_draws == 30 + kObjects + 4 && s.last_scene_draws == kObjects, "draws counted once each (74 per frame, 40 of them scene)");
    float tone[4], refr[4];
    Pixel(g.tone_rb, g.w, g.w / 2, g.h / 2, tone);
    Pixel(g.refr_rb, g.w, g.w / 2, g.h / 2, refr);
    std::printf("tonemap read centre = %.3f %.3f %.3f, mid-scene read centre = %.3f %.3f %.3f\n", tone[0], tone[1], tone[2], refr[0], refr[1], refr[2]);
    if (motion) {
        for (const auto& at : {std::array<uint32_t, 2>{g.w / 2, g.h / 2}, std::array<uint32_t, 2>{g.w / 4, g.h / 3}}) {
            float mv[4];
            Pixel(g.tone_rb, g.w, at[0], at[1], mv);
            const auto e = ExpectedMotion(g, at[0] + 0.5, at[1] + 0.5);
            std::printf("motion at (%u,%u): layer %.4f %.4f, expected %.4f %.4f px\n", at[0], at[1], mv[0], mv[1], e[0], e[1]);
            expect(std::fabs(mv[0] - e[0]) < 0.02 && std::fabs(mv[1] - e[1]) < 0.02 && std::fabs(e[0]) > 0.3 && std::fabs(e[1]) > 0.2,
                   "camera motion vectors match the true motion (right and up camera, jittered frames, Vulkan +Y down)");
        }
    } else {
        expect(Near(tone, 0.f, 0.25f, 1.f), "the tonemap read sees the processed scene (red and blue swapped)");
    }
    expect(Near(refr, 1.f, 0.25f, 0.f), "the mid-scene copy sees the scene as the game drew it");
    if (jitter) {
        std::printf("jitter: rows 0/1 changed only in %u frames, wrong in %u\n", jitter_rows_ok, jitter_rows_bad);
        expect(s.jitter_writes >= 25 && jitter_rows_ok >= 25 && jitter_rows_bad == 0,
               "jitter is written into the game's camera block before the GPU reads it, rows 0/1 only");
        expect(s.samplers_biased == 1, "the mip-mapped sampler got the mip bias");
        std::printf("jitter: picture moved by the told jitter in %u frames, wrong in %u\n", jitter_sign_ok, jitter_sign_bad);
        expect(jitter_sign_ok >= 25 && jitter_sign_bad == 0, "the picture moves by exactly the jitter DLSS is given (direction and size)");
    } else {
        expect(s.jitter_writes == 0 && s.samplers_biased == 0, "no jitter and no mip bias when jitter=0");
    }

    // Resize: new swap chain and scene targets.
    FreeSized(g);
    g.w = 400;
    g.h = 225;
    MakeSized(g);
    const uint64_t before = s.delivered;
    for (int i = 0; i < 20; ++i) Frame(g);
    s = Stats();
    Print("after resize", s);
    expect(s.selected && s.color_w == 400 && s.color_h == 225, "after a resize the new 400x225 scene colour is selected");
    expect(s.delivered >= before + 10, "and DLSS runs again at the new size");
    Pixel(g.tone_rb, g.w, g.w / 2, g.h / 2, tone);
    if (!motion) expect(Near(tone, 0.f, 0.25f, 1.f), "the tonemap read sees the processed scene after the resize");

    vkDeviceWaitIdle(g.v.dev);
    vkDestroySampler(g.v.dev, smp, nullptr);
    std::string text;
    {
        std::ifstream lf(log);
        std::stringstream ss;
        ss << lf.rdbuf();
        text = ss.str();
    }
    expect(text.find("selection: scene colour 320x180") != std::string::npos && text.find("camera: found") != std::string::npos &&
               text.find("resources: 320x180") != std::string::npos && text.find("frame 60:") != std::string::npos,
           "the log has the selection, the camera, the layer's resources and a summary line");
    vkDestroyDevice(g.v.dev, nullptr); // the rest goes with the process
    if (g_failed) std::printf("FAIL: %d check(s) failed\n", g_failed);
    else std::printf("PASS: Vulkan layer on a synthetic game (%s)\n", mode.c_str());
    return g_failed ? 1 : 0;
}
