#pragma once
#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace neural_probe {
using namespace DirectX::PackedVector;
struct Texture {
    ComPtr<ID3D12Resource> image,upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
};
ComPtr<ID3D12Resource> buffer(Session& s,UINT64 size,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=size;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    dx(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result)),"probe buffer");return result;
}
void transition(Session& s,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    s.commands->ResourceBarrier(1,&barrier);
}
Texture texture(Session& s,UINT width,UINT height,DXGI_FORMAT format,const void* pixels,UINT pixel_bytes,bool output=false) {
    Texture result;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=format;
    desc.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    dx(s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result.image)),"probe texture");
    UINT64 total=0;s.device12->GetCopyableFootprints(&desc,0,1,0,&result.footprint,nullptr,nullptr,&total);
    result.upload=buffer(s,total,D3D12_HEAP_TYPE_UPLOAD);
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};dx(result.upload->Map(0,&no_read,&mapped),"upload map");
    for(UINT y=0;y<height;++y)std::memcpy(static_cast<char*>(mapped)+size_t(y)*result.footprint.Footprint.RowPitch,
        static_cast<const char*>(pixels)+size_t(y)*width*pixel_bytes,size_t(width)*pixel_bytes);
    result.upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=result.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint=result.footprint;dst.pResource=result.image.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    transition(s,result.image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return result;
}
template<class T> void read(const std::filesystem::path& path,std::vector<T>& data) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()!=static_cast<std::streamoff>(data.size()*sizeof(T)))throw std::runtime_error("probe input length mismatch");
    file.seekg(0);file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()*sizeof(T)));
    if(!file)throw std::runtime_error("probe input read failed");
}
void save(const std::filesystem::path& path,const std::vector<uint16_t>& pixels) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*2));
    if(!file)throw std::runtime_error("probe output write failed");
}
void images(Session& s,const std::filesystem::path& prepared,const std::filesystem::path& output,UINT w,UINT h) {
    std::printf("REPLAY DIAGNOSTIC: %ux%u. Reinhard + sRGB approximation of captured linear color; NOT the game's final color stage.\n",w,h);
    std::ofstream(output/L"image-layout.txt")<<w<<" "<<h<<"\nRGBA16F little endian, RGB display-encoded approximation\n";
    for(unsigned frame=0;frame<2;++frame){
        std::vector<uint16_t> color(size_t(w)*h*4),motion(size_t(w)*h*2),result(size_t(w)*h*4,XMConvertFloatToHalf(-10.f));
        std::vector<float> depth(size_t(w)*h);
        read(prepared/("color"+std::to_string(frame)+".raw"),color);
        read(prepared/("motion"+std::to_string(frame)+".raw"),motion);
        read(prepared/("depth"+std::to_string(frame)+".raw"),depth);
        for(size_t i=0;i<color.size();++i){
            if(i%4==3){color[i]=XMConvertFloatToHalf(1.f);continue;}
            float value=XMConvertHalfToFloat(color[i]);if(!std::isfinite(value))throw std::runtime_error("invalid source color");
            value=std::max(0.f,value);value=value/(1.f+value);
            value=value<=.0031308f?12.92f*value:1.055f*std::pow(value,1.f/2.4f)-.055f;
            color[i]=XMConvertFloatToHalf(value);
        }
        save(output/("input"+std::to_string(frame)+".rgba16"),color);
        dx(s.allocator->Reset(),"allocator reset");dx(s.commands->Reset(s.allocator.Get(),nullptr),"command reset");
        auto c=texture(s,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,color.data(),8);
        auto z=texture(s,w,h,DXGI_FORMAT_R32_FLOAT,depth.data(),4);
        auto m=texture(s,w,h,DXGI_FORMAT_R16G16_FLOAT,motion.data(),4);
        auto out=texture(s,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,result.data(),8,true);
        auto* p=s.params;
        p->Set("DLSSNR.Color",c.image.Get());p->Set("DLSSNR.Depth",z.image.Get());
        p->Set("DLSSNR.MVec",m.image.Get());p->Set("DLSSNR.Output",out.image.Get());
        p->Set("DLSSNR.Reset",frame==0?1u:0u);p->Set("DLSSNR.DepthInverted",0u);
        p->Set("DLSSNR.MVecScaleX",-float(w));p->Set("DLSSNR.MVecScaleY",-float(h));
        for(const char* resource:{"Color","Depth","MVec","Output"}){
            const auto prefix=std::string("DLSSNR.")+resource+"Subrect";
            p->Set((prefix+"BaseX").c_str(),0u);p->Set((prefix+"BaseY").c_str(),0u);
            p->Set((prefix+"Width").c_str(),w);p->Set((prefix+"Height").c_str(),h);
        }
        D3D12_QUERY_HEAP_DESC query_desc{};query_desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query_desc.Count=2;
        ComPtr<ID3D12QueryHeap> query;dx(s.device12->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&query)),"timestamp heap");
        auto times=buffer(s,16,D3D12_HEAP_TYPE_READBACK);
        s.commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const auto start=std::chrono::steady_clock::now();
        const auto evaluation=NVSDK_NGX_D3D12_EvaluateFeature(s.commands.Get(),s.feature,p,nullptr);
        const auto api_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(!report(evaluation,"EvaluateFeature(18)"))throw std::runtime_error("neural evaluation failed");
        s.commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        s.commands->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        transition(s,out.image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const UINT64 bytes=UINT64(out.footprint.Footprint.RowPitch)*h;
        auto readback=buffer(s,bytes,D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=out.image.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out.footprint;
        s.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        s.finish();
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
        dx(readback->Map(0,&range,&mapped),"readback map");
        for(UINT y=0;y<h;++y)std::memcpy(result.data()+size_t(y)*w*4,static_cast<char*>(mapped)+size_t(y)*out.footprint.Footprint.RowPitch,size_t(w)*8);
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
        UINT64 frequency=0;dx(s.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
        range={0,16};dx(times->Map(0,&range,&mapped),"timestamp map");
        UINT64 stamps[2]{};std::memcpy(stamps,mapped,16);times->Unmap(0,&no_write);
        size_t invalid=0,sentinel=0,changed=0;float minimum=INFINITY,maximum=-INFINITY;double error=0;
        for(size_t i=0;i<result.size();++i)if(i%4!=3){
            const float value=XMConvertHalfToFloat(result[i]);
            invalid+=!std::isfinite(value);sentinel+=value==-10.f;changed+=result[i]!=color[i];
            minimum=std::min(minimum,value);maximum=std::max(maximum,value);error+=std::abs(value-XMConvertHalfToFloat(color[i]));
        }
        save(output/("output"+std::to_string(frame)+".rgba16"),result);
        std::printf("frame=%u RGB=[%g,%g] invalid=%zu sentinel=%zu changed=%zu meanAbsDifference=%g; CPU evaluate=%.3fms GPU bracket=%.3fms\n",
            frame,minimum,maximum,invalid,sentinel,changed,error/(double(w)*h*3),api_ms,double(stamps[1]-stamps[0])*1000/double(frequency));
        if(invalid || sentinel || !changed || maximum-minimum<.01f)throw std::runtime_error("invalid or trivial neural output");
    }
    std::puts("PASS: two recorded-input neural evaluations, finite nontrivial output; NOT live integration or final color/temporal validation.");
}
}
