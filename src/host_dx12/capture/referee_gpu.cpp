#include "host_dx12/capture/referee_gpu.hpp"
#include "core/referee.hpp"
#include "host_dx12/background/work.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/settings.hpp"

#include <windows.h>
#include <wrl/client.h>
#ifdef NA_HAVE_REFEREE_SHADERS
#include "referee_thumb.h"
#include "referee_tiles.h"
#include "referee_points.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "core/referee_trace.hpp"
#include "core/referee_placement.hpp"
#include "core/referee_parity.hpp"

using Microsoft::WRL::ComPtr;

namespace na::referee::gpu {
namespace {
std::atomic<bool> g_parity_seen{false}; // referee/parity-role-swap, current presentation

constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
enum Img : uint32_t { kPrev, kInput, kOutput, kScreen, kMotion, kPrevOutput, kPrevScreen, kImages };
constexpr uint32_t kTraceOffset = kResultFloats + 4 * kTileFloats;
constexpr uint32_t kGeometryOffset = kTraceOffset + 4 * kThumbFloats;
constexpr uint32_t kGeometryFloats = 6 * kTileFloats; // prior input, output and up to four screen origins
enum class Phase { Idle, PreRecorded, PreSubmitted, MainRecorded, MainSubmitted, Computing, Scoring, Retiring, Stopped };
constexpr uint32_t kMaxToken = 0x00ffffff; // high 24 bits of the capture token
constexpr ULONGLONG kStuckMs = 3000; // a half-taken sample is given up after this

struct Copy {
    ComPtr<ID3D12Resource> tex;   // the referee's own texture, NON_PIXEL_SHADER_RESOURCE between lists
    D3D12_RESOURCE_DESC desc{};
};

struct VisualEvidence {
    const void* chain = nullptr;
    uint64_t proof = 0, ordinal = 0;
    ULONGLONG at = 0;
    uint8_t losses = 0;
    bool skipped_parity = false, pending = false;
};

struct State {
    std::mutex m;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> thumb, tiles, points;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> results, readback;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandAllocator> trace_alloc;
    ComPtr<ID3D12GraphicsCommandList> trace_list;
    D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12Fence> pre_fence, main_fence, compute_fence;
    UINT64 sample_serial = 0, pre_value = 0, main_value = 0, compute_value = 0;
    bool init_tried = false, init_ok = false;
    Copy img[kImages];
    Phase phase = Phase::Idle;
    ULONGLONG phase_at = 0, next_at = 0, window_at = 0, coverage_at = 0;
    uint64_t observed_dx12 = 0, submitted_dx12 = 0, observed_dx11 = 0, submitted_dx11 = 0;
    shadow::DeliveryContinuity continuity11, continuity12;
    uint64_t pre_ordinal = 0, main_ordinal = 0;
    uint64_t epoch = 0, pre_epoch = 0, main_epoch = 0;
    float pre_jitter[2]{}, main_jitter[2]{}, mv_scale[2]{1, 1};
    const void* swapchain = nullptr;
    const void* screen_only_source = nullptr; // a missed pair can still supply a single exact screen
    bool screen_elapsed = false, have_motion = false, invalidated = false, single_main = false;
    bool pre_trace_output = false, pre_trace_screen = false, pre_accepted = false, adjacent = false;
    bool awaiting_result = false, trace_pair = false, trial_adjacent = false;
    bool trace_geometry = false;
    std::array<Presentation, 4> pending_presents{};
    std::atomic<bool> pending_receipt{false};
    FlickerTransition previous_transition;
    ParityRoleSwap parity; // VARIANT: referee/parity-role-swap
    uint64_t parity_at = 0;
    shadow::Context trace_context;
    const void* trace_source = nullptr;
    uint64_t trace_ordinal = 0;
    ULONGLONG trace_next_at = 0;
    bool trace_parity_skipped = false;
    struct MotionWitness {
        const void* source = nullptr;
        shadow::Context view;
        uint64_t ordinal = 0;
        uint32_t layout = 0, width = 0, height = 0;
        TileLocations locations;
        std::array<JitterVibration, kTiles> window;
        JitterVibration::Estimate reported;
    } geometry;
    std::atomic<uint64_t> trial_sample_ticket{0};
    uint64_t trial_last_ticket = 0, trial_last_ordinal = 0;
    const void* trial_source = nullptr;
    TrialSample trial_sample;
    uint8_t trial_tools = 0, trial_applied = 0;
    uint64_t solo_screens = 0;
    uint32_t token = 0, next_token = 0;
    ComPtr<ID3D12CommandQueue> pre_queue, main_queue;
    bool done_screen = false, done_tiles = false, done_pair = false;
    bool scaled_tiles = false;
    uint32_t padded_layouts = 0;
    uint32_t centered_in_window = 0;
    RegionalTiles regional_anchor;
    shadow::Context regional_context;
    const void* regional_source = nullptr;
    uint64_t regional_ordinal = 0, regional_at = 0;
    unsigned regional_streak = 0;
    float done_jitter_change = 0;
    na::referee::Window window;
    shadow::Context captured_context;
    shadow::Placement observed_alternate;
    shadow::RecentSamples recent;
    TileLocations tile_locations;
    TileLocations sampled_locations;
    shadow::Context tile_context;
    const void* tile_source = nullptr;
    shadow::Context preview_context;
    bool preview_pending = false;
    const void* preview_source = nullptr;
    uint64_t preview_last = 0;
    ULONGLONG preview_at = 0;
    bool preview_exact = false;
    std::vector<VisualEvidence> visual;
    std::atomic<bool> visual_rollback{false};
    std::vector<float> data;
    Stats stats;
    bool keep_receipt = false;
    LastSample last;
};
State& g = *new State; // never destroyed: its D3D12 objects must not be released by the CRT at exit

auto visual_proof(uint64_t proof)
{
    return std::find_if(g.visual.begin(), g.visual.end(), [proof](const VisualEvidence& v) {
        return v.proof == proof;
    });
}

void update_visual_pending()
{
    g.visual_rollback = std::any_of(g.visual.begin(), g.visual.end(),
        [](const VisualEvidence& v) { return v.pending; });
}

void set_phase(Phase p) { g.phase = p; g.phase_at = GetTickCount64(); }
bool enabled() { return na::settings::Get().referee_enabled.load(std::memory_order_relaxed) != 0; }
ULONGLONG every_ms()
{
    const uint32_t v = na::settings::Get().referee_every_ms.load(std::memory_order_relaxed);
    return v < 100 ? 100 : v;
}
void clear_parity_locked()
{
    g.parity.Reset();
    g.parity_at = 0;
    g_parity_seen.store(false, std::memory_order_relaxed);
}
void stop(const char* why)
{
    clear_parity_locked();
    set_phase(Phase::Stopped);
    g.stats.stopped = true;
    g.stats.running = false;
    std::snprintf(g.stats.why, sizeof g.stats.why, "%s", why);
    na::eventlog::Emit("referee_state", "stopped why=\"%s\"", why);
}

DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    default: return f;
    }
}
bool readable(DXGI_FORMAT f)
{
    switch (view_format(f)) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        return true;
    default: return false;
    }
}

bool init(ID3D12Device* dev)
{
    if (g.init_tried) {
        if (g.device.Get() != dev) stop("the evaluation device changed");
        return g.init_ok && g.phase != Phase::Stopped;
    }
    g.init_tried = true;
    g.device = dev;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kImages, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, 12 + kTiles};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable = {1, &range};
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC rd{3, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        stop("the referee's root signature could not be made");
        return false;
    }
