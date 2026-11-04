#include "test_support.hpp"
#include "core/jitter_probe_gpu.hpp"
#include <vector>
#include <cmath>
#include <cstring>
namespace {
bool upload(test::Device& d,ID3D12Resource* dst,const void* pixels){
 const auto desc=dst->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
 d.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
 auto staging=test::CreateBuffer(d.device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 if(!staging)return false;void* data=nullptr;if(FAILED(staging->Map(0,nullptr,&data)))return false;
 for(UINT y=0;y<desc.Height;++y)std::memcpy(static_cast<char*>(data)+fp.Offset+SIZE_T(y)*fp.Footprint.RowPitch,
     static_cast<const char*>(pixels)+SIZE_T(y)*desc.Width*4,SIZE_T(desc.Width)*4);
 staging->Unmap(0,nullptr);test::List l;if(!l.Create(d.device.Get()))return false;
 D3D12_TEXTURE_COPY_LOCATION src{},to{};src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
 to.pResource=dst;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l.list->CopyTextureRegion(&to,0,0,0,&src,nullptr);
 auto t=test::Transition(dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);l.list->ResourceBarrier(1,&t);
 if(FAILED(l.list->Close()))return false;ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();return true;
}
}
int main(){
 test::Device d;if(!test::CreateDevice(d))return test::kSkip;
 na::jitter::gpu::Probe probe;REQUIRE(probe.Init(d.device.Get()));
 constexpr UINT W=512,H=256;const na::jitter::Source key{1,W,H,28};
 uint64_t frame=1;
 // All physical sources are stationary. The colour may carry the raster
 // offset, no offset, or its opposite; the encoder can independently shift it.
 // This exercises the moving sampling domain that produced SE5 phantom flow.
 for(float encode:{0.f,.75f})for(int mode=0;mode<3;++mode){
  const float raster=mode==0?0.f:mode==1?.75f:-.75f;
  std::vector<uint32_t> previous(W*H),current(W*H);
  auto picture=[](float x,float y){
   const float v=.5f+.2f*std::sin(x*.7f)+.2f*std::cos(y*.53f);
   const uint32_t c=uint32_t(std::lround(v*255.f));return 0xff000000u|c|(c<<8)|(c<<16);
  };
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
   previous[y*W+x]=picture(float(x),float(y));current[y*W+x]=picture(float(x)+encode-raster,float(y));
  }
  auto a=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
  auto b=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
  REQUIRE(a && b && upload(d,a.Get(),current.data()) && upload(d,b.Get(),previous.data()));
  test::List l;REQUIRE(l.Create(d.device.Get()));
  REQUIRE(probe.Record(l.list.Get(),0,a.Get(),b.Get(),key,frame++,{.75f,0.f},{encode,0.f}));
  REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
  na::jitter::Source observed;na::jitter::Evidence e;REQUIRE(probe.Collect(0,observed,e));CHECK(observed==key);
  const uint64_t count[]={e.unshifted,e.raster,e.opposite};
  std::printf("encode=%.2f mode=%d tested=%llu unjittered=%llu raster=%llu opposite=%llu\n",encode,mode,
      (unsigned long long)e.tested,(unsigned long long)e.unshifted,(unsigned long long)e.raster,(unsigned long long)e.opposite);
  CHECK(e.tested>4000 && count[mode]>e.tested*2/3);
  // Production evaluates multi-frame windows. The measured GPU votes must
  // qualify the right source under that actual decision, including ambiguity.
  na::jitter::ColourDomain domain;domain.Bind(key);
  e.frame=60;e.tested*=30;e.unshifted*=30;e.raster*=30;e.opposite*=30;
  CHECK(!domain.Observe(key,e));e.frame=120;CHECK(domain.Observe(key,e));
  CHECK(domain.Get()==(mode==0?na::jitter::Convention::Unjittered:mode==1?na::jitter::Convention::Raster:na::jitter::Convention::Opposite));
  CHECK(!probe.Collect(0,observed,e)); // one completed slot is consumed once
 }
 // A small fixed overlay over a moving, textured world must not teach the
 // whole source to be unjittered. Ambiguous world pixels stay in the total.
 {
  std::vector<uint32_t> prev(W*H),cur(W*H);
  auto pic=[](float x,float y){const auto c=uint32_t(std::lround((.5f+.2f*std::sin(x*.7f)+.2f*std::cos(y*.53f))*255));return 0xff000000u|c|(c<<8)|(c<<16);};
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){prev[y*W+x]=pic(float(x),float(y));cur[y*W+x]=pic(float(x)+(x<W/5?0.f:4.f),float(y));}
  auto a=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
  auto b=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
  REQUIRE(a && b && upload(d,a.Get(),cur.data()) && upload(d,b.Get(),prev.data()));
  test::List l;REQUIRE(l.Create(d.device.Get()));REQUIRE(probe.Record(l.list.Get(),0,a.Get(),b.Get(),key,frame++,{.75f,0.f},{}));
  REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
  na::jitter::Source observed;na::jitter::Evidence e;REQUIRE(probe.Collect(0,observed,e));
  std::printf("moving world plus fixed overlay: tested=%llu unjittered=%llu\n",(unsigned long long)e.tested,(unsigned long long)e.unshifted);
  na::jitter::ColourDomain domain;domain.Bind(key);e.tested*=30;e.unshifted*=30;e.raster*=30;e.opposite*=30;e.frame=60;
  CHECK(!domain.Observe(key,e));e.frame=120;CHECK(!domain.Observe(key,e));CHECK(domain.Get()==na::jitter::Convention::Unknown);
 }
 return test::finish("jitter_probe_test");
}
