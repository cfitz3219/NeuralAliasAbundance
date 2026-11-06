// Game motion feed: scores the game's two-channel candidate textures against the picture and,
// once one is proven (game_motion_decide.hpp), converts it into the arbiter's tier 1 (engine
// motion, pixels, v = previous - current) with a mask of the pixels it wrote. Static pixels
// (zero in the texture) fall through to the camera tier.
//
// Recording: Record() runs inside J2's evaluation list after its colour/depth copies, with the
// colour copy in NON_PIXEL_SHADER_RESOURCE. It reads back the counters of this slot's previous
// recording (J2 reuses a slot only after its fence), never waits, and leaves every resource in
// the state it found it.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>
#include "host_dx12/j2/game_motion_decide.hpp"

namespace na::j2::game_motion {

struct Candidate {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON; // its state in the list now
    uint64_t id = 0;
};

struct Output { ID3D12Resource* motion = nullptr; ID3D12Resource* valid = nullptr; }; // NPSR when set

struct Stats {
    uint64_t recorded = 0, scored = 0, fed = 0, collected = 0, candidate_changes = 0;
    uint64_t measured = 0, good = 0, locked = 0, dropped = 0;
    uint32_t candidates = 0;
    int used_candidate = -1, used_encoding = -1;
    int last_best = -1; uint32_t last_candidate = 0;
    double last_ratio = 0, last_second = 0;
    uint64_t last_scored = 0, last_moving = 0, last_non_finite = 0;
    float last_max_x = 0, last_max_y = 0;
};

class Feed {
public:
    bool Init(ID3D12Device* device, uint32_t slots);
    // cands: up to kCandidates. reset: this frame does not continue the previous one (cut, resize,
    // gap): it is not scored. rw/rh: active extent (top left). Returns the tier 1 inputs when a
    // candidate is in use (converted in this list).
    Output Record(ID3D12GraphicsCommandList* list, uint32_t slot, const Candidate* cands, uint32_t count,
                  ID3D12Resource* color, uint32_t rw, uint32_t rh, bool reset);
    Stats GetStats() const;
    void Describe(char* text, size_t capacity) const;
    bool Ready() const { return pso_score_ != nullptr; }

private:
    struct SlotState {
        Microsoft::WRL::ComPtr<ID3D12Resource> counters, readback;
        uint64_t candidate_id[kCandidates]{};
        bool recorded = false;
    };
    bool ensure_textures(ID3D12Resource* color, uint32_t rw, uint32_t rh);
    void collect(uint32_t slot);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_score_, pso_convert_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> zeros_, prev_, xy_, valid_;
    UINT stride_ = 0;
    uint32_t slots_ = 0;
    SlotState slot_[8];
    uint64_t current_id_[kCandidates]{};
    bool prev_valid_ = false;
    DXGI_FORMAT prev_format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t tw_ = 0, th_ = 0;
    Decider decider_;
    Stats stats_;
};

} // namespace na::j2::game_motion
