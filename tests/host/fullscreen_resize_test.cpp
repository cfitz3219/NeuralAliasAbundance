// Real DX12 swapchain/proxy allocations behind hidden windowed and maximized
// borderless windows. No physical display-mode switch and no game launch.
#include "test_support.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/native_present/size_spoof.hpp"
#include "host_dx12/native_present/size_controller.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <cmath>
namespace na::overlay { long ReadNativeBufferForTests(IDXGISwapChain*,unsigned,REFIID,void**); }
namespace np=na::native_present;
namespace ctl=na::native_present::ctl;
namespace {
// The upscale callback is never executed: this fixture qualifies actual resize
// and allocation ownership, not DLSS evaluation or final image quality.
bool unused_upscale(ID3D12CommandQueue*,ID3D12Resource*,uint32_t,uint32_t,ID3D12Resource*,uint32_t,uint32_t,bool,void*) { return false; }
struct Game {
    test::ComPtr<IDXGISwapChain3> chain;
    test::ComPtr<ID3D12Resource> buffer;
    uint64_t token=0;
    UINT w=0,h=0,calls=0;
    HRESULT hr=E_FAIL;
};
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_NCCREATE) SetWindowLongPtrW(hwnd,GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* game=reinterpret_cast<Game*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    WINDOWPOS scratch{};
    if(np::PrepareSizeMessage(hwnd,msg,wp,lp,scratch)==np::MessageAction::Consume)return 0;
    if(game && game->token && msg==WM_SIZE) {
        if(wp==SIZE_MAXIMIZED || wp==SIZE_MINIMIZED)return 0;
        RECT rect{};
        { const np::GameScopeForTest scope; if(!GetClientRect(hwnd,&rect))return 0; }
        game->buffer.Reset();
        game->hr=game->chain->ResizeBuffers(0,UINT(rect.right),UINT(rect.bottom),DXGI_FORMAT_UNKNOWN,0);
        if(SUCCEEDED(game->hr)) game->hr=game->chain->GetBuffer(0,IID_PPV_ARGS(&game->buffer));
        if(SUCCEEDED(game->hr)) {
            const auto desc=game->buffer->GetDesc();game->w=UINT(desc.Width);game->h=desc.Height;++game->calls;
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int run(test::Device& device,bool maximized) {
    Game game;
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"NA.FullscreenResize.GPU";
    REQUIRE(RegisterClassW(&wc) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"Hidden render-size fixture",WS_POPUP|(maximized?WS_MAXIMIZE:0),
        0,0,320,180,nullptr,nullptr,wc.hInstance,&game);
    REQUIRE(hwnd && !IsWindowVisible(hwnd));
    RECT physical{};{np::NativeScope native;REQUIRE(GetClientRect(hwnd,&physical));}
    const UINT W=physical.right,H=physical.bottom;
    REQUIRE(W>8 && H>8);
    REQUIRE(np::Attach(hwnd,{W,H},{W,H}));np::ObserveWindowMode(hwnd,true);ctl::Reset({W,H},{W,H},1.f);
    na::present::SetRenderScale(1.f,&unused_upscale,nullptr);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=W;desc.Height=H;desc.BufferCount=2;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(device.factory->CreateSwapChainForHwnd(device.queue.Get(),hwnd,&desc,nullptr,nullptr,&chain)));
    REQUIRE(SUCCEEDED(chain.As(&game.chain)));
    REQUIRE(SUCCEEDED(game.chain->GetBuffer(0,IID_PPV_ARGS(&game.buffer))));
    const LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_STYLE);
    RECT bounds{};{np::NativeScope native;REQUIRE(GetWindowRect(hwnd,&bounds));}
    uint64_t now=1000;
    for(float scale:{.667f,.5f,1.f}) {
        const UINT rw=UINT(std::lround(W*scale)),rh=UINT(std::lround(H*scale));
        CHECK(ctl::Choose({rw,rh},scale,now).kind==ctl::ActionKind::Ask);
        game.token=np::RequestRenderSize({rw,rh},true);REQUIRE(game.token);ctl::Asked(game.token,now);
        MSG message{};REQUIRE(PeekMessageW(&message,hwnd,np::ResizeMessage(),np::ResizeMessage(),PM_REMOVE));
        DispatchMessageW(&message);REQUIRE(SUCCEEDED(game.hr));
        CHECK(game.w==rw && game.h==rh);
        const auto allocated=ctl::Get();CHECK(allocated.buffers.w==rw && allocated.buffers.h==rh);
        for(unsigned frame=0;frame<3;++frame){ctl::Observe({game.w,game.h});now+=16;
            const auto action=ctl::Tick(now);
            CHECK(action.kind==(frame==2?ctl::ActionKind::Commit:ctl::ActionKind::None));}
        CHECK(np::CompleteResize(game.token,true,{rw,rh}));
        na::present::SetRenderScale(scale,&unused_upscale,nullptr);
        test::ComPtr<ID3D12Resource> native;
        REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(game.chain.Get(),0,IID_PPV_ARGS(&native))));
        CHECK(native->GetDesc().Width==W && native->GetDesc().Height==H);
        CHECK(GetWindowLongPtrW(hwnd,GWL_STYLE)==style);
        RECT after{};{np::NativeScope scope;REQUIRE(GetWindowRect(hwnd,&after));}
        CHECK(std::memcmp(&after,&bounds,sizeof after)==0);
        std::printf("%s: game=%ux%u physical=%ux%u\n",maximized?"maximized borderless":"popup",game.w,game.h,W,H);
    }
    CHECK(game.calls==3);
    game.buffer.Reset();game.chain.Reset();chain.Reset();np::Detach(hwnd);DestroyWindow(hwnd);
    return 0;
}
}
int main() {
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP",L"1");
    test::Device d;if(!test::CreateDevice(d))return test::kSkip;
    REQUIRE(na::hooking::Initialize() && na::overlay::StartCore("SIZING",nullptr) && np::InstallSizeHooks());
    na::present::SetEnabled(true);
    REQUIRE(run(d,false)==0);REQUIRE(run(d,true)==0);
    na::present::SetRenderScale(1.f,nullptr,nullptr);
    REQUIRE(na::overlay::Stop());
    return test::finish("fullscreen_resize_test");
}