#ifdef NA_HAVE_REFEREE_SHADERS
    const D3D12_SHADER_BYTECODE shaders[] = {
        {referee_thumb, sizeof referee_thumb},
        {referee_tiles, sizeof referee_tiles},
        {referee_points, sizeof referee_points}
    };
    ComPtr<ID3D12PipelineState>* psos[3] = {&g.thumb, &g.tiles, &g.points};
    for (int i = 0; i < 3; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.root.Get();
        pd.CS = shaders[i];
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(psos[i]->ReleaseAndGetAddressOf())))) {
            stop("the referee's card programs could not be made");
            return false;
        }
    }
#else
    stop("the referee shaders were unavailable at build time");
    return false;
#endif
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kImages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const UINT64 bytes = UINT64(kGeometryOffset + kGeometryFloats) * sizeof(float);
    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap))) ||
        !buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g.results) ||
        !buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, g.readback) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.pre_fence))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.main_fence))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.compute_fence)))) {
        stop("the referee's buffers could not be made");
        return false;
    }
    g.init_ok = true;
    g.stats.running = true;
    g.window_at = GetTickCount64();
    na::eventlog::Emit("referee_state", "started every_ms=%llu (sampling picture, result, motion, jitter)",
                       static_cast<unsigned long long>(every_ms()));
    return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

// The backend supplies a valid top-left rectangle. Scratch textures contain just that rectangle.
bool ensure(Img which, ID3D12Resource* src, uint32_t width = 0, uint32_t height = 0)
{
    const D3D12_RESOURCE_DESC s = src->GetDesc();
    if (s.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || s.SampleDesc.Count != 1 ||
        s.DepthOrArraySize != 1 || s.MipLevels != 1 || !readable(s.Format)) return false;
    if (!width) width = static_cast<uint32_t>(s.Width);
    if (!height) height = s.Height;
    if (width > s.Width || height > s.Height) return false;
    Copy& c = g.img[which];
    if (c.tex && c.desc.Width == width && c.desc.Height == height && c.desc.Format == s.Format) return true;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{view_format(s.Format), {}, {}};
    D3D12_FEATURE_DATA_FORMAT_INFO info{s.Format, 0};
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info)) || info.PlaneCount != 1 ||
        FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) return false;
    c.tex.Reset();
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = s.Format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, NPSR, nullptr, IID_PPV_ARGS(&c.tex)))) return false;
    c.desc = d;
    return true;
}

// src (in `state`) -> our texture, in `l`. Both end in the states they started in.
void copy(ID3D12GraphicsCommandList* l, Img which, ID3D12Resource* src, D3D12_RESOURCE_STATES state)
{
    ID3D12Resource* dst = g.img[which].tex.Get();
    D3D12_RESOURCE_BARRIER b[3]{};
    UINT n = 0;
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[n++].UAV.pResource = src;
    }
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b[n++] = transition(dst, NPSR, D3D12_RESOURCE_STATE_COPY_DEST);
    l->ResourceBarrier(n, b);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const auto& extent = g.img[which].desc;
    const D3D12_BOX box{0, 0, 0, static_cast<UINT>(extent.Width), extent.Height, 1};
    l->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    n = 0;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    b[n++] = transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
    l->ResourceBarrier(n, b);
}

void emit_window(bool force)
{
    const ULONGLONG now = GetTickCount64();
    const uint32_t log_s = na::settings::Get().referee_log_s.load(std::memory_order_relaxed);
    if (!g.window.samples || (!force && now - g.window_at < ULONGLONG(log_s < 1 ? 1 : log_s) * 1000)) return;
    char line[2048];
    na::referee::Format(line, sizeof line, g.window, na::referee::Judge(g.window));
    na::eventlog::Emit("referee", "%s centered_screen_samples=%u", line, g.centered_in_window);
    if (g.preview_pending) {
        uint64_t first;
        const auto evidence = g.recent.For(g.preview_context, first);
        const auto choice = g.recent.Choose(evidence);
        na::eventlog::Emit("referee_choice",
                           "mode=preview first=%llu last=%llu current=%s generation=%llu samples=%u output=%u input=%u action=%s candidate=%s boundary=%llu why=\"%s\"",
                           static_cast<unsigned long long>(first), static_cast<unsigned long long>(g.preview_last),
                           shadow::Name(evidence.context.placement.path),
                           static_cast<unsigned long long>(evidence.context.generation), evidence.measured.samples,
                           evidence.measured.output_frames, evidence.measured.input_frames, choice.action,
                           shadow::Name(choice.candidate.path), static_cast<unsigned long long>(choice.candidate.boundary), choice.reason);
        g.preview_pending = false;
    }
    ++g.stats.logged;
    g.window = na::referee::Window();
    g.centered_in_window = 0;
    g.window_at = now;
}

bool completed(ID3D12Fence* fence, UINT64 value)
{
    if (!value) return true;
    const UINT64 done = fence->GetCompletedValue();
    if (done == UINT64_MAX) { stop("the device was removed"); return false; }
    return done >= value;
}

void retire()
{
    g.token = 0;
    set_phase(Phase::Retiring);
}

uint32_t sample_floats()
{
    if (g.trace_geometry) return kGeometryOffset + (2 + (std::max)(1u, g.padded_layouts)) * kTileFloats;
    if (g.trace_pair) return kGeometryOffset;
    return kResultFloats + g.padded_layouts * kTileFloats;
}

