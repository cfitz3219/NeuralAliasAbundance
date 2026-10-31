#pragma once
#include "host_dx12/nr/nr_pass.hpp"
#include <mutex>
#include <memory>
namespace na::nr { struct FgContract; }
namespace na::nr::native {
// Game-facing adapters supply exact states at this recording point. No NGX
// parameter object is manufactured; both adapters share one NR implementation.
struct FrameStates { uint32_t values[5]{}; bool preserve_additive_ui=false; };
bool RecordFrame(ID3D12GraphicsCommandList* list,const void* core,const void* stream,
                 const FgContract& frame,const Runtime& runtime,const FrameStates& states);
// A snapshot is readable only after its recording has actually been submitted.
// Same-queue order suffices; another queue must already have completed it.
// The owner retains payload resources through discard or every submission.
class RecordingLease {
public:
    bool Ready(ID3D12CommandQueue* consumer) const;
    bool Retired() const;
    explicit operator bool() const { return !!retired_; }
private:
    std::weak_ptr<void> record_;
    std::shared_ptr<const std::atomic<bool>> retired_,completed_;
    friend RecordingLease RetainRecording(ID3D12GraphicsCommandList*,std::shared_ptr<void>);
};
RecordingLease RetainRecording(ID3D12GraphicsCommandList* list,std::shared_ptr<void> payload);
// Scoped only around the original native FG evaluate. GPU ownership remains
// with its recorded Packet; the caller's parameter object is restored on return.
class InputOverride {
public:
    InputOverride()=default;
    InputOverride(const InputOverride&)=delete;
    InputOverride& operator=(const InputOverride&)=delete;
    ~InputOverride();
    void Bind(const void* parameters,ID3D12Resource* replacement);
private:
    void* parameters_=nullptr;
    ID3D12Resource* original_=nullptr;
};
// Called on the native FG recording thread before its original NGX evaluate.
void Before(ID3D12GraphicsCommandList* list,const void* core,const void* handle,const void* parameters,const Runtime& runtime,InputOverride* replacement=nullptr);
void After(const void* core,const void* handle,int32_t result);
void ReportUnavailable(const char* reason);
void Register(const void* core,const void* handle);
void Release(const void* core,const void* handle);
void CoreShutdown(const void* core);
void Prepare(ID3D12GraphicsCommandList* list);
void PrepareDevice(ID3D12Device* device);
void Maintain();
// Receipt becomes true only after Reset/destruction AND completion of every
// submission of this recording. Null means tracking failed, never completion.
std::shared_ptr<const std::atomic<bool>> TrackUse(ID3D12GraphicsCommandList* list);
// Called only at the game's ResizeBuffers boundary for an actual old buffer.
// Release completed game-buffer references, retaining executable NR payloads.
void RetireBackbuffer(ID3D12Resource* resource);
void TraceGpuProgress(); // background freeze logger; no waits/readbacks
void MarkSwapchainBuffer(ID3D12Resource* resource);
void Reset(ID3D12GraphicsCommandList* list);
bool OwnsRoute();
struct Status {
    uint64_t evaluates=0,reused=0,submitted=0,completed=0,skipped=0;
    uint32_t active=0,width=0,height=0;
    uint32_t source=0; // 1 native NGX FG, 2 game-facing Streamline real frame
    float last_ms=0,avg_ms=0;
    uint32_t gpu_samples=0;
    uint32_t input_states_known=0,color_state=0,hudless_state=0;
    uint32_t input_states[4]{},input_evidence[4]{},illegal_state_mask=0;
    uint32_t input_states_ready=0,state_contract_mask=0,hudless_replaced=0;
    uint32_t backbuffer_is_swapchain=0,output_real_is_swapchain=0,output_real_present=0;
    uint64_t frame_id=0;
    uint32_t identity_mode=0,multi_count=0,multi_index=0,list_type=0;
    char reason[96]{};
};
Status GetStatus();
// Lives around the original Execute forward; publishes completion only AFTER
// that forward. No queue waits, diagnostic readbacks or driver patching.
class Submission {
public:
    Submission(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists);
    ~Submission();
private:
    std::unique_lock<std::mutex> locks_[64];
    ID3D12CommandQueue* queue_=nullptr;
    std::shared_ptr<void> records_[64];
    uint32_t count_=0;
    bool entered_=false;
};
}
