// The NA1 enhancer on the card: docs/NA1-ENHANCER.md is the contract, na1.hlsl the picture maths
// (kept in step by hand with core/na1_reference.hpp), and this file the Direct3D 12 plumbing around
// them plus the wiring to the Referee's governor (core/enhancer_governor.hpp).
//
// What must stay true here, because the passes are recorded inside a game's own calls:
//   - Prepare and Record never compile a shader and never wait on the GPU. Programs are built on a
//     background worker; until they are ready Prepare returns nothing and the plain picture goes on.
//   - Every time Prepare returns nothing it leaves a plain-English reason in Stats::reason.
//   - A measurement comes back without a wait: the caller reports its submission
//     (Frame::Submitted), a fence of our own is signalled on that queue, and a later Prepare sees
//     the fence has passed and hands the reading to a background worker.
#include "host_dx12/background/work.hpp"
#include "na1.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "core/enhancer_governor.hpp"
#include "core/enhancer_screen_curve.hpp"
#include "core/enhancer_stats.hpp"
#include "core/na1_reference.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(NA_HAVE_NA1)
#include "na1_Prep.h"
#include "na1_BlurX.h"
#include "na1_BlurY.h"
#include "na1_Down.h"
#include "na1_Compose.h"
#include "na1_Measure.h"
#endif

namespace na::na1 {
using Microsoft::WRL::ComPtr;
namespace {

constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// One descriptor table per dispatch, so no pass ever shares or patches another's bindings.
constexpr unsigned kTablePrep = 0, kTableBlurX1 = 1, kTableBlurY1 = 2, kTableDown1 = 3, kTableBlurX2 = 4,
                   kTableBlurY2 = 5, kTableDown2 = 6, kTableBlurX3 = 7, kTableBlurY3 = 8, kTableCompose = 9,
                   kTableMeasure = 10, kTables = 11;
// One program per entry point of na1.hlsl.
constexpr unsigned kProgramPrep = 0, kProgramBlurX = 1, kProgramBlurY = 2, kProgramDown = 3, kProgramCompose = 4,
                   kProgramMeasure = 5, kPrograms = 6;
// Registers t0..t10 and u0..u3 of na1.hlsl, in this order in every table.
constexpr unsigned kSrvPicture = 0, kSrvDepth = 1, kSrvYq = 2, kSrvScalar = 3, kSrvB2 = 4, kSrvYq2 = 5, kSrvB3 = 6,
                   kSrvYq4 = 7, kSrvDeltaA = 8, kSrvDeltaB = 9, kSrvAfter = 10, kSrvs = 11;
constexpr unsigned kUavColour = 0, kUavYq = 1, kUavScalar = 2, kUavCells = 3, kUavs = 4;
constexpr unsigned kTableSize = kSrvs + kUavs;
// Root parameters.
constexpr unsigned kRootFrame = 0, kRootPass = 1, kRootTable = 2;

// cbuffer FrameC of na1.hlsl. Written once per Frame into a small upload buffer in Prepare.
struct FrameConstants {
    uint32_t size[2], size2[2];
    uint32_t size4[2], srgb, reversed;
    float texture, light, colour, key;
    float part_a, part_b;
    uint32_t has_a, has_b;
    float mid, m0, w_dark, p85;
    float p97, tint[3];
    int32_t curve_mode;
    float curve_x0, curve_dx, curve_mean_y;
    float curve_cy[16];
    float measure_key;
    uint32_t unchanged, pad[2];
};
static_assert(sizeof(FrameConstants) == 192, "HLSL cbuffer FrameC layout");
static_assert(offsetof(FrameConstants, size4) == 16 && offsetof(FrameConstants, texture) == 32 &&
              offsetof(FrameConstants, part_a) == 48 && offsetof(FrameConstants, mid) == 64 &&
              offsetof(FrameConstants, p97) == 80 && offsetof(FrameConstants, curve_mode) == 96 &&
              offsetof(FrameConstants, curve_cy) == 112 && offsetof(FrameConstants, measure_key) == 176,
              "HLSL cbuffer FrameC layout");
constexpr uint64_t kConstantBytes = 256;  // one constant-buffer block
static_assert(sizeof(FrameConstants) <= kConstantBytes, "FrameC fits its buffer");

// cbuffer PassC of na1.hlsl: root constants, set before each dispatch.
struct PassConstants {
    uint32_t lsize[2];
    int32_t taps, spacing;
    float half_radius;
    uint32_t pad[3];
};
static_assert(sizeof(PassConstants) == 32, "HLSL cbuffer PassC layout");
static_assert(offsetof(PassConstants, taps) == 8 && offsetof(PassConstants, half_radius) == 16, "HLSL cbuffer PassC layout");

// The measurement: 128 x 72 cells of 24 floats, then the two GPU timestamps.
constexpr uint32_t kCellCount = ref::kCellsW * ref::kCellsH;
constexpr uint32_t kCellBytes = kCellCount * ref::kCellFloats * 4u;
constexpr uint32_t kStampBytes = 16;
constexpr uint32_t kReadbackBytes = kCellBytes + kStampBytes;
static_assert(kCellBytes % 8 == 0, "timestamps are resolved at an 8-byte offset");

constexpr uint64_t kSampleMs = 250;       // one measurement every quarter second, one on its way at most
constexpr uint64_t kLostMs = 2000;        // a measurement whose submission was never reported is given up
constexpr uint64_t kStuckMs = 10000;      // and so is one the GPU never finished
constexpr uint64_t kPairFreshMs = 5000;   // how long a fitted display curve is believed
constexpr uint64_t kRetryBuildMs = 10000; // a failed program build is tried again after this
constexpr float kKnotShare = 0.3f;        // how far the fitted curve moves toward each new fit
constexpr float kMeanShare = 0.3f;        // the same for the assumed curve's mean brightness
constexpr float kGlideShare = 0.15f;      // the glide's share of the remaining gap when no frame time is given
constexpr size_t kFramesMost = 12;
constexpr uint32_t kPairMost = 16384;     // thumbnail cells accepted from the Referee

// The colour formats NA1 handles (contract section 1). An sRGB-typed 8-bit view counts as 8-bit.
constexpr unsigned kColourFormats = 4;
constexpr DXGI_FORMAT kColourFormat[kColourFormats] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                       DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
int colour_index(DXGI_FORMAT stated)
{
    if (stated == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) stated = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (unsigned i = 0; i < kColourFormats; ++i)
        if (kColourFormat[i] == stated) return int(i);
    return -1;
}
// Can a texture of format `own` be viewed as `view`? Only the exact format or its typeless parent:
// anything else would be a device error, not a refusal.
bool viewable(DXGI_FORMAT own, DXGI_FORMAT view)
{
    if (own == view) return true;
    switch (view) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return own == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return own == DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return own == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT: return own == DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16_FLOAT: return own == DXGI_FORMAT_R16_TYPELESS;
    default: return false;
    }
}
bool plain_texture(const D3D12_RESOURCE_DESC& d, uint32_t w, uint32_t h)
{
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == w && d.Height == h &&
           d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1;
}
bool layer_usable(const Delta& d, uint32_t w, uint32_t h)
{
    if (!d.layer || (d.format != DXGI_FORMAT_R16_FLOAT && d.format != DXGI_FORMAT_R32_FLOAT)) return false;
    const D3D12_RESOURCE_DESC desc = d.layer->GetDesc();
    return plain_texture(desc, w, h) && viewable(desc.Format, d.format);
}

void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

// Root signature and one pipeline per pass, for one device. Built on a background worker.
struct Programs {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso[kPrograms];
    bool store[kColourFormats] = {};  // can a compute pass write this colour format on this device
    std::atomic<int> ready{0};        // 0 building, 1 ready, -1 failed (error says why)
    std::mutex mutex;
    std::condition_variable cv;
    std::string error;
    bool permanent = false;           // a failure that trying again cannot cure
    uint64_t asked_ms = 0;
};

#if defined(NA_HAVE_NA1)
// Asked once per device and format, here on the worker, never while recording.
bool typed_store(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s{};
    s.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s)))) return false;
    return (s.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
           (s.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
#endif

bool build_programs(Programs& p)
{
#if !defined(NA_HAVE_NA1)
    p.error = "NA1 shaders were not included in this build.";
    p.permanent = true;
    return false;
#else
    na::observe::HostWorkScope own;
    char text[160];
    if (!typed_store(p.device.Get(), DXGI_FORMAT_R32_FLOAT) || !typed_store(p.device.Get(), DXGI_FORMAT_R32G32_FLOAT)) {
        p.error = "This graphics card cannot write NA1's working textures from a compute pass.";
        p.permanent = true;
        return false;
    }
    for (unsigned i = 0; i < kColourFormats; ++i) p.store[i] = typed_store(p.device.Get(), kColourFormat[i]);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrvs;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kUavs;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[kRootFrame].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b0: FrameC
    rp[kRootFrame].Descriptor.ShaderRegister = 0;
    rp[kRootPass].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b1: PassC
    rp[kRootPass].Constants.ShaderRegister = 1;
    rp[kRootPass].Constants.Num32BitValues = sizeof(PassConstants) / 4;
    rp[kRootTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[kRootTable].DescriptorTable.NumDescriptorRanges = 2;
    rp[kRootTable].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = rp;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob, problem;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &problem);
    if (SUCCEEDED(hr)) hr = p.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root));
    if (FAILED(hr)) {
        std::snprintf(text, sizeof(text), "NA1's root signature could not be prepared (error 0x%08lX).", static_cast<unsigned long>(hr));
        p.error = text;
        return false;
    }
    struct Code { const void* bytes; size_t size; const char* name; };
    const Code code[kPrograms] = {
        {na_na1_Prep, sizeof(na_na1_Prep), "Prep"},          {na_na1_BlurX, sizeof(na_na1_BlurX), "BlurX"},
        {na_na1_BlurY, sizeof(na_na1_BlurY), "BlurY"},       {na_na1_Down, sizeof(na_na1_Down), "Down"},
        {na_na1_Compose, sizeof(na_na1_Compose), "Compose"}, {na_na1_Measure, sizeof(na_na1_Measure), "Measure"}};
    for (unsigned i = 0; i < kPrograms; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = p.root.Get();
        d.CS.pShaderBytecode = code[i].bytes;
        d.CS.BytecodeLength = code[i].size;
        hr = p.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&p.pso[i]));
        if (FAILED(hr)) {
            std::snprintf(text, sizeof(text), "NA1's %s program could not be prepared (error 0x%08lX).", code[i].name,
                          static_cast<unsigned long>(hr));
            p.error = text;
            return false;
        }
    }
    return true;