void collect_sample()
{
    void* p = nullptr;
    D3D12_RANGE r{0, SIZE_T(sample_floats()) * sizeof(float)};
    const bool mapped = SUCCEEDED(g.readback->Map(0, &r, &p)) && p;
    na::referee::SampleScore score;
    uint32_t layout = 0;
    bool scaled_matches = false;
    TileLocations suggested_tiles;
    bool relocate = false;
    FlickerTransition transition;
    TrialSample trial;
    bool trial_valid = false;
    RegionalTiles regions;
    std::array<JitterDisplacement, kTiles> geometry{};
    if (mapped) {
        const auto* samples = static_cast<const float*>(p);
        const auto* padded = samples + kResultFloats;
        layout = RegisterScreenTiles(samples, padded, g.padded_layouts, scaled_matches);
        const float* screen_tiles = layout ? padded + (layout - 1) * kTileFloats : nullptr;
        const bool comparable = g.scaled_tiles || scaled_matches || layout != 0;
        const Sample observations{samples, g.done_screen, comparable, g.done_pair, g.done_jitter_change, screen_tiles};
        score = na::referee::Score(observations);
        shadow::ScreenVote vote = shadow::ScreenVote::Unknown;
        if (g.done_screen && comparable && (!g.padded_layouts || layout || scaled_matches) &&
            (g.trial_adjacent || (score.local.input && score.local.output))) {
            TileVotes local;
            const auto width = uint32_t((std::min)(g.img[kInput].desc.Width, g.img[kScreen].desc.Width));
            const auto height = (std::min)(g.img[kInput].desc.Height, g.img[kScreen].desc.Height);
            vote = shadow::DistinguishTrialOutput(score, observations, width, height, g.sampled_locations, &local);
            regions.Set(local, width, height, g.sampled_locations);
            const float* presented = screen_tiles ? screen_tiles : samples + BlockOffset(kTileScreen);
            for (unsigned i = 0; i < kTiles; ++i) if (regions.output & (1u << i))
                regions.detail[i] = EdgeEnergy(presented + i * kTile * kTile, kTile, kTile);
        }
        if (g.trace_pair && (!g.padded_layouts || layout)) {
            if (g.adjacent)
                transition = LocateTransition(samples + kTraceOffset, samples + BlockOffset(kThumbInput),
                    g.padded_layouts ? samples + kTraceOffset + 3 * kThumbFloats : nullptr,
                    g.captured_context.recovery_region, g.captured_context.recovery_floor);
            if (g.trial_adjacent) {
                const float* screen = g.padded_layouts ? samples + kTraceOffset + 3 * kThumbFloats : samples + BlockOffset(kThumbScreen);
                trial_valid = MeasureTrialScreen(samples + kTraceOffset + 2 * kThumbFloats, screen, trial);
                trial.comparable = trial_valid;
                trial.ordinal = g.main_ordinal;
                trial.context = g.captured_context;
                trial.alternate = g.observed_alternate;
                trial.configuration = g.captured_context.trial_configuration;
                trial.width = uint32_t(g.img[kScreen].desc.Width);
                trial.height = g.img[kScreen].desc.Height;
                trial.layout = layout;
                trial.origin = transition;
                trial.tools = g.trial_tools;
                trial.applied = g.trial_applied;
                if (g.have_motion && score.textured >= 64) {
                    const float* points = samples + BlockOffset(kPoints);
                    float total = 0;
                    unsigned count = 0;
                    for (unsigned i = 0; i < kGridW * kGridH; ++i) {
                        const float* point = points + i * kPointValues;
                        if (point[kStill] < 0 || point[kContrast] < Params{}.point_contrast) continue;
                        total += point[kFollows];
                        ++count;
                    }
                    if (count) trial.motion_error = total / count;
                }
                trial.tiles = regions;
                trial.bypassed = vote == shadow::ScreenVote::Input;
                trial.delivered = vote == shadow::ScreenVote::Output;
            }
            if (g.trace_geometry && trial.delivered && trial.comparable) {
                const float* earlier = samples + kGeometryOffset;
                const float* earlier_screen = earlier + (2 + (layout ? layout - 1 : 0)) * kTileFloats;
                const float* present = screen_tiles ? screen_tiles : samples + BlockOffset(kTileScreen);
                for (unsigned i = 0; i < kTiles; ++i) {
                    if (!(regions.output & (1u << i))) continue;
                    const unsigned at = i * kTile * kTile;
                    geometry[i] = {
                        MeasureTileShift(earlier + at, samples + BlockOffset(kTileInput) + at),
                        MeasureTileShift(earlier + kTileFloats + at, samples + BlockOffset(kTileOutput) + at),
                        MeasureTileShift(earlier_screen + at, present + at),
                        g.main_jitter[0] - g.pre_jitter[0], g.main_jitter[1] - g.pre_jitter[1]};
                }
            }
        }
        // VARIANT: referee/shared-texture-probes
        const bool ambiguous = g.done_tiles && score.tiles_textured >= kTiles / 2 &&
            score.tiles_input + score.tiles_output < 2 && !trial.delivered && !trial.bypassed;
        relocate = g.done_tiles && (score.tiles_textured < kTiles / 2 || ambiguous);
        if (relocate) suggested_tiles = FindTexturedTiles(samples, ambiguous, g.sampled_locations);
        bool keep_receipt;
        {
            std::lock_guard<std::mutex> lock(g.m);
            keep_receipt = g.keep_receipt;
        }
        if (keep_receipt) {
            g.data.assign(samples, samples + kResultFloats);
            if (screen_tiles) std::copy_n(screen_tiles, kTileFloats, g.data.data() + BlockOffset(kTileScreen));
        }
        const D3D12_RANGE none{0, 0};
        g.readback->Unmap(0, &none);
    }
    std::lock_guard<std::mutex> lock(g.m);
    if (mapped) {
        FlickerStage trace = FlickerStage::Unknown;
        if (g.trace_geometry && enabled() && g.preview_exact && !g.invalidated && g.main_epoch == g.epoch) {
            auto& w = g.geometry;
            if (w.source != g.swapchain || !w.view.SameTemporalSequence(g.captured_context) ||
                w.layout != layout || w.width != g.img[kScreen].desc.Width || w.height != g.img[kScreen].desc.Height ||
                g.main_ordinal <= w.ordinal || g.main_ordinal - w.ordinal > 240 ||
                !std::equal(std::begin(w.locations.cell), std::end(w.locations.cell), std::begin(g.sampled_locations.cell))) {
                w = {};
                w.source = g.swapchain;
                w.view = g.captured_context;
                w.layout = layout;
                w.width = uint32_t(g.img[kScreen].desc.Width);
                w.height = g.img[kScreen].desc.Height;
                w.locations = g.sampled_locations;
            }
            w.ordinal = g.main_ordinal;
            unsigned tile = 0;
            JitterVibration::Estimate best;
            for (unsigned i = 0; i < kTiles; ++i) {
                w.window[i].Add(geometry[i]);
                const auto current = w.window[i].Measure(false);
                if (current.valid && (!best.valid || (current.locked && !best.locked) ||
                    (current.locked == best.locked && std::fabs(current.shift) > std::fabs(best.shift)))) {
                    tile = i;
                    best = current;
                }
            }
            if (best.valid && (!w.reported.valid || best.locked != w.reported.locked ||
                std::fabs(best.shift - w.reported.shift) >= .03f)) {
                w.reported = best;
                na::eventlog::Emit("referee_jitter_geometry",
                    "frame=%llu tile_xy=%d,%d samples=%u phase_residual_screen_px=%.4f phase_residual_dlaa_px=%.4f locked=%u",
                    (unsigned long long)g.main_ordinal, regions.at[tile].x, regions.at[tile].y,
                    w.window[tile].count, best.shift, w.window[tile].Measure(true).shift, unsigned(best.locked));
            }
        }
        if (g.trace_pair && g.trial_adjacent && g.preview_exact && !g.invalidated) {
            ++g.stats.trial_pairs;
            if (trial_valid) ++g.stats.trial_comparable;
        }
        if (trial.ordinal && enabled() && g.preview_exact && !g.invalidated) {
            g.trial_sample = trial;
            g.trial_sample.observed_at = GetTickCount64();
            g.trial_source = g.swapchain;
            g.trial_last_ticket = g.captured_context.trial_ticket;
            g.trial_sample_ticket.store(g.trial_last_ticket, std::memory_order_release);
            g.trial_last_ordinal = trial.ordinal;
            // After a non-witness, move the next probe slightly later. A
            // seven-frame render cycle sampled every 21 frames otherwise
            // repeats the same pair even when its parity alternates.
            if (!g.keep_receipt && (!trial.comparable || !trial.delivered)) {
                const uint64_t phase = trial.ordinal * 11400714819323198485ull;
                g.trace_next_at += 20u * ((phase >> 60) + 1);
            }
        }
        if (g.trace_pair && (!g.padded_layouts || layout) && g.main_epoch == g.epoch && g.preview_exact)
            ++g.stats.trace_pairs;
        if (enabled() && g.preview_exact && !g.invalidated && g.main_epoch == g.epoch && transition.stage != FlickerStage::Unknown) {
            if (g.trace_source == g.swapchain && g.trace_context.SameTemporalSequence(g.captured_context) &&
                g.main_ordinal > g.trace_ordinal && g.main_ordinal - g.trace_ordinal <= 240 &&
                Reverses(g.previous_transition, transition)) {
                trace = transition.stage;
                ++g.stats.trace_reversals;
                na::eventlog::Emit("referee_trace", "frames=%llu,%llu first_stage=%s region=%u,%u "
                    "change_stops=%.3f/%.3f/%.3f color=%llu depth=%llu reader=%llu generation=%llu",
                    (unsigned long long)g.pre_ordinal, (unsigned long long)g.main_ordinal, StageName(trace),
                    transition.region % FlickerTransition::columns, transition.region / FlickerTransition::columns,
                    transition.stops[0], transition.stops[1], transition.stops[2],
                    (unsigned long long)g.captured_context.color, (unsigned long long)g.captured_context.depth,
                    (unsigned long long)g.captured_context.native_screen_proof, (unsigned long long)g.captured_context.generation);
            }
            const auto now = GetTickCount64();
            if (g.trace_source != g.swapchain || !g.trace_context.SameScene(g.captured_context) ||
                g.trace_context.generation != g.captured_context.generation ||
                !(g.trace_context.placement == g.captured_context.placement) ||
                g.trace_context.trial_configuration != g.captured_context.trial_configuration ||
                g.main_ordinal <= g.trace_ordinal || g.main_ordinal - g.trace_ordinal > 240)
                clear_parity_locked();
            const auto matched = g.parity.Matched();
            if (g.parity.Observe(g.pre_ordinal, g.main_ordinal, transition.stops)) {
                ++g.stats.parity_recognitions;
                na::eventlog::Emit("referee_parity_swap", "frames=%llu,%llu region=%u,%u change_stops=%.3f/%.3f/%.3f "
                    "matched=%llu brighter=%s color=%llu generation=%llu why=\"the screen flips brightness with frame parity, "
                    "opposite to the DLAA input: the scene targets exchange roles every frame, follow the role\"",
                    (unsigned long long)g.pre_ordinal, (unsigned long long)g.main_ordinal,
                    transition.region % FlickerTransition::columns, transition.region / FlickerTransition::columns,
                    transition.stops[0], transition.stops[1], transition.stops[2],
                    (unsigned long long)g.parity.Matched(), g.parity.Phase() > 0 ? "odd" : "even",
                    (unsigned long long)g.captured_context.color, (unsigned long long)g.captured_context.generation);
            }
            if (g.parity.Matched() != matched) g.parity_at = now;
            g.stats.parity_matched = g.parity.Matched();
            g.trace_source = g.swapchain;
            g.trace_context = g.captured_context;
            g.previous_transition = transition;
            g.trace_ordinal = g.main_ordinal;
            g_parity_seen.store(g.parity.Recognized(), std::memory_order_relaxed);
        }
        if (enabled() && g.preview_exact && !g.invalidated && g.main_epoch == g.epoch && regions.Mixed()) {
            const auto now = GetTickCount64();
            const bool same = g.regional_streak && g.regional_source == g.swapchain &&
                g.captured_context.SameTemporalSequence(g.regional_context) && g.main_ordinal > g.regional_ordinal &&
                g.main_ordinal - g.regional_ordinal <= 240 && now - g.regional_at <= 4000 &&
                g.regional_anchor.Compare(regions) == RegionalTiles::Match::Original;
            g.regional_streak = same ? (std::min)(g.regional_streak + 1, 4u) : 1;
            if (!same) g.regional_anchor = regions;
            if (g.regional_streak == 3) {
                unsigned i = 0, c = 0;
                while (!(regions.input & (1u << i))) ++i;
                while (!(regions.output & (1u << c))) ++c;
                na::eventlog::Emit("referee_composite",
                    "frame=%llu path=%s input_xy=%d,%d control_xy=%d,%d tile_size=%u input_mask=%u output_mask=%u",
                    (unsigned long long)g.main_ordinal, shadow::Name(g.captured_context.placement.path),
                    regions.at[i].x, regions.at[i].y, regions.at[c].x, regions.at[c].y, kTile,
                    unsigned(regions.input), unsigned(regions.output));
            }
            g.regional_context = g.captured_context;
            g.regional_source = g.swapchain;
            g.regional_at = now;
            g.regional_ordinal = g.main_ordinal;
        } else g.regional_streak = 0;
        score.verified_mixed = g.regional_streak >= 3;
        if (layout) ++g.centered_in_window;
        g.window.Add(score);
        if (relocate && g.preview_exact && g.main_epoch == g.epoch) {
            g.tile_locations = suggested_tiles;
            g.tile_source = g.swapchain;
            g.tile_context = g.captured_context;
        }
        if (g.captured_context.placement.path != shadow::Path::None && g.main_epoch == g.epoch) {
            const ULONGLONG now = GetTickCount64();
            if (g.preview_source != g.swapchain || now - g.preview_at > every_ms() * shadow::RecentSamples::capacity + kStuckMs)
                g.recent.Clear();
            g.preview_at = now;
            g.preview_source = g.swapchain;
            g.preview_context = g.captured_context;
            const bool exact = g.done_screen && g.preview_exact;
            g.recent.Add(g.preview_context, g.main_ordinal, exact ? score : SampleScore{}, exact ? g.observed_alternate : shadow::Placement{});
            g.preview_pending = true;
            g.preview_last = g.main_ordinal;
        }
        const uint64_t proof = g.captured_context.native_screen_proof;
        if (enabled() && g.main_epoch == g.epoch && proof && g.captured_context.placement.path == shadow::Path::Native11 &&
            g.done_screen && g.preview_exact && !g.invalidated) {
            const auto vote = shadow::VoteScreen(score);
            const ULONGLONG now = GetTickCount64();
            auto it = visual_proof(proof);
            if (it == g.visual.end()) {
                if (g.visual.size() >= 24) {
                    const auto oldest = std::min_element(g.visual.begin(), g.visual.end(),
                        [](const VisualEvidence& a, const VisualEvidence& b) { return a.at < b.at; });
                    g.visual.erase(oldest);
                }
                g.visual.push_back({g.swapchain, proof});
                it = g.visual.end() - 1;
            }
            auto& evidence = *it;
            if (evidence.chain != g.swapchain || now - evidence.at > every_ms() * 4 + kStuckMs)
                evidence = {g.swapchain, proof};
            evidence.ordinal = g.main_ordinal;
            evidence.skipped_parity = false;
            if (vote != shadow::ScreenVote::Unknown) {
                evidence.at = now;
                evidence.losses = uint8_t(((evidence.losses << 1) | (vote == shadow::ScreenVote::Input)) & 15u);
                // Two input votes from the same reader, even when another
                // independently verified order appears between these samples.
                evidence.pending = (evidence.losses & (evidence.losses - 1)) != 0;
            }
            update_visual_pending();
        }
        ++g.stats.samples;
        if (!g.data.empty()) {
            g.last.data.swap(g.data);
            g.data.clear();
            g.last.screen = g.done_screen; g.last.tiles = g.scaled_tiles || scaled_matches || layout != 0; g.last.pair = g.done_pair;
            g.last.jitter_change = g.done_jitter_change;
            g.last.number = g.stats.samples;
            g.last.centered_layout = layout;
            g.last.flicker_stage = static_cast<uint8_t>(trace);
        }
        if (g.done_pair) ++g.stats.pairs;
        if (g.done_screen) {
            ++g.stats.screens;
            if (g.single_main) ++g.solo_screens;
        }
    } else {
        ++g.stats.failed;
    }
    emit_window(!enabled());
    retire();
    g.next_at = GetTickCount64() + every_ms();
}

