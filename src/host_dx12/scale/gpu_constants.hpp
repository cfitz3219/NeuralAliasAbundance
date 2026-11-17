#pragma once
#include "scene_group.hpp"
namespace na::scale {
// GPU-time CB copy + explicit extent edits. A record-time CPU clone would miss
// later game/J2 uploads (including submission-time camera jitter). The source
// buffer is never written. Prepare on the setup worker under HostWorkScope;
// retain this object in
// each executable recording and every submitted occurrence.
class GpuConstants {
public:
    static std::shared_ptr<const GpuConstants> Create(ID3D12Device* device, ID3D12Resource* source,
        uint64_t offset, const void* observed_bytes, size_t bytes,
        const std::vector<ConstantPatch>& patches, std::string& error,bool submission_copy=false);
    // Copy the latest GPU-visible source, apply only prepared size fields, then
    // expose the owned buffer in VERTEX_AND_CONSTANT_BUFFER state. Caller binds
    // Address() to the selected operation's root CBV/private table CBV.
    // Source state is explicit, and restored before this returns.
    bool RecordBefore(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES source_state) const;
    bool CanRecord(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES source_state) const;
    bool CanRecord(uint32_t observed_list_type,D3D12_RESOURCE_STATES source_state)const;
    // After the affected draw/dispatch and after rebinding the game's CB, return
    // the owned buffer to COMMON. Balanced calls support legal replay after
    // completion of the prior execution. Concurrent queues need private copies or ordering;
    // a shared_ptr/recording lease alone does not serialize GPU writes.
    void RecordAfter(ID3D12GraphicsCommandList* list) const;
    D3D12_GPU_VIRTUAL_ADDRESS Address() const { return owned_->GetGPUVirtualAddress(); }
    UINT Bytes() const { return static_cast<UINT>(bytes_); }
    // UPLOAD/GPU_UPLOAD cannot be GPU-written in the middle of a list. One
    // recorded copy still sees late CPU/J1 uploads on every executable replay.
    bool ReusableWithinRecording()const{return fixed_source_state_;}
    bool SubmissionCopy()const{return submission_copy_;}
    D3D12_RESOURCE_BARRIER Transition(D3D12_RESOURCE_STATES,D3D12_RESOURCE_STATES)const;
    void CopyPrepared(ID3D12GraphicsCommandList*)const; // caller places owned buffer in COPY_DEST
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> source_, owned_, patch_data_;
    uint64_t source_offset_ = 0, bytes_ = 0;
    bool fixed_source_state_ = false;
    bool submission_copy_ = false;
    std::vector<uint32_t> offsets_;
};
} // namespace na::scale
