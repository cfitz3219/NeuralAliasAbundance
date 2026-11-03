#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace na::overlay {
// Caller serializes changes. DXGI ResizeBuffers1 assigns one presentation
// queue per buffer, unlike creation's single queue for the whole chain.
class PresentQueues {
public:
    void Created(ID3D12CommandQueue* queue) { queues_={queue}; per_buffer_=false; }
    bool Resized(HRESULT result,UINT count,IUnknown* const* queues) {
        if(FAILED(result) || !queues || !count)return false;
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> next(count);
        for(UINT i=0;i<count;++i)if(!queues[i] || FAILED(queues[i]->QueryInterface(IID_PPV_ARGS(&next[i]))))return false;
        queues_=std::move(next);per_buffer_=true;return true;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> At(UINT index) const {
        if(queues_.empty() || (per_buffer_ && index>=queues_.size()))return {};
        return queues_[per_buffer_?index:0];
    }
private:
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> queues_;
    bool per_buffer_=false;
};
}