void maintain()
{
    if (g.phase == Phase::Computing && !g.awaiting_result && completed(g.compute_fence.Get(), g.compute_value)) {
        if (na::background::Submit(collect_sample)) set_phase(Phase::Scoring);
    }
    if (g.phase == Phase::Retiring && completed(g.pre_fence.Get(), g.pre_value) &&
        completed(g.main_fence.Get(), g.main_value) && completed(g.compute_fence.Get(), g.compute_value)) {
        g.pre_value = g.main_value = g.compute_value = 0;
        g.pre_queue.Reset();
        g.main_queue.Reset();
        g.swapchain = nullptr;
        g.invalidated = false;
        g.awaiting_result = g.trace_pair = g.trial_adjacent = false;
        set_phase(Phase::Idle);
    }
}

void log_coverage_if_due()
{
    const auto now = GetTickCount64();
    if (now < g.coverage_at) return;
    const auto scene12 = g.observed_dx12, submitted12 = g.submitted_dx12;
    const auto presents11 = g.observed_dx11, submitted11 = g.submitted_dx11;
    const auto dx11 = g.continuity11, dx12 = g.continuity12;
    const auto solo = g.solo_screens;
    const auto pairs = g.stats.trace_pairs, reversals = g.stats.trace_reversals, rejected = g.stats.rejected_presents;
    const auto trial_pairs = g.stats.trial_pairs, comparable = g.stats.trial_comparable;
    if (na::background::Submit([scene12, submitted12, presents11, submitted11, dx11, dx12,
                                solo, pairs, reversals, rejected, trial_pairs, comparable] {
        na::eventlog::Emit("referee_coverage", "dx12_scene=%llu dx12_submitted=%llu dx11_presents=%llu dx11_submitted=%llu "
                           "dx12_submission_changes=%llu dx12_longest_miss=%llu dx11_submission_changes=%llu dx11_longest_miss=%llu solo_screens=%llu "
                           "trace_pairs=%llu trace_reversals=%llu rejected_presents=%llu trial_pairs=%llu trial_comparable=%llu",
                           static_cast<unsigned long long>(scene12), static_cast<unsigned long long>(submitted12),
                           static_cast<unsigned long long>(presents11), static_cast<unsigned long long>(submitted11),
                           static_cast<unsigned long long>(dx12.changes), static_cast<unsigned long long>(dx12.longest_miss),
                           static_cast<unsigned long long>(dx11.changes), static_cast<unsigned long long>(dx11.longest_miss),
                           static_cast<unsigned long long>(solo), static_cast<unsigned long long>(pairs),
                           static_cast<unsigned long long>(reversals), static_cast<unsigned long long>(rejected),
                           static_cast<unsigned long long>(trial_pairs), static_cast<unsigned long long>(comparable));
    })) {
        const uint32_t seconds = na::settings::Get().referee_log_s.load(std::memory_order_relaxed);
        g.coverage_at = now + 1000ull * (std::max)(seconds, 1u);
    } else {
        g.coverage_at = now + 1000;
    }
}

