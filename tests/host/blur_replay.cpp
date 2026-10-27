// Temporary offline diagnosis only. Replays captured game inputs, never loaded into a game.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
int main(int argc,char**argv){
 REQUIRE(argc==4);std::string dir=argv[1], mode=argv[2], dest=argv[3];
 test::Device d;REQUIRE(test::CreateDevice(d,false));auto dev=d.device.Get();
 REQUIRE(na::dlss::InitOwn(dev,L"."));
 constexpr unsigned W=3440,H=1440;
 DXGI_FORMAT fmts[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT};
 const char* names[]={"color","depth",mode=="composed-motion" ? "motion-corrected" : "motion"};
 test::ComPtr<ID3D12Resource> tex[3],up[3];D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3]{};UINT64 sizes[3]{};
 std::vector<char> data[8][3];float jit[8][2]{};
 for(int k=0;k<3;k++){
  tex[k]=test::CreateTexture(dev,W,H,fmts[k],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);REQUIRE(tex[k]);auto rd=tex[k]->GetDesc();dev->GetCopyableFootprints(&rd,0,1,0,&fp[k],nullptr,nullptr,&sizes[k]);up[k]=test::CreateBuffer(dev,sizes[k],D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(up[k]);
  for(int i=0;i<8;i++){std::ifstream f(dir+"/frame"+std::to_string(i)+"-"+names[k]+".raw",std::ios::binary);REQUIRE(f.good());data[i][k].resize(size_t(sizes[k]));f.read(data[i][k].data(),std::streamsize(sizes[k]));REQUIRE(f.gcount()==std::streamsize(sizes[k]));}
 }
 for(int i=0;i<8;i++){std::ifstream f(dir+"/frame"+std::to_string(i)+"-meta.json");std::string s((std::istreambuf_iterator<char>(f)),{});auto p=s.find("\"jitter\": [");REQUIRE(p!=std::string::npos);REQUIRE(sscanf_s(s.c_str()+p+11,"%f, %f",&jit[i][0],&jit[i][1])==2);}
 auto out=test::CreateTexture(dev,W,H,fmts[0],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(out);D3D12_PLACED_SUBRESOURCE_FOOTPRINT fo{};UINT64 so=0;auto rd=out->GetDesc();dev->GetCopyableFootprints(&rd,0,1,0,&fo,nullptr,nullptr,&so);auto rb=test::CreateBuffer(dev,so,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);REQUIRE(rb);
 test::List l;REQUIRE(l.Create(dev));
 for(int frame=0;frame<64;frame++){
  int i=frame%8;if(frame)l.Reset();
  for(int k=0;k<3;k++){
   void*p=nullptr;REQUIRE(SUCCEEDED(up[k]->Map(0,nullptr,&p)));if(k==2&&mode=="zero-motion")std::memset(p,0,size_t(sizes[k]));else std::memcpy(p,data[i][k].data(),size_t(sizes[k]));up[k]->Unmap(0,nullptr);
   if(frame){auto b=test::Transition(tex[k].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);l.list->ResourceBarrier(1,&b);}
   D3D12_TEXTURE_COPY_LOCATION s{},t{};s.pResource=up[k].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=fp[k];t.pResource=tex[k].Get();t.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l.list->CopyTextureRegion(&t,0,0,0,&s,nullptr);
   auto b=test::Transition(tex[k].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);l.list->ResourceBarrier(1,&b);
  }
  na::dlss::EvalDesc e;e.color=tex[0].Get();e.depth=tex[1].Get();e.motion=tex[2].Get();e.output=out.Get();e.render_w=e.out_w=W;e.render_h=e.out_h=H;e.quality=na::dlss::Quality::DLAA;e.hdr=true;e.auto_exposure=true;e.reset=frame==0;e.jitter_x=jit[i][0];e.jitter_y=jit[i][1];if(mode=="reverse-jitter"){e.jitter_x=-e.jitter_x;e.jitter_y=-e.jitter_y;}if(mode=="zero-jitter")e.jitter_x=e.jitter_y=0;
  REQUIRE(na::dlss::Evaluate(l.list.Get(),e));
  if(frame==63){auto b=test::Transition(out.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);l.list->ResourceBarrier(1,&b);D3D12_TEXTURE_COPY_LOCATION s{},t{};s.pResource=out.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;t.pResource=rb.Get();t.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;t.PlacedFootprint=fo;l.list->CopyTextureRegion(&t,0,0,0,&s,nullptr);}
  REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* ls[]={l.list.Get()};d.queue->ExecuteCommandLists(1,ls);na::dlss::OnExecuted(d.queue.Get(),1,ls);d.WaitIdle();
 }
 void*p=nullptr;REQUIRE(SUCCEEDED(rb->Map(0,nullptr,&p)));std::ofstream f(dest,std::ios::binary);f.write(static_cast<char*>(p),std::streamsize(so));f.close();rb->Unmap(0,nullptr);na::dlss::Shutdown();printf("replay %s: 64 evaluations, output %s\n",mode.c_str(),dest.c_str());return 0;
}