#endif
}

bool texture(ID3D12Device* dev, ID3D12CommandQueue* queue, uint32_t w, uint32_t h, DXGI_FORMAT format, na::gpu::Texture& out)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return na::gpu::CreateTexture(dev, queue, d, kRead, out);
}
bool buffer(ID3D12Device* dev, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
            ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out.ReleaseAndGetAddressOf())));
}

// The working textures. They rest in the read state and are flipped to the write state around the
// one dispatch that writes each. With a known queue one set is shared by every Frame on it, in GPU
// order; without one each Frame has its own.
struct Scratch {
    na::gpu::Texture yq, b1x, b1, yq2, b2x, b2, yq4, b3x, b3, color;
    ComPtr<ID3D12Resource> cells;  // the Measure pass's sums; rests in the write state
    ComPtr<ID3D12CommandQueue> queue;
    uint32_t w = 0, h = 0, w2 = 0, h2 = 0, w4 = 0, h4 = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
std::shared_ptr<Scratch> make_scratch(ID3D12Device* dev, ID3D12CommandQueue* queue, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
    auto s = std::make_shared<Scratch>();
    s->w = w;
    s->h = h;
    s->w2 = ref::HalfSize(w);
    s->h2 = ref::HalfSize(h);
    s->w4 = ref::HalfSize(s->w2);
    s->h4 = ref::HalfSize(s->h2);
    s->format = format;
    s->queue = queue;
    const DXGI_FORMAT one = DXGI_FORMAT_R32_FLOAT, two = DXGI_FORMAT_R32G32_FLOAT;
    const bool made = texture(dev, queue, w, h, two, s->yq) && texture(dev, queue, w, h, one, s->b1x) &&
                      texture(dev, queue, w, h, one, s->b1) && texture(dev, queue, s->w2, s->h2, two, s->yq2) &&
                      texture(dev, queue, s->w2, s->h2, one, s->b2x) && texture(dev, queue, s->w2, s->h2, one, s->b2) &&
                      texture(dev, queue, s->w4, s->h4, two, s->yq4) && texture(dev, queue, s->w4, s->h4, one, s->b3x) &&
                      texture(dev, queue, s->w4, s->h4, one, s->b3) && texture(dev, queue, w, h, format, s->color) &&
                      buffer(dev, kCellBytes, D3D12_HEAP_TYPE_DEFAULT, kWrite, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, s->cells);
    return made ? s : nullptr;
}

// One measurement on its way from a recorded frame to the governor. There is one of these at a
// time; it is replaced only when a measurement is given up, so that a late command list cannot
// write into a buffer a newer measurement is using.
struct Probe {
    enum State { kIdle, kHanded, kRecorded, kSubmitted, kReading };
    std::mutex m;
    State state = kIdle;
    uint32_t serial = 0;              // which Frame the measurement belongs to
    ComPtr<ID3D12Resource> readback;  // cells, then two timestamps
    ComPtr<ID3D12QueryHeap> queries;  // absent: no GPU time, everything else works
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0, frequency = 0;
    bool timed = false;
    // What the measured frame was made with.
    uint64_t started_ms = 0;
    ref::Dials applied;
    float frame_ms = 0;
    bool reset = false, curve_measured = false;
    na::enhancer::Governor::Part part[2];
};
std::shared_ptr<Probe> make_probe(ID3D12Device* dev)
{
    auto p = std::make_shared<Probe>();
    if (!buffer(dev, kReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, p->readback) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence))))
        return nullptr;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&p->queries)))) p->queries.Reset();
    return p;
}