// The reduction, on our own list, on the presenting queue.
void compute(ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec)
{
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    if (!g.alloc || g.list_type != type) {
        g.alloc.Reset();
        g.list.Reset();
        if (FAILED(g.device->CreateCommandAllocator(type, IID_PPV_ARGS(&g.alloc))) ||
            FAILED(g.device->CreateCommandList(0, type, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)))) {
            stop("the referee's command list could not be made");
            return;
        }
        g.list->Close();
        g.list_type = type;
    }
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) { stop("the referee's command list could not reset"); return; }
    ID3D12GraphicsCommandList* l = g.list.Get();
    const bool screen = presented && ensure(kScreen, presented);
    if (screen) {
        D3D12_RESOURCE_BARRIER b[2] = {transition(presented, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       transition(g.img[kScreen].tex.Get(), NPSR, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = presented;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = g.img[kScreen].tex.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b[0] = transition(presented, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        b[1] = transition(g.img[kScreen].tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
        l->ResourceBarrier(2, b);
    }
    // Views: a missing image is stood in for by the input (never read: its pass is not run).
    const UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kImages; ++i) {
        const Copy& c = (g.img[i].tex && (i != kScreen || screen) && (i != kMotion || g.have_motion)) ? g.img[i] : g.img[kInput];
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = view_format(c.desc.Format);
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(c.tex.Get(), &v, h);
        h.ptr += inc;
    }
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g.root.Get());
    l->SetComputeRootDescriptorTable(1, g.heap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRootUnorderedAccessView(2, g.results->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER rb = transition(g.results.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->ResourceBarrier(1, &rb);
    struct K { uint32_t src, base, w, h, mw, mh, pad0, pad1; float sx, sy, djx, djy; TileLocations locations; };
    static_assert(sizeof(K) == (12 + kTiles) * sizeof(uint32_t));
    auto size = [](Img which, uint32_t& sw_, uint32_t& sh_) { sw_ = uint32_t(g.img[which].desc.Width); sh_ = g.img[which].desc.Height; };
    auto run = [&](ID3D12PipelineState* pso, const K& k, UINT gx, UINT gy) {
        l->SetPipelineState(pso);
        l->SetComputeRoot32BitConstants(0, 12 + kTiles, &k, 0);
        l->Dispatch(gx, gy, 1);
    };
    using namespace na::referee;
    K k{};
    const Img thumbs[3] = {kInput, kOutput, kScreen};
    const Block thumb_blocks[3] = {kThumbInput, kThumbOutput, kThumbScreen}, tile_blocks[3] = {kTileInput, kTileOutput, kTileScreen};
    uint32_t ow = 0, oh = 0, sw = 0, shh = 0;
    size(kOutput, ow, oh);
    if (screen) size(kScreen, sw, shh);
    uint32_t iw = 0, ih = 0;
    size(kInput, iw, ih);
    const bool aligned = screen && ow == sw && oh == shh && iw == sw && ih == shh;
    // Native DLAA may feed a game's own display resample (e.g. supersampling).
    // Compare like-aspect pictures on the smaller pixel grid. A small border can
    // instead preserve native pixels; the shared tile vote checks both origins.
    const double scene_aspect = double(iw) / ih;
    const bool resampled = screen && g.captured_context.placement.path == shadow::Path::Native11 &&
                           iw == ow && ih == oh && sw >= kTile && shh >= kTile &&
                           std::abs(double(sw) / shh - scene_aspect) < scene_aspect * .01;
    g.padded_layouts = screen && iw == ow && ih == oh ? PaddedLayouts(iw, ih, sw, shh) : 0;
    const bool tiles = aligned || resampled || g.padded_layouts;
    // Only compare consistent native grids. Centered padding requires the
    // independent full-resolution registration vote before scoring.
    g.trace_pair = (aligned || g.padded_layouts) && (g.adjacent || g.trial_adjacent) &&
        g.pre_trace_screen && g.pre_trace_output && g.pre_accepted &&
        g.img[kPrevOutput].desc.Width == ow && g.img[kPrevOutput].desc.Height == oh &&
        g.img[kPrevOutput].desc.Format == g.img[kOutput].desc.Format &&
        g.img[kPrevScreen].desc.Width == sw && g.img[kPrevScreen].desc.Height == shh &&
        g.img[kPrevScreen].desc.Format == g.img[kScreen].desc.Format;
    g.trace_geometry = g.trace_pair && g.adjacent && g.trial_adjacent &&
        g.img[kPrev].desc.Width == iw && g.img[kPrev].desc.Height == ih &&
        g.img[kPrev].desc.Format == g.img[kInput].desc.Format &&
        std::hypot(g.main_jitter[0] - g.pre_jitter[0], g.main_jitter[1] - g.pre_jitter[1]) >= .25f;
    g.sampled_locations = g.swapchain == g.tile_source && g.captured_context.SameTemporalSequence(g.tile_context)
        ? g.tile_locations : TileLocations{};
    for (int i = 0; screen && i < 3; ++i) {
        k = {};
        k.src = thumbs[i];
        size(thumbs[i], k.w, k.h);
        k.base = BlockOffset(thumb_blocks[i]);
        run(g.thumb.Get(), k, (kThumbW + 7) / 8, (kThumbH + 7) / 8);
        if (tiles) {
            k.locations = g.sampled_locations;
            k.base = BlockOffset(tile_blocks[i]);
            k.mw = (std::min)(iw, sw);
            k.mh = (std::min)(ih, shh);
            run(g.tiles.Get(), k, kTiles, 1);
            if (i == 2 && g.padded_layouts) {
                k.base = kResultFloats;
                k.pad0 = 1;
                run(g.tiles.Get(), k, kTiles, g.padded_layouts);
            }
        }
    }
    if (g.trace_pair) {
        const Img earlier[3]{kPrev, kPrevOutput, kPrevScreen};
        for (uint32_t i = 0; i < 3; ++i) {
            k = {};
            k.src = earlier[i];
            size(earlier[i], k.w, k.h);
            k.base = kTraceOffset + i * kThumbFloats;
            if (i == 2 && g.padded_layouts) { k.pad0 = 1; k.mw = iw; k.mh = ih; }
            run(g.thumb.Get(), k, (kThumbW + 7) / 8, (kThumbH + 7) / 8);
        }
        if (g.padded_layouts) {
            k.src = kScreen;
            k.base = kTraceOffset + 3 * kThumbFloats;
            run(g.thumb.Get(), k, (kThumbW + 7) / 8, (kThumbH + 7) / 8);
        }
        if (g.trace_geometry) {
            for (unsigned i = 0; i < 3; ++i) {
                k = {};
                k.src = earlier[i];
                size(earlier[i], k.w, k.h);
                k.base = kGeometryOffset + i * kTileFloats;
                k.mw = iw;
                k.mh = ih;
                k.locations = g.sampled_locations;
                k.pad0 = i == 2 && g.padded_layouts;
                run(g.tiles.Get(), k, kTiles, k.pad0 ? g.padded_layouts : 1);
            }
        }
    }
    uint32_t pw = 0, ph = 0;
    size(kPrev, pw, ph);
    const bool pair = g.have_motion && pw == iw && ph == ih;
    if (pair) {
        k = {};
        k.src = kInput;
        k.base = BlockOffset(kPoints);
        k.w = iw; k.h = ih;
        size(kMotion, k.mw, k.mh);
        k.sx = g.mv_scale[0]; k.sy = g.mv_scale[1];
        k.djx = g.main_jitter[0] - g.pre_jitter[0];
        k.djy = g.main_jitter[1] - g.pre_jitter[1];
        run(g.points.Get(), k, (kGridW + 7) / 8, (kGridH + 5) / 6);
    }
    D3D12_RESOURCE_BARRIER after[2]{};
    after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    after[0].UAV.pResource = g.results.Get();
    after[1] = transition(g.results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->ResourceBarrier(2, after);
    l->CopyBufferRegion(g.readback.Get(), 0, g.results.Get(), 0,
                        UINT64(sample_floats()) * sizeof(float));
    rb = transition(g.results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    l->ResourceBarrier(1, &rb);
    if (FAILED(l->Close())) { stop("the referee's command list could not close"); return; }
    ID3D12CommandList* one[] = {l};
    exec(queue, 1, one);
    g.compute_value = g.sample_serial;
    if (FAILED(queue->Signal(g.compute_fence.Get(), g.compute_value))) {
        stop("the referee reduction could not be fenced");
        return;
    }
    g.done_screen = screen;
    g.done_tiles = tiles;
    g.scaled_tiles = aligned || resampled;
    g.done_pair = pair;
    g.done_jitter_change = pair ? std::hypot(g.main_jitter[0] - g.pre_jitter[0], g.main_jitter[1] - g.pre_jitter[1]) : 0.f;
    if (!pair) ++g.stats.unpaired;
    set_phase(Phase::Computing);
}

void watchdog()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g.phase_at < kStuckMs) return;
    switch (g.phase) {
    case Phase::PreRecorded: case Phase::MainRecorded:
        stop("a recorded diagnostic copy has unknown submission status");
        break;
    case Phase::PreSubmitted: case Phase::MainSubmitted:
        ++g.stats.unpaired;
        retire();
        g.next_at = now + every_ms();
        break;
    case Phase::Computing:
        if (g.awaiting_result) {
            ++g.stats.unpaired;
            retire();
        }
        break;
    default: break;
    }
}

void reset_evidence_locked()
{
    ++g.epoch;
    g.tile_source = nullptr;
    g.recent.Clear();
    g.preview_pending = false;
    g.visual.clear();
    g.visual_rollback.store(false, std::memory_order_relaxed);
    g.trace_source = nullptr;
    clear_parity_locked();
    g.geometry = {};
}

} // namespace

bool Supported()
{
#ifdef NA_HAVE_REFEREE_SHADERS
    return true;
#else
    return false;
#endif
}

void NotifyEvaluationReset()
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(g.m);
    reset_evidence_locked();
}

bool Wanted()
{
    if (!enabled()) return false;
    std::lock_guard<std::mutex> lock(g.m);
    watchdog();
    maintain();
    if (g.phase == Phase::Idle) return GetTickCount64() >= g.next_at;
    return g.phase == Phase::PreSubmitted && g.swapchain;
}

uint32_t Record(ID3D12GraphicsCommandList* l, const Evaluation& e)
{
    if (!l || !e.color || !e.output || e.owns_present || !enabled()) return 0;
    const auto color_desc = e.color->GetDesc();
    const auto output_desc = e.output->GetDesc();
    const auto rw = e.render_w ? e.render_w : static_cast<uint32_t>(color_desc.Width);
    const auto rh = e.render_h ? e.render_h : color_desc.Height;
    const auto ow = e.output_w ? e.output_w : static_cast<uint32_t>(output_desc.Width);
    const auto oh = e.output_h ? e.output_h : output_desc.Height;
    if (rw > color_desc.Width || rh > color_desc.Height || ow > output_desc.Width || oh > output_desc.Height) return 0;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.phase == Phase::Stopped) return 0;
    if (e.reset) reset_evidence_locked();
    watchdog();
    if (g.next_token == kMaxToken) { stop("the referee's recording identities are exhausted"); return 0; }
    const bool due = g.phase == Phase::Idle && GetTickCount64() >= g.next_at;
    if (!due && g.phase != Phase::PreSubmitted) return 0;
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !init(dev.Get())) return 0;
    // VARIANT: referee/isolated-evaluation
    const bool screen_only = due && g.screen_only_source;
    // Vary parity only when opening a probe. Once it has a preceding Present,
    // skipping its partner can starve a trial whose two readers alternate.
    if (due && e.context.native_screen_proof) {
        const uint64_t screen_ordinal = screen_only ? e.present_ordinal : e.present_ordinal + 1;
        const auto v = visual_proof(e.context.native_screen_proof);
        if (v != g.visual.end() && ((screen_ordinal ^ v->ordinal) & 1u) == 0 &&
            !v->skipped_parity) {
            // Prefer the other parity, but an order that only runs every
            // other frame must still be sampled.
            v->skipped_parity = true;
            return 0;
        }
    }
    if (due && !screen_only) {
        const auto now = GetTickCount64();
        const bool trace_due = (g.keep_receipt || now >= g.trace_next_at) &&
            rw == ow && rh == oh && e.context.generation;
        const bool same_trial = e.context.trial_ticket && e.context.trial_ticket == g.trial_last_ticket;
        const bool ordinary_trace = !e.context.trial_ticket && g.trace_source && g.trace_context.SameTemporalSequence(e.context);
        const auto last = same_trial ? g.trial_last_ordinal : g.trace_ordinal;
        if (!g.keep_receipt && trace_due && (same_trial || ordinary_trace) &&
            ((e.present_ordinal + 1) & 1) == (last & 1) && !g.trace_parity_skipped) {
            g.trace_parity_skipped = true;
            return 0;
        }
        g.trace_parity_skipped = false;
        if (!ensure(kPrev, e.color, rw, rh)) { ++g.stats.failed; return 0; }
        copy(l, kPrev, e.color, e.color_state);
        g.pre_accepted = g.pre_trace_screen = false;
        // Limit the additional texture copies to native-resolution pairs.
        g.pre_trace_output = trace_due && ensure(kPrevOutput, e.output, ow, oh);
        if (trace_due) g.trace_next_at = now + (e.context.trial_ticket && !RouteTrial::WatchTicket(e.context.trial_ticket) ? 400 : 750);
        if (g.pre_trace_output) copy(l, kPrevOutput, e.output, e.output_state);
        g.captured_context = e.context;
        g.pre_ordinal = e.present_ordinal;
        g.pre_epoch = g.epoch;
        g.pre_jitter[0] = e.jitter_x;
        g.pre_jitter[1] = e.jitter_y;
        ++g.sample_serial;
        ++g.stats.pre;
        set_phase(Phase::PreRecorded);
        return g.token = ++g.next_token;
    }
    if (!screen_only && (!g.swapchain || e.present_ordinal <= g.pre_ordinal)) return 0;
    // Only temporal measurements depend on the previous picture.
    const bool adjacent = !screen_only && g.pre_epoch == g.epoch && e.present_ordinal - g.pre_ordinal == 1 && !e.reset &&
        g.captured_context.SameTemporalSequence(e.context) && color_desc.Format == g.img[kPrev].desc.Format &&
        rw == g.img[kPrev].desc.Width && rh == g.img[kPrev].desc.Height;
    g.adjacent = adjacent;
    // A history reset or a different physical scene texture can be part of
    // the artifact. Compare only the presentation for a ticketed trial;
    // motion and stage attribution still require the same render sequence.
    g.trial_adjacent = !screen_only && e.present_ordinal - g.pre_ordinal == 1 && e.context.trial_ticket &&
        e.context.trial_ticket == g.captured_context.trial_ticket &&
        e.context.trial_configuration == g.captured_context.trial_configuration &&
        e.context.placement.path == g.captured_context.placement.path &&
        rw == g.img[kPrev].desc.Width && rh == g.img[kPrev].desc.Height;
    const uint32_t mw = e.mv_display_res ? ow : rw, mh = e.mv_display_res ? oh : rh;
    g.have_motion = adjacent && !e.mv_jittered && e.motion && ensure(kMotion, e.motion, mw, mh);
    if (!ensure(kInput, e.color, rw, rh) || !ensure(kOutput, e.output, ow, oh)) { ++g.stats.failed; return 0; }
    if (screen_only) {
        ++g.sample_serial;
        g.swapchain = g.screen_only_source;
        g.screen_only_source = nullptr;
    }
    g.single_main = screen_only;
    g.captured_context = e.context;
    copy(l, kInput, e.color, e.color_state);
    copy(l, kOutput, e.output, e.output_state);
    if (g.have_motion) copy(l, kMotion, e.motion, e.motion_state);
    g.main_ordinal = e.present_ordinal;
    g.main_epoch = g.epoch;
    g.main_jitter[0] = e.jitter_x;
    g.main_jitter[1] = e.jitter_y;
    // The point shader predicts in input pixels; display-resolution vectors use output pixels.
    g.mv_scale[0] = e.mv_scale_x * (float(rw) / float(mw));
    g.mv_scale[1] = e.mv_scale_y * (float(rh) / float(mh));
    g.screen_elapsed = false;
    g.preview_exact = false;
    ++g.stats.main;
    set_phase(Phase::MainRecorded);
    return g.token = ++g.next_token;
}

void Submitted(uint32_t token, ID3D12CommandQueue* queue, uint8_t tools, uint8_t applied)
{
    if (!token || !queue) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (token != g.token) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) || dev.Get() != g.device.Get()) {
        stop("the referee copies were submitted on another device");
        return;
    }
    if (g.phase == Phase::PreRecorded) {
        g.trial_tools = tools;
        g.trial_applied = applied;
        g.pre_value = 2 * g.sample_serial;
        if (FAILED(queue->Signal(g.pre_fence.Get(), g.pre_value))) { stop("the previous input could not be fenced"); return; }
        g.pre_queue = queue;
        set_phase(Phase::PreSubmitted);
    } else if (g.phase == Phase::MainRecorded) {
        g.trial_tools &= tools;
        g.trial_applied &= applied;
        g.main_value = g.sample_serial;
        if (FAILED(queue->Signal(g.main_fence.Get(), g.main_value))) { stop("the current inputs could not be fenced"); return; }
        g.main_queue = queue;
        set_phase(Phase::MainSubmitted);
    }
    g.token = 0;
    if (g.invalidated) retire();
}

