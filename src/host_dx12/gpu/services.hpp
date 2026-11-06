#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
namespace na::gpu {
using Forward=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
// Owning NVRHI handle retained alongside the native resource for externally
// recorded work. Existing recording/fence owners retain this value unchanged.
class Texture : public Microsoft::WRL::ComPtr<ID3D12Resource> {
public:
    using Microsoft::WRL::ComPtr<ID3D12Resource>::ComPtr;
    void Reset(){owner_.reset();Microsoft::WRL::ComPtr<ID3D12Resource>::Reset();}
    std::shared_ptr<void> owner_;
};
bool CreateTexture(ID3D12Device*,ID3D12CommandQueue*,const D3D12_RESOURCE_DESC&,D3D12_RESOURCE_STATES,Texture&);
// A list created/opened/submitted by NVRHI. Native passes can record into its
// exported list; we never import a game-owned command list into NVRHI.
class Recording {
public:
    Recording();~Recording();
    bool Open(ID3D12Device*,ID3D12CommandQueue*);
    ID3D12GraphicsCommandList* Native()const;
    bool Close();
    bool Submit();
    bool Ready(ID3D12CommandQueue*)const;
    void Retain(std::shared_ptr<void>);
    void Discard();
private:
    struct State;std::unique_ptr<State> state_;
};
// Preserve list order, forward every native group once, and submit owned work
// through its NVRHI queue/fence. No queue wait or callback diagnostic readback.
bool SubmitMixed(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,Recording*,Forward);
// A cached binding packet for a compute program, imported resources and small
// immutable per-dispatch constants. Usable only on our currently open NVRHI list.
struct ComputeBindings {
    ID3D12Resource* source[2]{};DXGI_FORMAT source_format[2]{};
    ID3D12Resource* destination=nullptr;DXGI_FORMAT destination_format=DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* uint_buffers[2]{};
};
class ComputePacket;
std::shared_ptr<ComputePacket> PrepareCompute(ID3D12Device*,ID3D12CommandQueue*,const void* code,size_t code_size,uint32_t constants_size,const ComputeBindings&);
bool Dispatch(ID3D12GraphicsCommandList*,const std::shared_ptr<ComputePacket>&,const void* constants,uint32_t constants_size,uint32_t x,uint32_t y);
struct Stats {uint64_t textures=0,opens=0,submits=0,collections=0,errors=0,dispatches=0,bindings=0,retained=0;bool built=false;};
Stats GetStats();
void Collect();
bool Enabled();
// No product preference: automatic selection follows measured backend cost.
// Offline regressions can still exercise both implementations explicitly.
enum class TestBackend {Automatic, Native, Nvrhi};
void SetBackendForTests(TestBackend);
}