struct Counters {
    std::atomic<uint64_t> recorded{0};
    std::atomic<const char*> refused{nullptr};  // why the last Record recorded nothing; a string literal
};

// Everything a Frame carries from Prepare to Record.
struct FrameWork {
    std::shared_ptr<Programs> programs;
    std::shared_ptr<Scratch> scratch;
    std::shared_ptr<Counters> counters;
    std::shared_ptr<Probe> probe;  // set when this frame measures
    uint32_t probe_serial = 0;
    ComPtr<ID3D12DescriptorHeap> heap;
    unsigned stride = 0;
    ComPtr<ID3D12Resource> constants;  // FrameC, upload heap, stays mapped
    void* mapped = nullptr;
    ComPtr<ID3D12Resource> color, depth, delta[2];  // the caller's inputs, kept alive with the Frame
    bool unchanged = false;  // every dial at zero: only the measurement runs
    bool prepared = false;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned table, unsigned slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(table * kTableSize + slot) * SIZE_T(stride);
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned table) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(table * kTableSize) * UINT64(stride);
        return h;
    }
    void srv(unsigned table, unsigned slot, ID3D12Resource* r, DXGI_FORMAT format) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        programs->device->CreateShaderResourceView(r, &d, cpu(table, slot));
    }
    void uav(unsigned table, unsigned slot, ID3D12Resource* r, DXGI_FORMAT format) const
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        programs->device->CreateUnorderedAccessView(r, nullptr, &d, cpu(table, kSrvs + slot));
    }
    void raw_uav(unsigned table, unsigned slot, ID3D12Resource* r, uint32_t bytes) const
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = bytes / 4;
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        programs->device->CreateUnorderedAccessView(r, nullptr, &d, cpu(table, kSrvs + slot));
    }
};

// Everything the Pass owns. Shared with the background job that reads a measurement, so that a
// Pass can go away while a reading is still on a worker.
struct Core {
    mutable std::mutex mutex;
    std::shared_ptr<Programs> programs;
    std::shared_ptr<Scratch> scratch;
    std::vector<std::shared_ptr<Frame>> frames;
    std::shared_ptr<Counters> counters = std::make_shared<Counters>();
    std::shared_ptr<Probe> probe;
    uint64_t allocations = 0, busy = 0;
    std::string reason = "Waiting for the first DLAA frame.";

    // The Referee's control rule, and what the last frame was really drawn with.
    na::enhancer::Governor governor;
    ref::Dials applied;
    bool pinned = false;
    ref::Dials pin_dials;
    ref::Scene pin_scene;
    uint64_t last_sample_ms = 0;
    bool sampled = false;
    bool reset_pending = false;  // a camera cut since the last measurement
    uint32_t lost = 0;           // measurements in a row that never came back
    float frame_ms = 0, gpu_ms = 0;
    char governor_line[1024] = {};
    bool governor_pending = false;

    // The display curve.
    ref::Curve curve;            // the one in use
    bool curve_chosen = false;
    std::string curve_why;
    ref::Curve fitted;
    bool have_fit = false, fit_unstamped = false, pair_seen = false, pair_sdr = false;
    uint64_t fit_ms = 0;
    std::string fit_why;         // why the last pair gave no fit
    float mean_y = 0;
    bool have_mean = false;
    char curve_line[512] = {};
    bool curve_pending = false;

    std::vector<float> cells;    // the last measurement that came back
    uint64_t measured = 0;
};

// One Observe of the governor, and its log line when something changed.
void tell_governor(Core& s, const na::enhancer::Governor::Sample& sample)
{
    if (s.pinned) return;  // a test holds the dials
    if (!s.governor.Observe(sample)) return;
    int used = s.governor.Format(s.governor_line, sizeof(s.governor_line));
    if (used < 0) used = 0;
    if (size_t(used) < sizeof(s.governor_line))
        std::snprintf(s.governor_line + used, sizeof(s.governor_line) - size_t(used), " | gpu %.2f ms frame %.1f ms",
                      double(s.gpu_ms), double(s.governor.FrameMedianMs()));
    s.governor_pending = true;
}
// A sample tick with nothing measured: the governor still sees the frame time.
void observe_nothing(Core& s, uint64_t now)
{
    na::enhancer::Governor::Sample sample;
    sample.frame_ms = s.frame_ms;
    sample.now_ms = now;
    sample.reset = s.reset_pending;
    s.reset_pending = false;
    tell_governor(s, sample);
}