void Discard(uint32_t token, bool may_still_run)
{
    if (!token) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (token != g.token || (g.phase != Phase::PreRecorded && g.phase != Phase::MainRecorded)) return;
    ++g.stats.discarded;
    if (may_still_run) { stop("a discarded diagnostic recording may still execute"); return; }
    retire();
    g.next_at = GetTickCount64() + 50;
}

bool NeedsScreen(Presentation frame)
{
    if (!enabled() || !frame.swapchain) return false;
    std::lock_guard<std::mutex> lock(g.m);
    if (!frame.submitted || frame.native11 != (g.captured_context.placement.path == shadow::Path::Native11)) return false;
    if (g.phase == Phase::PreSubmitted)
        return frame.await_result && g.pre_trace_output && frame.ordinal == g.pre_ordinal && !g.swapchain;
    return g.phase == Phase::MainSubmitted && !g.screen_elapsed &&
        frame.ordinal == g.main_ordinal && frame.swapchain == g.swapchain;
}

// All callers hold the referee lock. Window accounting is attached to a
// returned Present, including the gaps caused by rejected submissions.
static void ObserveDelivery(Presentation frame)
{
    (frame.native11 ? g.continuity11 : g.continuity12).Observe(frame.swapchain, frame.ordinal, frame.eligible, frame.submitted);
    if (frame.eligible) {
        if (frame.native11) { ++g.observed_dx11; if (frame.submitted) ++g.submitted_dx11; }
        else { ++g.observed_dx12; if (frame.submitted) ++g.submitted_dx12; }
    }
}

