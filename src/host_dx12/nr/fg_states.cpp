#include "host_dx12/nr/fg_states.hpp"
#include <atomic>
#include <vector>
#include "host_dx12/hooking/variant_registry.hpp"
namespace na::nr::states {
namespace {
const GUID key={0x156d114c,0xf816,0x455e,{0xb2,0xf0,0x11,0x48,0xa9,0x19,0x16,0x72}};
void* barrier_original[8]{};
template<unsigned I> void STDMETHODCALLTYPE Enhanced(ID3D12GraphicsCommandList7* list,UINT count,const D3D12_BARRIER_GROUP* groups){
    // Enhanced layouts are a different contract. Never reuse a legacy state
    // across them; a subsequent observed legacy transition can qualify again.
    Reset(list);
    reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*,UINT,const D3D12_BARRIER_GROUP*)>(barrier_original[I])(list,count,groups);
}
struct Entry { ID3D12Resource* resource; D3D12_RESOURCE_STATES state; bool known; };
// Command-list recording is externally single-threaded by the D3D12 contract.
// Private data follows Reset/destruction; no resources or global locks retained.
struct State final:IUnknown {
    std::atomic<ULONG> refs{1};std::vector<Entry> entries;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override {
        if(!p)return E_POINTER;*p=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*p=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {auto n=--refs;if(!n)delete this;return n;}
};
State* Get(ID3D12GraphicsCommandList* list,bool create) {
    IUnknown* p=nullptr;UINT size=sizeof(p);
    if(SUCCEEDED(list->GetPrivateData(key,&size,&p)) && p)return static_cast<State*>(p);
    if(!create)return nullptr;auto* s=new State;
    if(FAILED(list->SetPrivateDataInterface(key,s))){s->Release();return nullptr;}return s;
}
}
bool InstallEnhancedInvalidation(){
    using namespace na::hooking::variants;
    static void* thunks[]={reinterpret_cast<void*>(&Enhanced<0>),reinterpret_cast<void*>(&Enhanced<1>),reinterpret_cast<void*>(&Enhanced<2>),reinterpret_cast<void*>(&Enhanced<3>),reinterpret_cast<void*>(&Enhanced<4>),reinterpret_cast<void*>(&Enhanced<5>),reinterpret_cast<void*>(&Enhanced<6>),reinterpret_cast<void*>(&Enhanced<7>)};
    static const SlotSpec spec{80,8,thunks,barrier_original};
    return Register(Kind::CommandList7,&spec,1);
}
void Reset(ID3D12GraphicsCommandList* list){if(list)list->SetPrivateDataInterface(key,nullptr);}
void Observe(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers) {
    if(!list || !count || !barriers)return;auto* s=Get(list,true);if(!s)return;
    for(UINT i=0;i<count;++i){const auto& b=barriers[i];
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){s->entries.clear();continue;}
        if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
           (b.Transition.Subresource!=0 && b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))continue;
        Entry* e=nullptr;for(auto& v:s->entries)if(v.resource==b.Transition.pResource){e=&v;break;}
        if(!e){if(s->entries.size()==512)s->entries.erase(s->entries.begin());s->entries.push_back({b.Transition.pResource,{},{}});e=&s->entries.back();}
        e->state=b.Transition.StateAfter;e->known=b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    }s->Release();
}
bool Read(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES& state) {
    if(!list)return false;
    using namespace na::hooking::variants;
    if(Active(Kind::CommandList7)) {ID3D12GraphicsCommandList7* extended=nullptr;
        if(SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&extended)))){
            const auto coverage=CoverageOf(Kind::CommandList7,extended);extended->Release();
            if(coverage!=Coverage::Complete)return false;
        }
    }
    auto* s=Get(list,false);if(!s)return false;bool found=false;
    for(const auto& v:s->entries)if(v.resource==r){found=v.known;state=v.state;break;}
    s->Release();return found;
}
}
