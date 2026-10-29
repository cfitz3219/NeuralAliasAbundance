#pragma once
#include "host_dx12/nr/nr_pass.hpp"
#include <mutex>
namespace na::nr::native {
// Called on the native FG recording thread before its original NGX evaluate.
void Before(ID3D12GraphicsCommandList* list,const void* core,const void* handle,const void* parameters,const Runtime& runtime);
void Release(const void* core,const void* handle);
void CoreShutdown(const void* core);
void Prepare(ID3D12GraphicsCommandList* list);
void PrepareDevice(ID3D12Device* device);
void Maintain();
void Reset(ID3D12GraphicsCommandList* list);
bool OwnsRoute();
struct Status {
    uint64_t evaluates=0,reused=0,submitted=0,completed=0,skipped=0;
    uint32_t active=0,width=0,height=0;
    float last_ms=0,avg_ms=0;
    uint32_t gpu_samples=0;
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
    std::unique_lock<std::mutex> lock_;
    ID3D12CommandQueue* queue_=nullptr;
    void* records_[64]{};
    uint32_t count_=0;
    bool entered_=false;
};
}