// Runs on a background worker once the GPU has finished the measured frame.
void read_probe(const std::shared_ptr<Core>& self, const std::shared_ptr<Probe>& probe)
{
    std::vector<float> cells(size_t(kCellCount) * ref::kCellFloats);
    UINT64 stamps[2] = {};
    bool ok = false;
    void* mapped = nullptr;
    const D3D12_RANGE whole{0, kReadbackBytes};
    if (SUCCEEDED(probe->readback->Map(0, &whole, &mapped)) && mapped) {
        std::memcpy(cells.data(), mapped, kCellBytes);
        std::memcpy(stamps, static_cast<const unsigned char*>(mapped) + kCellBytes, kStampBytes);
        const D3D12_RANGE none{0, 0};
        probe->readback->Unmap(0, &none);
        ok = true;
    }
    na::enhancer::Governor::Sample sample;
    bool timed = false, curve_measured = false, reset = false;
    uint64_t frequency = 0;
    {
        std::lock_guard<std::mutex> hold(probe->m);
        sample.applied = probe->applied;
        sample.have_applied = true;
        sample.frame_ms = probe->frame_ms;
        sample.now_ms = probe->started_ms;
        sample.reset = reset = probe->reset;
        sample.part[0] = probe->part[0];
        sample.part[1] = probe->part[1];
        timed = probe->timed;
        frequency = probe->frequency;
        curve_measured = probe->curve_measured;
    }
    if (ok) sample.n = na::enhancer::Summarise(cells.data(), curve_measured);
    if (ok && timed && frequency && stamps[1] >= stamps[0])
        sample.gpu_ms = float(double(stamps[1] - stamps[0]) * 1000.0 / double(frequency));
    {
        Core& s = *self;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (sample.gpu_ms > 0) s.gpu_ms = sample.gpu_ms;
        tell_governor(s, sample);
        if (ok) {
            s.cells = std::move(cells);
            ++s.measured;
            s.lost = 0;
            if (sample.n.valid && std::isfinite(sample.n.mean_y)) {
                s.mean_y = s.have_mean && !reset ? s.mean_y + (sample.n.mean_y - s.mean_y) * kMeanShare : sample.n.mean_y;
                s.have_mean = true;
            }
        }
    }
    // Last, so that whoever sees "idle" also sees the numbers.
    std::lock_guard<std::mutex> hold(probe->m);
    probe->state = Probe::kIdle;
}

// Looks, without waiting, whether the measurement on its way has finished on the GPU, and gives up
// one that never will. Called with the Pass mutex held.
void collect(const std::shared_ptr<Core>& self, uint64_t now, bool age)
{
    Core& s = *self;
    const std::shared_ptr<Probe> probe = s.probe;
    if (!probe) return;
    bool read = false, gone = false;
    {
        std::lock_guard<std::mutex> hold(probe->m);
        if (probe->state == Probe::kSubmitted) {
            const UINT64 done = probe->fence->GetCompletedValue();
            if (done != UINT64_MAX && done >= probe->value) {
                probe->state = Probe::kReading;
                read = true;
            } else if (age && now >= probe->started_ms + kStuckMs) {
                gone = true;
            }
        } else if ((probe->state == Probe::kHanded || probe->state == Probe::kRecorded) && age && now >= probe->started_ms + kLostMs) {
            gone = true;  // thrown away, or its submission was never reported
        }
    }
    if (read) {
        if (!na::background::Submit([self, probe] { read_probe(self, probe); })) {
            // The worker pool is full: the reading waits for the next frame.
            std::lock_guard<std::mutex> hold(probe->m);
            probe->state = Probe::kSubmitted;
        }
        return;
    }
    if (gone) {
        s.probe.reset();
        ++s.lost;
        observe_nothing(s, now);
    }
}

// Which display curve this frame uses, and a log line when the kind or the reason changes.
void choose_curve(Core& s, bool eight_bit, uint64_t now)
{
    if (s.fit_unstamped) {  // a fit arrived since the last frame: its age counts from here
        s.fit_ms = now;
        s.fit_unstamped = false;
    }
    // A fit older than its five seconds is from another moment of the game: it is dropped, so the
    // next fit is taken whole instead of being blended with it.
    if (s.have_fit && (now < s.fit_ms || now - s.fit_ms > kPairFreshMs)) s.have_fit = false;
    ref::Curve c;
    const char* why = "";
    if (eight_bit) {
        c = na::enhancer::DisplayReferredCurve();
        why = "8-bit picture: it is already what the screen shows";
    } else if (s.have_fit && s.pair_sdr) {
        c = s.fitted;
        why = "fitted from the Referee's output and screen thumbnails";
    } else {
        c = na::enhancer::AssumedCurve(s.mean_y);
        why = !s.pair_seen ? "no screen measurement from the Referee yet"
            : !s.pair_sdr ? "the screen is HDR"
            : !s.fit_why.empty() ? s.fit_why.c_str()
            : "the last screen measurement is older than 5 seconds";
    }
    if (!s.curve_chosen || c.mode != s.curve.mode || s.curve_why != why) {
        const char* kind = c.mode == 0 ? "display" : c.mode == 1 ? "fitted" : "assumed";
        int used = std::snprintf(s.curve_line, sizeof(s.curve_line), "mode=%s why=\"%s\"", kind, why);
        if (c.mode == 1 && used > 0 && size_t(used) < sizeof(s.curve_line)) {
            used += std::snprintf(s.curve_line + used, sizeof(s.curve_line) - size_t(used), " x0=%.3f dx=%.3f knots=", double(c.x0), double(c.dx));
            for (unsigned k = 0; k < 16 && used > 0 && size_t(used) < sizeof(s.curve_line); ++k)
                used += std::snprintf(s.curve_line + used, sizeof(s.curve_line) - size_t(used), "%s%.4f", k ? "," : "", double(c.cy[k]));
        } else if (c.mode == 2 && used > 0 && size_t(used) < sizeof(s.curve_line)) {
            std::snprintf(s.curve_line + used, sizeof(s.curve_line) - size_t(used), " mean_y=%.3f", double(c.mean_y));
        }
        s.curve_pending = true;
        s.curve_why = why;
        s.curve_chosen = true;
    }
    s.curve = c;
}

void say(Core& s, const char* text) { s.reason = text; }

}  // namespace

struct Frame::Work : FrameWork {};
struct Pass::State : Core {};

Frame::Frame() : work_(std::make_unique<Work>()) {}
Frame::~Frame() = default;
Pass::Pass() : state_(std::make_shared<State>()) {}
Pass::~Pass() = default;

ID3D12Resource* Frame::Color() const
{
    const FrameWork& w = *work_;
    return w.unchanged || !w.scratch ? nullptr : w.scratch->color.Get();
}
bool Frame::Measures() const { return work_->probe != nullptr; }
ID3D12Resource* Frame::StageForTests(unsigned index) const
{
    const FrameWork& w = *work_;
    if (!w.scratch) return nullptr;
    const Scratch& s = *w.scratch;
    ID3D12Resource* const stages[] = {s.yq.Get(), s.b1x.Get(), s.b1.Get(), s.yq2.Get(), s.b2x.Get(),
                                      s.b2.Get(), s.yq4.Get(), s.b3x.Get(), s.b3.Get()};
    return index < sizeof(stages) / sizeof(stages[0]) ? stages[index] : nullptr;
}

