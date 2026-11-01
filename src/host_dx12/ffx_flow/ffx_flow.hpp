// P4-0c: AMD FidelityFX optical flow (FSR3 3.0.3, MIT; third_party/ffx-opticalflow) run by our
// own small dispatcher, in the caller's command list. Plain compute, no fences, no AMD runtime:
// this is the flow engine for J2's inline route (Sniper), where NVIDIA's fence-driven unit has
// no gap to run in, and the fallback when that unit is unavailable.
//
// Per frame, Record() plays AMD's pass order into the list: prepare luma, luminance pyramid
// (SPD), scene-change histogram and divergence, then per level from coarse (6) to fine (0):
// search, filter, scale. Input: the current frame's colour (any float/UNORM format readable as
// float4), NON_PIXEL_SHADER_RESOURCE. Output: one R16G16_SINT vector per 8x8 block in FULL-SIZE
// PIXELS (integers), size ceil(w/8) x ceil(h/8), left in COMMON (like the NVOF grid). The sign
// convention is measured by ffx_flow_test and recorded in kOutputIsPrevMinusCur.
//
// Resources (the two input pyramids and the two flow pyramids alternate by frame parity) live in
// UNORDERED_ACCESS at rest; Record() transitions what each pass reads. Descriptors: a per-slot
// block in one shader-visible heap, one 64-entry table per dispatch; `slot` must not be reused
// while a list that used it may still run on the GPU (J2's ring index).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace na::ffx_flow {

constexpr uint32_t kBlock = 8;    // output block size in pixels
constexpr uint32_t kLevels = 7;   // pyramid levels (AMD: advancedAlgorithmIterations)

struct Stats {
    uint64_t records = 0;   // Record() calls that recorded work
    uint64_t resets = 0;    // of which cleared the history (first frame or reset)
    uint32_t dispatches = 0; // per Record
};

class Flow {
public:
    Flow() = default;
    ~Flow();
    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;

    // False with a reason when the shaders were not built (no DXC), the device is below Shader
    // Model 6.2, or a resource could not be created. `slots` = number of independent descriptor
    // blocks (J2's ring size). hdr selects AMD's HDR colour permutation (linear/scRGB input);
    // J2 feeds its fixed sRGB-style encode, so false.
    bool Init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t slots, bool hdr, char* reason, size_t cap);
    void Shutdown();
    bool Ready() const { return ready_; }

    // Records the whole flow for one frame. `color` must be NON_PIXEL_SHADER_RESOURCE for the
    // duration of the list; `color_format` is the SRV format (typeless colours need the typed one).
    // reset = true (or the first call) clears the history: the result then pairs this frame with
    // nothing and is not meaningful until the next call. Returns false with nothing recorded when
    // not ready or the slot is out of range.
    bool Record(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT color_format, bool reset, uint32_t slot);

    ID3D12Resource* Output() const { return out_.res.Get(); }  // R16G16_SINT, COMMON after Record
    ID3D12Resource* SceneChange() const { return scd_out_.res.Get(); } // R32_UINT 3x1, UNORDERED_ACCESS
    uint32_t OutWidth() const { return out_w_; }
    uint32_t OutHeight() const { return out_h_; }
    uint32_t Width() const { return w_; }
    uint32_t Height() const { return h_; }
    Stats GetStats() const { return stats_; }

private:
    struct Tex {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t w = 0, h = 0;
    };
    struct Pipeline { // one per na::ffx_flow::Pass
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    };
    struct Constants { // cbOF, 8 dwords (ffx_opticalflow_callbacks_hlsl.h)
        int32_t luma_w, luma_h;
        uint32_t level, level_count;
        uint32_t frame_index, transfer_function;
        float min_lum, max_lum;
    };
    struct Binding {
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    bool make_tex(uint32_t w, uint32_t h, DXGI_FORMAT f, Tex& t);
    void want(Tex& t, D3D12_RESOURCE_STATES s, bool uav_barrier_if_same = true);
    void flush(ID3D12GraphicsCommandList* list);
    void dispatch(ID3D12GraphicsCommandList* list, int pass, const Binding* uavs, uint32_t nu, const Binding* srvs, uint32_t ns,
                  const Constants& c, uint32_t x, uint32_t y, uint32_t z);
    void clear(ID3D12GraphicsCommandList* list, Tex& t);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t index) const;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Pipeline passes_[7];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_, clear_heap_; // shader-visible; CPU-only copy for clears
    UINT stride_ = 0;
    uint32_t slots_ = 0, slot_ = 0, cursor_ = 0, clears_ = 0; // next dispatch table / clear view within the slot
    ID3D12GraphicsCommandList* list_ = nullptr;                 // during Record (flushes from want)
    bool ready_ = false, hdr_ = false, first_ = true;
    uint32_t w_ = 0, h_ = 0, out_w_ = 0, out_h_ = 0;
    uint32_t frame_index_ = 0, parity_ = 0;
    Tex input_[2][kLevels];  // R8_UINT luma pyramids (level L: w>>L x h>>L)
    Tex flow_[2][kLevels];   // R16G16_SINT flow pyramids (level 0: out_w x out_h, then (n+1)/2)
    Tex scd_hist_, scd_prev_, scd_temp_, scd_out_;
    Tex out_;                // final level-0 flow for the caller
    D3D12_RESOURCE_BARRIER pending_[32];
    uint32_t npending_ = 0;
    Stats stats_;
};

}  // namespace na::ffx_flow
