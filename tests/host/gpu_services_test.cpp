// Offline NVRHI/native interoperability. All GPU waits/readback are test-only.
#include "test_support.hpp"
#include "host_dx12/gpu/services.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <atomic>
namespace {unsigned forwards=0;void STDMETHODCALLTYPE forward(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* l){++forwards;q->ExecuteCommandLists(n,l);}}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
#if !defined(NA_HAVE_NVRHI)
    return test::kSkip;
#else
    test::ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;test::ComPtr<ID3D12InfoQueue> info;d.device.As(&info);
    CHECK(!na::gpu::Enabled());na::gpu::Recording disabled;CHECK(!disabled.Open(d.device.Get(),d.queue.Get()));
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),false));
    const auto before=na::observe::GetProducerStats();
    na::settings::Get().experimental_nvrhi=1;
    constexpr unsigned W=16,H=16;constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=W;td.Height=H;td.DepthOrArraySize=td.MipLevels=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    na::gpu::Texture source,destination;REQUIRE(na::gpu::CreateTexture(d.device.Get(),d.queue.Get(),td,Read,source));
    REQUIRE(na::gpu::CreateTexture(d.device.Get(),d.queue.Get(),td,Read,destination));
    CHECK(source.owner_&&destination.owner_);CHECK(na::gpu::GetStats().textures==2);
    CHECK(na::observe::GetProducerStats().objects_created==before.objects_created);
    const char* shader="Texture2D<float4> src:register(t0); RWTexture2D<float4> dst:register(u0); cbuffer B:register(b0){float factor;uint3 padding;} [numthreads(8,8,1)] void main(uint2 p:SV_DispatchThreadID){dst[p]=src[p]*factor;}";
    test::ComPtr<ID3DBlob> code,error;REQUIRE(SUCCEEDED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)));
    na::gpu::ComputeBindings bind;bind.source[0]=source.Get();bind.source_format[0]=td.Format;bind.destination=destination.Get();bind.destination_format=td.Format;
    auto packet=na::gpu::PrepareCompute(d.device.Get(),d.queue.Get(),code->GetBufferPointer(),code->GetBufferSize(),16,bind);REQUIRE(packet);
    // Game-owned lists surround the NVRHI list. Shared queue ordering, no GPU wait in the service.
    test::List prefix,suffix;REQUIRE(prefix.Create(d.device.Get())&&suffix.Create(d.device.Get()));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;d.device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    float* map=nullptr;REQUIRE(SUCCEEDED(upload->Map(0,nullptr,reinterpret_cast<void**>(&map))));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W*4;++x)reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(map)+y*fp.Footprint.RowPitch)[x]=.25f;
    upload->Unmap(0,nullptr);auto b=test::Transition(source.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);prefix.list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;prefix.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);prefix.list->ResourceBarrier(1,&b);REQUIRE(SUCCEEDED(prefix.list->Close()));
    na::gpu::Recording own;REQUIRE(own.Open(d.device.Get(),d.queue.Get()));
    auto out=destination.Get();b=test::Transition(out,Read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);own.Native()->ResourceBarrier(1,&b);
    struct Constants {float factor;uint32_t pad[3];} constants{2,{}};
    REQUIRE(na::gpu::Dispatch(own.Native(),packet,&constants,sizeof constants,2,2));
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);own.Native()->ResourceBarrier(1,&b);REQUIRE(own.Close());
    b=test::Transition(out,Read,D3D12_RESOURCE_STATE_COPY_SOURCE);suffix.list->ResourceBarrier(1,&b);
    src={};dst={};src.pResource=out;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    suffix.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);suffix.list->ResourceBarrier(1,&b);REQUIRE(SUCCEEDED(suffix.list->Close()));
    // Recording leases retain external allocations; NVRHI retains imported bindings too.
    std::weak_ptr<void> lifetime=destination.owner_;own.Retain(destination.owner_);packet.reset();destination.Reset();
    ID3D12CommandList* lists[]={prefix.list.Get(),own.Native(),suffix.list.Get()};
    REQUIRE(na::gpu::SubmitMixed(d.queue.Get(),3,lists,&own,forward));CHECK(forwards==2);d.WaitIdle();na::gpu::Collect();
    REQUIRE(SUCCEEDED(readback->Map(0,nullptr,reinterpret_cast<void**>(&map))));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W*4;++x)CHECK(reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(map)+y*fp.Footprint.RowPitch)[x]==.5f);
    readback->Unmap(0,nullptr);CHECK(lifetime.expired());CHECK(na::gpu::GetStats().retained==0);
    // Owned lists are single-use; an attempted replay is rejected, never blindly resubmitted to NVRHI.
    CHECK(!own.Submit());own.Discard();
    REQUIRE(own.Open(d.device.Get(),d.queue.Get()));REQUIRE(own.Close());own.Discard(); // discard without submit, then safely reopen
    REQUIRE(own.Open(d.device.Get(),d.queue.Get()));REQUIRE(own.Close());REQUIRE(own.Submit());d.WaitIdle();na::gpu::Collect();
    CHECK(na::gpu::GetStats().errors==0);CHECK(na::gpu::GetStats().dispatches==1);
    if(info){unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,m,&size))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::printf("D3D12: %s\n",m->pDescription);}}CHECK(errors==0);}
    na::observe::SetProcessExiting();return test::finish("shared NVRHI GPU services");
#endif
}