static bool CopyPreviousScreen(ID3D12CommandQueue* queue, ID3D12Resource* image, ExecuteFn exec)
{
    if (!queue || queue != g.pre_queue.Get() || !exec || !image ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const auto desc = image->GetDesc();
    const auto in = g.img[kPrev].desc;
    if ((desc.Width != in.Width || desc.Height != in.Height) &&
        !PaddedLayouts(uint32_t(in.Width), in.Height, uint32_t(desc.Width), desc.Height)) return false;
    if (!ensure(kPrevScreen, image)) return false;
    if (!g.trace_alloc) {
        if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.trace_alloc)))) return false;
        if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.trace_alloc.Get(), nullptr, IID_PPV_ARGS(&g.trace_list)))) {
            g.trace_alloc.Reset();
            return false;
        }
        if (FAILED(g.trace_list->Close())) { g.trace_list.Reset(); g.trace_alloc.Reset(); return false; }
    }
    if (FAILED(g.trace_alloc->Reset()) || FAILED(g.trace_list->Reset(g.trace_alloc.Get(), nullptr))) return false;
    copy(g.trace_list.Get(), kPrevScreen, image, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(g.trace_list->Close())) return false;
    ID3D12CommandList* lists[]{g.trace_list.Get()};
    exec(queue, 1, lists);
    ++g.pre_value;
    if (FAILED(queue->Signal(g.pre_fence.Get(), g.pre_value))) {
        stop("the previous presentation copy could not be fenced");
        return false;
    }
    return true;
}