std::shared_ptr<Frame> Pass::Prepare(ID3D12Device* dev, ID3D12CommandQueue* queue, const Inputs& in, const Options& options)
{
    Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!options.enabled) {
        // Off is exactly as before: nothing compiled, nothing allocated. The next switch-on starts
        // from the plain picture and glides in.
        say(s, "NA1 Enhance is switched off.");
        s.applied = ref::Dials{};
        return {};
    }
    const uint64_t now = in.now_ms ? in.now_ms : GetTickCount64();
    if (std::isfinite(in.frame_ms) && in.frame_ms > 0) s.frame_ms = in.frame_ms;
    if (in.reset) s.reset_pending = true;

    if (!dev || !in.color || !in.depth) {
        say(s, "Waiting for the DLAA picture and its depth.");
        return {};
    }
    if (in.width < 16 || in.height < 16) {
        say(s, "The picture is too small for NA1.");
        return {};
    }
    const int format_index = colour_index(in.color_format);
    if (format_index < 0) {
        say(s, "The picture's colour format is not one NA1 handles (16 or 32-bit float, 11/10-bit float, 8-bit RGBA).");
        return {};
    }
    const DXGI_FORMAT format = kColourFormat[format_index];
    const bool eight_bit = format == DXGI_FORMAT_R8G8B8A8_UNORM;  // always sRGB: stored bytes, display-referred
    const D3D12_RESOURCE_DESC cd = in.color->GetDesc();
    if (!plain_texture(cd, in.width, in.height) || cd.MipLevels != 1) {
        say(s, "The picture is not a plain single-sample texture of the stated size.");
        return {};
    }
    if (!viewable(cd.Format, format)) {
        say(s, "The picture's own format does not match the format it was described with.");
        return {};
    }
    const D3D12_RESOURCE_DESC dd = in.depth->GetDesc();
    if (!plain_texture(dd, in.width, in.height)) {
        say(s, "Depth is not the same size as the picture (upscaling or a late depth source), so NA1 is not run.");
        return {};
    }
    if (in.depth_format != DXGI_FORMAT_R32_FLOAT || !viewable(dd.Format, DXGI_FORMAT_R32_FLOAT)) {
        say(s, "Depth is not 32-bit float, so NA1 is not run.");
        return {};
    }

    na::observe::HostWorkScope own;
    if (!s.programs || s.programs->device.Get() != dev) {
        // A new device: everything made for the old one is dropped (its Frames die with their last holder).
        s.programs = std::make_shared<Programs>();
        s.programs->device = dev;
        s.programs->asked_ms = now;
        s.scratch.reset();
        s.frames.clear();
        s.probe.reset();
        const std::shared_ptr<Programs> programs = s.programs;
        if (!na::background::Submit([programs] {
                const bool ok = build_programs(*programs);
                {
                    std::lock_guard<std::mutex> done(programs->mutex);
                    programs->ready.store(ok ? 1 : -1, std::memory_order_release);
                }
                programs->cv.notify_all();
            })) {
            s.programs.reset();
            say(s, "Waiting for a free background worker to prepare NA1.");
            return {};
        }
    }
    const int ready = s.programs->ready.load(std::memory_order_acquire);
    if (ready == 0) {
        say(s, "Preparing NA1 in the background.");
        return {};
    }
    if (ready < 0) {
        s.reason = s.programs->error;  // the build's own words are kept
        // Not final unless it cannot be cured: tried again later, still on a worker.
        if (!s.programs->permanent && now >= s.programs->asked_ms + kRetryBuildMs) s.programs.reset();
        return {};
    }
    if (!s.programs->store[format_index]) {
        say(s, "This graphics card cannot write the picture's colour format from a compute pass, so NA1 is not run.");
        return {};
    }

    collect(state_, now, true);

    // The display curve and the scene values this frame is drawn with.
    choose_curve(s, eight_bit, now);
    const bool due = !s.sampled || now < s.last_sample_ms || now - s.last_sample_ms >= kSampleMs;
    if (!s.pinned && s.governor.Suspended()) {
        // Switched off for time by the Referee. It still has to see the frame time to try again.
        if (due) {
            observe_nothing(s, now);
            s.sampled = true;
            s.last_sample_ms = now;
        }
        s.applied = ref::Dials{};
        s.reason = std::string("Referee: ") + s.governor.Why();
        return {};
    }
    ref::Scene scene = s.pinned ? s.pin_scene : s.governor.Scene();
    if (!s.pinned) scene.curve = s.curve;
    const ref::Dials governed = s.pinned ? s.pin_dials : s.governor.Dials();
    if (s.pinned || in.reset) {
        s.applied = governed;  // a cut is the one moment a step cannot be seen
    } else {
        // A little of the remaining gap each frame, so no step of the governor is visible; a dial
        // that is nearly there lands exactly, so zero really is zero.
        const float glide_ms = s.governor.Settings().glide_ms;
        const float step_ms = std::isfinite(in.frame_ms) && in.frame_ms > 0 ? (std::min)(in.frame_ms, 250.f)
                                                                         : -glide_ms * std::log(1.f - kGlideShare);
        s.applied = na::enhancer::Governor::Glide(s.applied, governed, step_ms, glide_ms);
    }
    const ref::Dials applied = s.applied;
    const bool has[2] = {layer_usable(in.delta[0], in.width, in.height), layer_usable(in.delta[1], in.width, in.height)};
    const bool left_out = (in.delta[0].layer && !has[0]) || (in.delta[1].layer && !has[1]);
    const bool unchanged = applied.texture == 0 && applied.light == 0 && applied.colour == 0 && applied.key == 0 &&
                           (!has[0] || applied.part[0] == 0) && (!has[1] || applied.part[1] == 0);

    // The governor needs numbers even while every dial is at zero, or it could never start: on a
    // sample tick a frame is handed out that only measures.
    bool measure = false;
    if (due) {
        if (!s.probe) s.probe = make_probe(dev);
        if (s.probe) {
            std::lock_guard<std::mutex> hold(s.probe->m);
            measure = s.probe->state == Probe::kIdle;
        }
    }
    const char* state_text = unchanged ? "Measuring only: the Referee has every dial at zero for this scene, so the picture is unchanged."
                                       : "Running.";
    if (s.lost >= 2)
        state_text = unchanged ? "On, but no measurement has come back, so every dial stays at zero and the picture is unchanged."
                               : "Running, but measurements have stopped coming back, so the dials are held.";
    if (unchanged && !measure) {
        say(s, state_text);
        return {};
    }

    std::shared_ptr<Frame> frame;
    for (const auto& f : s.frames)
        if (f.use_count() == 1) {
            frame = f;
            break;
        }
    if (!frame) {
        if (s.frames.size() >= kFramesMost) {
            ++s.busy;
            say(s, "Waiting for an earlier frame to finish on the graphics card.");
            return {};
        }
        frame = std::shared_ptr<Frame>(new Frame);
        s.frames.push_back(frame);
        ++s.allocations;
    }
    FrameWork& w = *frame->work_;
    w.prepared = false;
    w.probe.reset();
    w.programs = s.programs;
    w.counters = s.counters;

    const bool shared = queue != nullptr;
    std::shared_ptr<Scratch> scratch = shared ? s.scratch : w.scratch;
    if (!scratch || scratch->w != in.width || scratch->h != in.height || scratch->format != format || scratch->queue.Get() != queue) {
        scratch = make_scratch(dev, queue, in.width, in.height, format);
        if (!scratch) {
            say(s, "NA1's working textures could not be allocated.");
            return {};
        }
        if (shared) s.scratch = scratch;
    }
    w.scratch = scratch;
    if (!w.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTables * kTableSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&w.heap)))) {
            say(s, "NA1's descriptors could not be allocated.");
            return {};
        }
        w.stride = dev->GetDescriptorHandleIncrementSize(hd.Type);
        // Every slot starts as an empty view, so a pass that names a register it does not use this
        // frame (an absent lighting layer) reads zeros instead of an unwritten descriptor.
        for (unsigned t = 0; t < kTables; ++t) {
            for (unsigned i = 0; i < kSrvs; ++i) w.srv(t, i, nullptr, DXGI_FORMAT_R32_FLOAT);
            for (unsigned i = 0; i < kUavs; ++i) w.uav(t, i, nullptr, DXGI_FORMAT_R32_FLOAT);
        }
    }
    if (!w.constants) {
        const D3D12_RANGE no_read{0, 0};
        if (!buffer(dev, kConstantBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, w.constants) ||
            FAILED(w.constants->Map(0, &no_read, &w.mapped)) || !w.mapped) {
            w.constants.Reset();
            w.mapped = nullptr;
            say(s, "NA1's constants could not be allocated.");
            return {};
        }
    }
    w.color = in.color;
    w.depth = in.depth;
    w.delta[0] = has[0] ? in.delta[0].layer : nullptr;
    w.delta[1] = has[1] ? in.delta[1].layer : nullptr;
    w.unchanged = unchanged;

    // This Frame is free (nothing recorded with it is still pending), so its tables can be rewritten.
    const Scratch& sc = *scratch;
    const DXGI_FORMAT one = DXGI_FORMAT_R32_FLOAT, two = DXGI_FORMAT_R32G32_FLOAT;
    w.srv(kTablePrep, kSrvPicture, in.color, format);
    w.srv(kTablePrep, kSrvDepth, in.depth, one);
    w.uav(kTablePrep, kUavYq, sc.yq.Get(), two);
    struct Level { unsigned x, y, down; ID3D12Resource* yq; ID3D12Resource* after_x; ID3D12Resource* blurred; ID3D12Resource* next; };
    const Level levels[3] = {{kTableBlurX1, kTableBlurY1, kTableDown1, sc.yq.Get(), sc.b1x.Get(), sc.b1.Get(), sc.yq2.Get()},
                             {kTableBlurX2, kTableBlurY2, kTableDown2, sc.yq2.Get(), sc.b2x.Get(), sc.b2.Get(), sc.yq4.Get()},
                             {kTableBlurX3, kTableBlurY3, kTables, sc.yq4.Get(), sc.b3x.Get(), sc.b3.Get(), nullptr}};
    for (const Level& level : levels) {
        w.srv(level.x, kSrvYq, level.yq, two);
        w.uav(level.x, kUavScalar, level.after_x, one);
        w.srv(level.y, kSrvYq, level.yq, two);
        w.srv(level.y, kSrvScalar, level.after_x, one);
        w.uav(level.y, kUavScalar, level.blurred, one);
        if (level.next) {
            w.srv(level.down, kSrvYq, level.yq, two);
            w.srv(level.down, kSrvScalar, level.blurred, one);
            w.uav(level.down, kUavYq, level.next, two);
        }
    }
    w.srv(kTableCompose, kSrvPicture, in.color, format);
    w.srv(kTableCompose, kSrvYq, sc.yq.Get(), two);
    w.srv(kTableCompose, kSrvScalar, sc.b1.Get(), one);
    w.srv(kTableCompose, kSrvB2, sc.b2.Get(), one);
    w.srv(kTableCompose, kSrvYq2, sc.yq2.Get(), two);
    w.srv(kTableCompose, kSrvB3, sc.b3.Get(), one);
    w.srv(kTableCompose, kSrvYq4, sc.yq4.Get(), two);
    w.srv(kTableCompose, kSrvDeltaA, w.delta[0].Get(), has[0] ? in.delta[0].format : one);
    w.srv(kTableCompose, kSrvDeltaB, w.delta[1].Get(), has[1] ? in.delta[1].format : one);
    w.uav(kTableCompose, kUavColour, sc.color.Get(), format);
    w.srv(kTableMeasure, kSrvPicture, in.color, format);
    w.srv(kTableMeasure, kSrvDepth, in.depth, one);
    w.srv(kTableMeasure, kSrvAfter, unchanged ? in.color : sc.color.Get(), format);  // unchanged: before is after
    w.raw_uav(kTableMeasure, kUavCells, sc.cells.Get(), kCellBytes);

    FrameConstants c{};
    c.size[0] = sc.w;
    c.size[1] = sc.h;
    c.size2[0] = sc.w2;
    c.size2[1] = sc.h2;
    c.size4[0] = sc.w4;
    c.size4[1] = sc.h4;
    c.srgb = eight_bit ? 1u : 0u;
    c.reversed = in.reversed ? 1u : 0u;
    c.texture = applied.texture;
    c.light = applied.light;
    c.colour = applied.colour;
    c.key = applied.key;
    c.part_a = has[0] ? applied.part[0] : 0.f;
    c.part_b = has[1] ? applied.part[1] : 0.f;
    c.has_a = has[0] ? 1u : 0u;
    c.has_b = has[1] ? 1u : 0u;
    c.mid = scene.mid;
    c.m0 = scene.m0;
    c.w_dark = scene.w_dark;
    c.p85 = scene.p85;
    c.p97 = scene.p97;
    for (unsigned i = 0; i < 3; ++i) c.tint[i] = scene.tint[i];
    c.curve_mode = scene.curve.mode;
    c.curve_x0 = scene.curve.x0;
    c.curve_dx = scene.curve.dx;
    c.curve_mean_y = scene.curve.mean_y;
    for (unsigned i = 0; i < 16; ++i) c.curve_cy[i] = scene.curve.cy[i];
    c.measure_key = applied.key;
    c.unchanged = unchanged ? 1u : 0u;
    std::memcpy(w.mapped, &c, sizeof(c));

    // The measurement is claimed last, when nothing can fail any more.
    if (measure) {
        Probe& p = *s.probe;
        std::lock_guard<std::mutex> hold(p.m);
        p.state = Probe::kHanded;
        w.probe_serial = ++p.serial;
        p.timed = false;
        p.started_ms = now;
        p.applied = applied;
        if (!has[0]) p.applied.part[0] = 0;
        if (!has[1]) p.applied.part[1] = 0;
        p.frame_ms = s.frame_ms;
        p.reset = s.reset_pending;
        p.curve_measured = scene.curve.mode != 2;  // an assumed curve cannot tell a dark scene from a dim one
        for (unsigned i = 0; i < 2; ++i) {
            p.part[i].present = has[i];
            p.part[i].gpu_ms = in.delta[i].gpu_ms;
            p.part[i].mean_darkening = in.delta[i].mean_darkening;
            p.part[i].worst_tile = in.delta[i].worst_tile;
        }
        w.probe = s.probe;
        s.reset_pending = false;
        s.sampled = true;
        s.last_sample_ms = now;
    }
    w.prepared = true;
    s.reason = state_text;
    if (left_out) s.reason = std::string(unchanged ? "Measuring only" : "Running") + "; a lighting layer was left out: not the picture's size, or not a 16 or 32-bit float layer.";
    return frame;
}

