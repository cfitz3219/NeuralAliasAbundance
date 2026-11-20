#pragma once
// Independent camera-free shader audit. GPU inputs are controlled here, so the
// implementation is compared with the CPU reference without AMD's integer-search
// errors hiding a shader error. All waits/readbacks are confined to this fixture.
#include "core/total_motion.hpp"
#include "host_dx12/j2/flow_shaders.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <vector>

inline int AuditTotalMotionShader(test::Device& d)
{
    constexpr unsigned W=128,H=96;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    test::List list;REQUIRE(list.Create(d.device.Get()));
    test::ComPtr<ID3DBlob> shader,errors,signature;
    HRESULT hr=D3DCompile(na::j2::kFlowShader,sizeof na::j2::kFlowShader-1,"audit-measure",nullptr,nullptr,"measure","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors);
    if(FAILED(hr)&&errors)std::printf("%s\n",static_cast<const char*>(errors->GetBufferPointer()));
    REQUIRE(SUCCEEDED(hr));
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,8};
    D3D12_ROOT_PARAMETER roots[2]{};
    roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={2,ranges};
    roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={0,0,28};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{2,roots,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(d.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    test::ComPtr<ID3D12PipelineState> pso;REQUIRE(SUCCEEDED(d.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,12,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    test::ComPtr<ID3D12DescriptorHeap> heap;REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    D3D12_DESCRIPTOR_HEAP_DESC cd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    test::ComPtr<ID3D12DescriptorHeap> clear_heap;REQUIRE(SUCCEEDED(d.device->CreateDescriptorHeap(&cd,IID_PPV_ARGS(&clear_heap))));
    const auto clear_cpu=clear_heap->GetCPUDescriptorHandleForHeapStart();
    const unsigned stride=d.device->GetDescriptorHandleIncrementSize(hd.Type);
    const auto handle=[&](unsigned i){auto a=heap->GetCPUDescriptorHandleForHeapStart();a.ptr+=size_t(i)*stride;return a;};
    const auto srv=[&](unsigned i,ID3D12Resource* r,DXGI_FORMAT format){D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format=format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
        d.device->CreateShaderResourceView(r,&v,handle(i));};
    const auto uav=[&](unsigned i,ID3D12Resource* r,DXGI_FORMAT format){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format=format;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.device->CreateUnorderedAccessView(r,nullptr,&v,handle(i));};
    auto output=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R32G32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto mask=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R32_FLOAT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto counters=test::CreateBuffer(d.device.Get(),40,D3D12_HEAP_TYPE_DEFAULT,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto read=test::CreateBuffer(d.device.Get(),W*H*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto read_mask=test::CreateBuffer(d.device.Get(),W*H*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto read_counts=test::CreateBuffer(d.device.Get(),40,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(output&&mask&&counters&&read&&read_mask&&read_counts);
    const auto execute=[&]{list.list->Close();ID3D12CommandList* ls[]={list.list.Get()};d.queue->ExecuteCommandLists(1,ls);d.WaitIdle();list.Reset();};
    struct Case{const char* name;float dx,jx,jy,raw;bool valid,flat,cut;};
    const Case cases[]={
        {"stationary rejects wrong block",0,0,0,7,true,false,false},
        {"one eighth pixel from zero block",.125f,0,0,0,true,false,false},
        {"quarter pixel on raster grid",.25f,-.375f,.125f,0,true,false,false},
        {"reversal",-.25f,.25f,-.25f,0,true,false,false},
        {"subpixel near integer block",3.4f,0,0,-3,true,false,false},
        {"flat unknown",0,0,0,0,false,true,false},
        {"flat approximate",0,0,0,-4,true,true,false},
        {"whole scene cut",3.4f,0,0,-3,true,false,true}};
    for(const auto& c:cases){
        std::vector<test::ComPtr<ID3D12Resource>> keep;
        const auto upload=[&](DXGI_FORMAT format,unsigned bpp,const void* data){
            auto texture=test::CreateTexture(d.device.Get(),W,H,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
            const unsigned row=(W*bpp+255u)&~255u;
            auto buffer=test::CreateBuffer(d.device.Get(),size_t(row)*H,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p=nullptr;buffer->Map(0,nullptr,&p);
            for(unsigned y=0;y<H;++y)std::memcpy(static_cast<char*>(p)+size_t(y)*row,static_cast<const char*>(data)+size_t(y)*W*bpp,W*bpp);
            buffer->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION a{},b{};
            a.pResource=buffer.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint.Footprint={format,W,H,1,row};
            b.pResource=texture.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
            auto bar=test::Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,S);list.list->ResourceBarrier(1,&bar);keep.push_back(buffer);return texture;
        };
        const auto signal=[](float x,float y){return .5f+.18f*std::sin(.17f*x)+.14f*std::sin(.11f*x+.13f*y)+.12f*std::cos(.07f*y)+.05f*std::sin(.09f*x-.21f*y);};
        std::vector<float> cur(W*H),prev(W*H),rgba(W*H*4),raw(W*H*2),valid(W*H,c.valid?1.f:0.f);
        std::vector<unsigned> cut(W*H);cut[1]=c.cut?1u:0u;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const size_t i=size_t(y)*W+x;
            cur[i]=c.flat?.5f:signal(float(x)+.5f-c.dx,float(y)+.5f);
            prev[i]=c.flat?.5f:signal(float(x)+.5f,float(y)+.5f);
            raw[i*2]=c.raw;
        }
        for(size_t i=0;i<cur.size();++i){rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=cur[i];rgba[i*4+3]=1;}
        auto current=upload(DXGI_FORMAT_R32G32B32A32_FLOAT,16,rgba.data());
        for(size_t i=0;i<prev.size();++i)rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=prev[i];
        auto previous=upload(DXGI_FORMAT_R32G32B32A32_FLOAT,16,rgba.data());
        auto flow=upload(DXGI_FORMAT_R32G32_FLOAT,8,raw.data());auto validity=upload(DXGI_FORMAT_R32_FLOAT,4,valid.data());
        auto change=upload(DXGI_FORMAT_R32_UINT,4,cut.data());
        for(unsigned i=0;i<8;++i)srv(i,nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT);
        srv(1,flow.Get(),DXGI_FORMAT_R32G32_FLOAT);srv(3,validity.Get(),DXGI_FORMAT_R32_FLOAT);srv(4,change.Get(),DXGI_FORMAT_R32_UINT);
        srv(5,current.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);srv(6,previous.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
        uav(8,output.Get(),DXGI_FORMAT_R32G32_FLOAT);uav(9,mask.Get(),DXGI_FORMAT_R32_FLOAT);uav(11,nullptr,DXGI_FORMAT_R32G32B32A32_FLOAT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC cv{};cv.Format=DXGI_FORMAT_R32_TYPELESS;cv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;cv.Buffer.NumElements=10;cv.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        d.device->CreateUnorderedAccessView(counters.Get(),nullptr,&cv,handle(10));
        d.device->CreateUnorderedAccessView(counters.Get(),nullptr,&cv,clear_cpu);
        ID3D12DescriptorHeap* heaps[]={heap.Get()};list.list->SetDescriptorHeaps(1,heaps);
        const unsigned zeros[4]{};auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=size_t(10)*stride;
        list.list->ClearUnorderedAccessViewUint(gpu,clear_cpu,counters.Get(),zeros,0,nullptr);
        D3D12_RESOURCE_BARRIER order{};order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list.list->ResourceBarrier(1,&order);
        std::array<unsigned,28> constants{};constants[0]=constants[2]=W;constants[1]=constants[3]=H;constants[6]=2;
        std::memcpy(&constants[22],&c.jx,4);std::memcpy(&constants[23],&c.jy,4);
        list.list->SetComputeRootSignature(root.Get());list.list->SetPipelineState(pso.Get());
        list.list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list.list->SetComputeRoot32BitConstants(1,28,constants.data(),0);
        list.list->Dispatch((W+7)/8,(H+7)/8,1);
        const auto read_texture=[&](ID3D12Resource* r,ID3D12Resource* target,DXGI_FORMAT format,unsigned row){
            auto bar=test::Transition(r,U,D3D12_RESOURCE_STATE_COPY_SOURCE);list.list->ResourceBarrier(1,&bar);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=r;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            b.pResource=target;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={format,W,H,1,row};
            list.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);std::swap(bar.Transition.StateBefore,bar.Transition.StateAfter);list.list->ResourceBarrier(1,&bar);
        };
        read_texture(output.Get(),read.Get(),DXGI_FORMAT_R32G32_FLOAT,W*8);read_texture(mask.Get(),read_mask.Get(),DXGI_FORMAT_R32_FLOAT,W*4);
        auto bar=test::Transition(counters.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);list.list->ResourceBarrier(1,&bar);
        list.list->CopyBufferRegion(read_counts.Get(),0,counters.Get(),0,40);std::swap(bar.Transition.StateBefore,bar.Transition.StateAfter);list.list->ResourceBarrier(1,&bar);
        execute();
        float* data=nullptr;float* masks=nullptr;unsigned* counts=nullptr;
        REQUIRE(SUCCEEDED(read->Map(0,nullptr,reinterpret_cast<void**>(&data))));
        REQUIRE(SUCCEEDED(read_mask->Map(0,nullptr,reinterpret_cast<void**>(&masks))));
        REQUIRE(SUCCEEDED(read_counts->Map(0,nullptr,reinterpret_cast<void**>(&counts))));
        const na::motion::total::Image ci{cur.data(),W,H},pi{prev.data(),W,H};
        unsigned compared=0,decisions=0,vector_errors=0;double maximum=0;
        for(unsigned y=8;y<H-8;++y)for(unsigned x=8;x<W-8;++x){const size_t i=size_t(y)*W+x;
            const auto expected=c.cut?na::motion::total::Result{}:na::motion::total::MeasurePixel(ci,pi,float(x)+.5f-c.jx,float(y)+.5f-c.jy,c.valid,c.raw,0);
            if(masks[i]!=expected.valid)++decisions;
            const double delta=std::hypot(data[i*2]-expected.x,data[i*2+1]-expected.y);
            maximum=(std::max)(maximum,delta);if(delta>.03)++vector_errors;++compared;
        }
        unsigned count=0;for(unsigned i=0;i<8;++i)count+=counts[i];CHECK(count==W*H);CHECK(counts[8]==(c.cut?1u:0u));
        std::printf("measure GPU/CPU %s: compared=%u validity_mismatches=%u vector_errors_over_.03=%u max_delta=%.6f\n",c.name,compared,decisions,vector_errors,maximum);
        // Hardware bilinear fractional-coordinate precision is finite; a tiny number of
        // threshold-adjacent decisions may differ. Report every difference rather than conceal it.
        CHECK(decisions<=compared/200);CHECK(vector_errors<=compared/200);
        read->Unmap(0,nullptr);read_mask->Unmap(0,nullptr);read_counts->Unmap(0,nullptr);
    }
    return 0;
}