void OnPresent(Presentation frame, ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec)
{
    if (!enabled() || !frame.swapchain) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (frame.await_result) {
        const auto existing = std::find_if(g.pending_presents.begin(), g.pending_presents.end(),
            [&](const Presentation& p) { return p.swapchain == frame.swapchain && p.ordinal == frame.ordinal; });
        if (existing != g.pending_presents.end()) return;
        const auto slot = std::find_if(g.pending_presents.begin(), g.pending_presents.end(),
            [](const Presentation& p) { return !p.swapchain; });
        if (slot == g.pending_presents.end()) return; // An unacknowledgeable image must not supply proof.
        *slot = frame;
        g.pending_receipt.store(true, std::memory_order_release);
    } else {
        ObserveDelivery(frame);
        log_coverage_if_due();
    }
    watchdog();
    maintain();
    if (g.phase == Phase::PreSubmitted) {
        if (frame.native11 != (g.captured_context.placement.path == shadow::Path::Native11)) return;
        if (g.swapchain && frame.swapchain != g.swapchain) return;
        if (frame.ordinal == g.pre_ordinal && !g.swapchain && frame.submitted) {
            g.swapchain = frame.swapchain;
            g.pre_accepted = false;
            if (frame.await_result && g.pre_trace_output) g.pre_trace_screen = CopyPreviousScreen(queue, presented, exec);
        }
        else if (frame.ordinal > g.pre_ordinal) {
            g.screen_only_source = g.swapchain;
            ++g.stats.unpaired;
            retire();
        }
    }
    if (g.phase != Phase::MainSubmitted || frame.ordinal < g.main_ordinal) return;
    if (frame.swapchain != g.swapchain || frame.native11 != (g.captured_context.placement.path == shadow::Path::Native11)) return;
    const bool screen = !g.screen_elapsed && frame.ordinal == g.main_ordinal && frame.submitted;
    g.screen_elapsed = true;
    if (!queue || !exec) return;
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) || dev.Get() != g.device.Get() ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    if ((queue != g.pre_queue.Get() && !completed(g.pre_fence.Get(), g.pre_value)) ||
        (queue != g.main_queue.Get() && !completed(g.main_fence.Get(), g.main_value))) return;
    if ((!screen || !presented) && !g.have_motion) { retire(); return; }
    if (!frame.await_result) g.trial_adjacent = false;
    g.preview_exact = screen;
    g.awaiting_result = screen && frame.await_result;
    const auto& current = g.captured_context;
    frame.alternate.width = current.width;
    frame.alternate.height = current.height;
    g.observed_alternate = g.preview_exact && current.SameScene(frame.alternate) &&
        current.generation == frame.alternate.generation ? frame.alternate.placement : shadow::Placement{};
    compute(queue, screen ? presented : nullptr, exec);
}

void PresentResult(const void* swapchain, uint64_t ordinal, bool accepted)
{
    if (!swapchain || !g.pending_receipt.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g.m);
    const auto slot = std::find_if(g.pending_presents.begin(), g.pending_presents.end(),
        [&](const Presentation& p) { return p.swapchain == swapchain && p.ordinal == ordinal; });
    if (slot == g.pending_presents.end()) return;
    auto frame = *slot;
    *slot = {};
    g.pending_receipt.store(std::any_of(g.pending_presents.begin(), g.pending_presents.end(),
        [](const Presentation& p) { return p.swapchain != nullptr; }), std::memory_order_release);
    if (!accepted) {
        frame.eligible = frame.submitted = false;
        ++g.stats.rejected_presents;
    }
    ObserveDelivery(frame);
    log_coverage_if_due();
    if (swapchain != g.swapchain) return;
    if (g.phase == Phase::PreSubmitted && ordinal == g.pre_ordinal) {
        g.pre_accepted = accepted;
        if (!accepted) retire();
    } else if (g.phase == Phase::Computing && g.awaiting_result && ordinal == g.main_ordinal) {
        g.awaiting_result = false;
        if (!accepted) retire();
    }
}

uint64_t TakeNativeScreenLoss(const void* swapchain)
{
    if (!enabled() || !g.visual_rollback.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(g.m);
    const auto next = std::find_if(g.visual.begin(), g.visual.end(), [swapchain](const VisualEvidence& v) {
        return v.chain == swapchain && v.pending;
    });
    if (next == g.visual.end()) return 0;
    const uint64_t proof = next->proof;
    g.visual.erase(next);
    update_visual_pending();
    return proof;
}

bool TakeTrialSample(uint64_t ticket, TrialSample& sample)
{
    if (!ticket || ticket != g.trial_sample_ticket.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g.m);
    if (ticket != g.trial_sample_ticket.load(std::memory_order_relaxed)) return false;
    sample = g.trial_sample;
    g.trial_sample_ticket.store(0, std::memory_order_relaxed);
    return true;
}

void InvalidatePresentation(const void* swapchain)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.trial_source == swapchain) {
        g.trial_source = nullptr;
        g.trial_last_ticket = g.trial_last_ordinal = 0;
        g.trial_sample_ticket.store(0, std::memory_order_release);
    }
    for (auto& p : g.pending_presents) if (p.swapchain == swapchain) p = {};
    g.pending_receipt.store(std::any_of(g.pending_presents.begin(), g.pending_presents.end(),
        [](const Presentation& p) { return p.swapchain != nullptr; }), std::memory_order_release);
    if (g.trace_source == swapchain) {
        g.trace_source = nullptr;
        clear_parity_locked();
    }
    g.visual.erase(std::remove_if(g.visual.begin(), g.visual.end(),
        [swapchain](const VisualEvidence& v) { return v.chain == swapchain; }), g.visual.end());
    update_visual_pending();
    if (g.screen_only_source == swapchain) g.screen_only_source = nullptr;
    if (g.tile_source == swapchain) g.tile_source = nullptr;
    if (g.preview_source == swapchain) {
        g.recent.Clear();
        g.preview_pending = false;
        g.preview_source = nullptr;
    }
    if (g.swapchain && swapchain != g.swapchain) return;
    g.preview_exact = false;
    switch (g.phase) {
    case Phase::PreRecorded: case Phase::MainRecorded:
        g.invalidated = true;
        break;
    case Phase::PreSubmitted: case Phase::MainSubmitted:
        ++g.stats.unpaired;
        retire();
        break;
    default: break;
    }
}

bool ParityRoleSwapSeen(const void* swapchain, const PlacementScope& scope)
{
    if (!swapchain || !scope || !enabled() || !g_parity_seen.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(g.m);
    return g.trace_source == swapchain && scope.OwnsPicture(g.trace_context) &&
        g.parity.Recognized() && GetTickCount64() - g.parity_at <= 20000;
}
Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

LastSample GetLastSample()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.last;
}

void EnableSampleReceiptForTests()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.keep_receipt = true;
}

} // namespace na::referee::gpu