bool Frame::Record(ID3D12GraphicsCommandList* l)
{
    FrameWork& w = *work_;
    if (!w.prepared || !w.programs || !w.scratch || !w.heap || !w.constants) return false;
    if (!l || (l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && l->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
        w.counters->refused.store("NA1 needs a direct or compute command list; this one is neither, so nothing was recorded.",
                                  std::memory_order_relaxed);
        return false;
    }
    const std::shared_ptr<Probe> probe = w.probe;
    bool measure = false, timed = false;
    if (probe) {
        std::lock_guard<std::mutex> hold(probe->m);
        measure = probe->state == Probe::kHanded && probe->serial == w.probe_serial;
        timed = measure && probe->queries && !w.unchanged;
    }
    if (w.unchanged && !measure) return false;  // nothing to do: the picture is unchanged and already measured

    na::observe::HostWorkScope own;
    Scratch& s = *w.scratch;
    const Programs& programs = *w.programs;
    ID3D12DescriptorHeap* heaps[] = {w.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(programs.root.Get());
    l->SetComputeRootConstantBufferView(kRootFrame, w.constants->GetGPUVirtualAddress());
    const auto run = [&](unsigned table, unsigned program, const PassConstants& pc, uint32_t width, uint32_t height) {
        l->SetComputeRoot32BitConstants(kRootPass, sizeof(PassConstants) / 4, &pc, 0);
        l->SetComputeRootDescriptorTable(kRootTable, w.gpu(table));
        l->SetPipelineState(programs.pso[program].Get());
        l->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    };
    const auto into = [&](ID3D12Resource* target, unsigned table, unsigned program, const PassConstants& pc, uint32_t width, uint32_t height) {
        transition(l, target, kRead, kWrite);
        run(table, program, pc, width, height);
        transition(l, target, kWrite, kRead);
    };
    if (!w.unchanged) {
        // GPU time of the passes every enhanced frame pays for (the measurement is not one of them).
        if (timed) l->EndQuery(probe->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        PassConstants full{};
        full.lsize[0] = s.w;
        full.lsize[1] = s.h;
        into(s.yq.Get(), kTablePrep, kProgramPrep, full, s.w, s.h);
        // Blur levels as (radius, spacing): (3,1), (6,1), (12,2), each x then y, halving between.
        struct Level { unsigned x, y, down; uint32_t lw, lh, nw, nh; int radius, spacing; ID3D12Resource* after_x; ID3D12Resource* blurred; ID3D12Resource* next; };
        const Level levels[3] = {{kTableBlurX1, kTableBlurY1, kTableDown1, s.w, s.h, s.w2, s.h2, 3, 1, s.b1x.Get(), s.b1.Get(), s.yq2.Get()},
                                 {kTableBlurX2, kTableBlurY2, kTableDown2, s.w2, s.h2, s.w4, s.h4, 6, 1, s.b2x.Get(), s.b2.Get(), s.yq4.Get()},
                                 {kTableBlurX3, kTableBlurY3, kTables, s.w4, s.h4, 0, 0, 12, 2, s.b3x.Get(), s.b3.Get(), nullptr}};
        for (const Level& level : levels) {
            PassConstants pc{};
            pc.lsize[0] = level.lw;
            pc.lsize[1] = level.lh;
            pc.taps = level.radius / level.spacing;
            pc.spacing = level.spacing;
            pc.half_radius = 0.5f * float(level.radius);
            into(level.after_x, level.x, kProgramBlurX, pc, level.lw, level.lh);
            into(level.blurred, level.y, kProgramBlurY, pc, level.lw, level.lh);
            if (level.next) into(level.next, level.down, kProgramDown, pc, level.nw, level.nh);
        }
        into(s.color.Get(), kTableCompose, kProgramCompose, full, s.w, s.h);
        if (timed) l->EndQuery(probe->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    }
    if (measure) {
        // The picture before and after, reduced to cells and copied out in this same list. It is
        // read on a later frame, once the caller has reported the submission and the GPU is done.
        PassConstants cells{};
        cells.lsize[0] = s.w;
        cells.lsize[1] = s.h;
        run(kTableMeasure, kProgramMeasure, cells, ref::kCellsW, ref::kCellsH);
        transition(l, s.cells.Get(), kWrite, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(probe->readback.Get(), 0, s.cells.Get(), 0, kCellBytes);
        transition(l, s.cells.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kWrite);
        if (timed) l->ResolveQueryData(probe->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, probe->readback.Get(), kCellBytes);
        std::lock_guard<std::mutex> hold(probe->m);
        if (probe->state == Probe::kHanded && probe->serial == w.probe_serial) {
            probe->state = Probe::kRecorded;
            probe->timed = timed;
        }
    }
    w.counters->refused.store(nullptr, std::memory_order_relaxed);
    w.counters->recorded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Frame::Submitted(ID3D12CommandQueue* queue)
{
    FrameWork& w = *work_;
    const std::shared_ptr<Probe> probe = w.probe;
    if (!probe || !queue) return;  // this frame carried no measurement
    na::observe::HostWorkScope own;
    std::lock_guard<std::mutex> hold(probe->m);
    if (probe->state != Probe::kRecorded || probe->serial != w.probe_serial) return;
    UINT64 frequency = 0;
    probe->frequency = probe->timed && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) ? frequency : 0;
    // Signal does not wait: the fence passes when the queue reaches this point, after the list.
    if (FAILED(queue->Signal(probe->fence.Get(), probe->value + 1))) return;  // given up later as lost
    ++probe->value;
    probe->state = Probe::kSubmitted;
}

Stats Pass::GetStats() const
{
    const Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    Stats stats;
    stats.recorded = s.counters->recorded.load(std::memory_order_relaxed);
    stats.allocations = s.allocations;
    stats.busy = s.busy;
    stats.samples = s.governor.Samples();
    stats.compiling = s.programs && s.programs->ready.load(std::memory_order_acquire) == 0;
    stats.gpu_ms = s.gpu_ms;
    stats.texture = s.applied.texture;
    stats.light = s.applied.light;
    stats.colour = s.applied.colour;
    stats.key = s.applied.key;
    const char* refused = s.counters->refused.load(std::memory_order_relaxed);
    std::snprintf(stats.reason, sizeof(stats.reason), "%s", refused ? refused : s.reason.c_str());
    return stats;
}

bool Pass::WaitPrepared(uint32_t milliseconds)
{
    std::shared_ptr<Programs> p;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        p = state_->programs;
    }
    if (!p) return false;
    std::unique_lock<std::mutex> wait(p->mutex);
    return p->cv.wait_for(wait, std::chrono::milliseconds(milliseconds), [&] { return p->ready.load() != 0; }) && p->ready.load() == 1;
}

void Pass::Unavailable(const char* reason)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->reason = reason ? reason : "NA1 was not run this frame.";
}

void Pass::SetScreenPair(const float* output_log2, const float* screen_log2, uint32_t count, bool screen_is_sdr)
{
    Core& s = *state_;
    if (!screen_is_sdr || !output_log2 || !screen_log2 || !count) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pair_seen = true;
        s.pair_sdr = screen_is_sdr;
        if (screen_is_sdr) s.fit_why = "the Referee's thumbnails were empty";
        return;
    }
    // The Referee's screen thumbnail is log2(luma + 1e-4) of the stored 8-bit value; the fit wants
    // linear light, 0..1. An SDR screen is taken as gamma 2.2.
    count = (std::min)(count, kPairMost);
    std::vector<float> screen(count);
    for (uint32_t i = 0; i < count; ++i) {
        const float stored = std::exp2(screen_log2[i]) - na::enhancer::kRefereeLumaFloor;
        const float v = std::isfinite(stored) && stored > 0 ? std::pow((std::min)(stored, 1.f), 2.2f) : 0.f;
        screen[i] = v;
    }
    ref::Curve fit;
    const char* why = nullptr;
    const bool ok = na::enhancer::FitScreenCurve(output_log2, screen.data(), count, fit, &why);

    std::lock_guard<std::mutex> lock(s.mutex);
    s.pair_seen = true;
    s.pair_sdr = true;
    if (!ok) {
        // A good fit is kept for its five seconds; one bad pair does not throw it away.
        s.fit_why = why ? why : "the screen thumbnails gave no curve";
        return;
    }
    s.fit_why.clear();
    if (s.have_fit && !s.reset_pending) {
        // Knots move part of the way to each new fit, so the curve does not jump four times a
        // second. The old curve is read at the new knots' places first: every fit picks its own span.
        for (unsigned k = 0; k < 16; ++k) {
            const float old = ref::Shown(s.fitted, fit.x0 + float(k) * fit.dx);
            fit.cy[k] = old + (fit.cy[k] - old) * kKnotShare;
        }
    }
    s.fitted = fit;
    s.have_fit = true;
    s.fit_unstamped = true;
}

bool Pass::TakeGovernorLine(char* out, size_t n)
{
    Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.governor_pending || !out || !n) return false;
    std::snprintf(out, n, "%s", s.governor_line);
    s.governor_pending = false;
    return true;
}

bool Pass::TakeCurveLine(char* out, size_t n)
{
    Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.curve_pending || !out || !n) return false;
    std::snprintf(out, n, "%s", s.curve_line);
    s.curve_pending = false;
    return true;
}

void Pass::PinForTests(const ref::Dials& dials, const ref::Scene& scene)
{
    Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pinned = true;
    s.pin_dials = dials;
    s.pin_scene = scene;
}

void Pass::UnpinForTests()
{
    Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pinned = false;
    s.applied = ref::Dials{};
}

bool Pass::WaitMeasuredForTests(uint32_t milliseconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            collect(state_, 0, false);
            const std::shared_ptr<Probe> probe = state_->probe;
            if (!probe) return true;
            std::lock_guard<std::mutex> hold(probe->m);
            if (probe->state == Probe::kIdle) return true;
        }
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool Pass::LastCellsForTests(std::vector<float>& cells) const
{
    const Core& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.measured) return false;
    cells = s.cells;
    return true;
}

Pass& ScenePass()
{
    static auto* pass = new Pass;  // lives as long as the process: no destruction order to get wrong at exit
    return *pass;
}

Options CurrentOptions()
{
    Options o;
    o.enabled = na::settings::Get().na1_enabled.load();
    return o;
}

}  // namespace na::na1
