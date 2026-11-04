// S1/S2 regression: actual user32 hooks and hidden fixture HWNDs, no game.
#include "test_support.hpp"
#include "host_dx12/native_present/input_map.hpp"
#include "host_dx12/native_present/size_probe.hpp"
#include <windowsx.h>
#include <cstring>
#include <thread>
#include <atomic>
using namespace na::native_present;
extern "C" __declspec(dllimport) BOOL WINAPI FixtureClientRect(HWND,RECT*);
namespace {
struct Window {
    HWND hwnd = nullptr;
    explicit Window(const wchar_t* cls=L"STATIC", void* context=nullptr, DWORD style=WS_OVERLAPPEDWINDOW) {
        hwnd=CreateWindowExW(0,cls,L"NA hidden sizing fixture",style,
            -1700,120,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),context);
        CHECK(hwnd);
    }
    ~Window() { Detach(hwnd); DestroyWindow(hwnd); }
};
bool eq(POINT a,POINT b) { return a.x==b.x && a.y==b.y; }
RECT native_client(HWND h) { NativeScope n; RECT r{}; CHECK(GetClientRect(h,&r)); return r; }
InputGeometry geometry() { return {{-3440,100,0,1540},{2293,960},1}; }
void deliver(HWND hwnd,uint64_t token) {
    UINT msg=ResizeMessage(); WPARAM wp=static_cast<WPARAM>(token); LPARAM lp=0; WINDOWPOS scratch{};
    CHECK(PrepareSizeMessage(hwnd,msg,wp,lp,scratch)==MessageAction::Changed);
    CHECK(msg==WM_SIZE && LOWORD(lp)==1720 && HIWORD(lp)==720);
}
struct PostedGame { uint64_t token=0; Size queried{}; bool committed=false; unsigned calls=0; bool fullscreen=false; };
LRESULT CALLBACK posted_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_NCCREATE) SetWindowLongPtrW(hwnd,GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* game=reinterpret_cast<PostedGame*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    UINT msg=message; WPARAM wp=wparam; LPARAM lp=lparam; WINDOWPOS scratch{};
    const auto action=PrepareSizeMessage(hwnd,msg,wp,lp,scratch);
    if(action==MessageAction::Consume) return 0;
    if(game && game->token && msg==WM_SIZE && wp!=SIZE_MINIMIZED) {
        // Fullscreen/maximized handlers can treat SIZE_MAXIMIZED as a physical
        // monitor-layout event and ignore its client extent. A render-only
        // request must take the ordinary client-size branch instead.
        if (game->fullscreen && wp==SIZE_MAXIMIZED) return 0;
        GameScopeForTest scope; RECT r{};
        if(GetClientRect(hwnd,&r)) {
            game->queried={static_cast<UINT>(r.right),static_cast<UINT>(r.bottom)}; ++game->calls;
            // Real game callback re-enters the helper. This would deadlock if
            // request dispatch held its state mutex while calling the game.
            const auto current=GetSizeState();
            game->committed=current.request==game->token && CompleteResize(game->token,true,game->queried);
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int main() {
    if (!InstallSizeHooks()) { std::fprintf(stderr,"size hook installation failed\n"); return 1; }
    return test::run({
        {"real hook sees game module, native bypass nests and other HWND stays physical", [] {
            Window a,b; const auto physical=native_client(a.hwnd);
            CHECK(Attach(a.hwnd,{3440,1440},{2293,960}));
            RECT r{};
            { GameScopeForTest game;
                CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==2293 && r.bottom==960);
                CHECK(GetClientRect(b.hwnd,&r)); CHECK(r.right==native_client(b.hwnd).right);
                { NativeScope n; { NativeScope n2; CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==physical.right); }
                    CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==physical.right); }
                CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==2293);
            }
            CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==physical.right); // same-module native exemption
            CHECK(!GameCaller(reinterpret_cast<const void*>(&GetTickCount64))); // system callers stay native
            Detach(a.hwnd);
            { GameScopeForTest game; CHECK(GetClientRect(a.hwnd,&r)); CHECK(r.right==physical.right); }
        }},
        {"outer window preserves origin and nonclient border", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            RECT physical{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&physical)); }
            const auto client=native_client(w.hwnd);
            RECT changed{}; { GameScopeForTest game; CHECK(GetWindowRect(w.hwnd,&changed)); }
            CHECK(changed.left==physical.left && changed.top==physical.top);
            CHECK(changed.right-changed.left==physical.right-physical.left-client.right+2293);
            RECT after{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&after)); }
            CHECK(std::memcmp(&after,&physical,sizeof after)==0);
        }},
        {"foreign overlay stays native and registered engine DLL gets game geometry", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960})); RECT r{};
            CHECK(FixtureClientRect(w.hwnd,&r)); CHECK(r.right==native_client(w.hwnd).right);
            CHECK(RegisterGameModule(reinterpret_cast<const void*>(&FixtureClientRect)));
            CHECK(FixtureClientRect(w.hwnd,&r)); CHECK(r.right==2293 && r.bottom==960);
            { NativeScope native; CHECK(FixtureClientRect(w.hwnd,&r)); CHECK(r.right==native_client(w.hwnd).right); }
        }},
        {"descriptors preserve format flags refresh HWND and sample fields", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            DXGI_SWAP_CHAIN_DESC d{}; d.OutputWindow=w.hwnd; d.BufferDesc.Width=3440; d.BufferDesc.Height=1440;
            d.BufferDesc.RefreshRate={144,1}; d.BufferDesc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
            d.BufferCount=3; d.SampleDesc={1,0}; d.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            const auto original=d; CHECK(RewriteDesc(&d));
            CHECK(d.BufferDesc.Width==2293 && d.BufferDesc.Height==960);
            d.BufferDesc.Width=original.BufferDesc.Width; d.BufferDesc.Height=original.BufferDesc.Height;
            CHECK(std::memcmp(&d,&original,sizeof d)==0);
            DXGI_SWAP_CHAIN_DESC1 d1{}; d1.Width=3440; d1.Height=1440; d1.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
            CHECK(RewriteDesc1(w.hwnd,&d1)); CHECK(d1.Width==2293 && d1.AlphaMode==DXGI_ALPHA_MODE_IGNORE);
        }},
        {"display mode generation sees borderless and exclusive changes at the same output size", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            ObserveWindowMode(w.hwnd,true); const auto first=GetSizeState(); CHECK(first.mode_generation);
            ObserveWindowMode(w.hwnd,true); CHECK(GetSizeState().mode_generation==first.mode_generation);
            const LONG_PTR style=GetWindowLongPtrW(w.hwnd,GWL_STYLE);
            SetWindowLongPtrW(w.hwnd,GWL_STYLE,WS_POPUP);
            ObserveWindowMode(w.hwnd,true); const auto borderless=GetSizeState();
            CHECK(borderless.mode_generation!=first.mode_generation);
            ObserveWindowMode(w.hwnd,false); const auto exclusive=GetSizeState();
            CHECK(exclusive.mode_generation!=borderless.mode_generation);
            CHECK(exclusive.render.w==2293 && exclusive.output.w==3440 && exclusive.request==0);
            ObserveWindowMode(w.hwnd,false); CHECK(GetSizeState().mode_generation==exclusive.mode_generation);
            SetWindowLongPtrW(w.hwnd,GWL_STYLE,style); ObserveWindowMode(w.hwnd,true);
            CHECK(GetSizeState().mode_generation!=exclusive.mode_generation);
        }},
        {"physical maximize and minimize messages retain their OS meaning", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            UINT msg=WM_SIZE; WPARAM wp=SIZE_MAXIMIZED; LPARAM lp=MAKELPARAM(3440,1440); WINDOWPOS scratch{};
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Changed);
            CHECK(msg==WM_SIZE && wp==SIZE_MAXIMIZED && LOWORD(lp)==2293);
            wp=SIZE_MINIMIZED; lp=0;
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Forward && wp==SIZE_MINIMIZED && lp==0);
        }},
        {"live resize advertises on delivery and commits mapping only on matching success", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            InputGeometry before{}; CHECK(GetInputGeometry(&before));
            const auto token=RequestRenderSize({1720,720}); CHECK(token);
            RECT r{}; CHECK(RewriteClientRect(w.hwnd,&r)); CHECK(r.right==2293); // not delivered yet
            CHECK(RequestRenderSize({1147,480})==0); // exactly one request
            deliver(w.hwnd,token); CHECK(RewriteClientRect(w.hwnd,&r)); CHECK(r.right==1720);
            InputGeometry pending{}; CHECK(GetInputGeometry(&pending));
            CHECK(pending.render.w==2293 && pending.generation==before.generation);
            CHECK(!CompleteResize(token+1,true,{1720,720}));
            CHECK(CompleteResize(token,true,{1720,720}));
            InputGeometry committed{}; CHECK(GetInputGeometry(&committed));
            CHECK(committed.render.w==1720 && committed.generation==token);
            CHECK(!CompleteResize(token,true,{1720,720}));
            CHECK(RequestRenderSize({1720,720})==0);
        }},
        {"posted resize reaches game WndProc without moving HWND or deadlocking re-entry", [] {
            WNDCLASSW cls{}; cls.lpfnWndProc=posted_proc; cls.hInstance=GetModuleHandleW(nullptr);
            cls.lpszClassName=L"NA.NativeSizing.PostedFixture";
            CHECK(RegisterClassW(&cls) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
            PostedGame game; Window w(cls.lpszClassName,&game);
            CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            RECT before{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&before)); }
            game.token=RequestRenderSize({1720,720}); CHECK(game.token);
            MSG message{}; CHECK(PeekMessageW(&message,w.hwnd,ResizeMessage(),ResizeMessage(),PM_REMOVE));
            DispatchMessageW(&message);
            CHECK(game.calls==1 && game.committed && game.queried.w==1720 && game.queried.h==720);
            CHECK(GetSizeState().request==0 && GetSizeState().render.w==1720);
            RECT after{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&after)); }
            CHECK(std::memcmp(&before,&after,sizeof before)==0);
        }},
        {"render-only resize works on maximized borderless HWND without changing its state", [] {
            WNDCLASSW cls{}; cls.lpfnWndProc=posted_proc; cls.hInstance=GetModuleHandleW(nullptr);
            cls.lpszClassName=L"NA.NativeSizing.FullscreenFixture";
            CHECK(RegisterClassW(&cls) || GetLastError()==ERROR_CLASS_ALREADY_EXISTS);
            PostedGame game; game.fullscreen=true;
            Window w(cls.lpszClassName,&game,WS_POPUP|WS_MAXIMIZE);
            CHECK(IsZoomed(w.hwnd) && !IsWindowVisible(w.hwnd));
            CHECK(Attach(w.hwnd,{3440,1440},{3440,1440}));
            const auto style=GetWindowLongPtrW(w.hwnd,GWL_STYLE);
            RECT before{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&before)); }
            for (const Size target : {Size{2293,960},Size{1720,720},Size{3440,1440}}) {
                game.committed=false; game.token=RequestRenderSize(target,true); CHECK(game.token);
                MSG message{}; CHECK(PeekMessageW(&message,w.hwnd,ResizeMessage(),ResizeMessage(),PM_REMOVE));
                DispatchMessageW(&message);
                CHECK(game.committed && game.queried.w==target.w && game.queried.h==target.h);
                CHECK(GetSizeState().render.w==target.w && GetSizeState().render.h==target.h);
                CHECK(IsZoomed(w.hwnd) && GetWindowLongPtrW(w.hwnd,GWL_STYLE)==style);
                RECT after{}; { NativeScope n; CHECK(GetWindowRect(w.hwnd,&after)); }
                CHECK(std::memcmp(&before,&after,sizeof before)==0);
            }
            CHECK(game.calls==3);
        }},
        {"failed resize restores old advertised geometry and allows retry", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            const auto token=RequestRenderSize({1720,720}); CHECK(token); deliver(w.hwnd,token);
            CHECK(!CompleteResize(token,false,{}));
            RECT r{}; CHECK(RewriteClientRect(w.hwnd,&r)); CHECK(r.right==2293);
            const auto retry=RequestRenderSize({1720,720}); CHECK(retry>token); deliver(w.hwnd,retry);
            CHECK(!CompleteResize(retry,true,{1719,720})); CHECK(GetSizeState().render.w==2293);
            const auto final=RequestRenderSize({1720,720}); CHECK(final>retry);
            CHECK(!CompleteResize(retry,true,{1720,720})); deliver(w.hwnd,final);
            CHECK(CompleteResize(final,true,{1720,720}));
        }},
        {"physical window resize supplies exact presenter rounding and supersedes queued mode", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            const auto old=RequestRenderSize({1720,720}); CHECK(old);
            CHECK(!GetSizeState().request_window); // our own ask
            const auto token=BeginWindowResize(w.hwnd,{2560,1440},{1707,960}); CHECK(token>old);
            CHECK(BeginWindowResize(w.hwnd,{2560,1440},{1707,960})==token);
            CHECK(!CompleteResize(old,true,{1720,720}));
            CHECK(GetSizeState().request_window); // a real resize: completes at the allocation
            UINT msg=WM_SIZE; WPARAM wp=SIZE_RESTORED; LPARAM lp=MAKELPARAM(2560,1440); WINDOWPOS scratch{};
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Changed);
            CHECK(LOWORD(lp)==1707 && HIWORD(lp)==960);
            CHECK(GetSizeState().render.w==2293 && GetSizeState().output.w==3440);
            CHECK(CompleteResize(token,true,{1707,960}));
            CHECK(GetSizeState().render.w==1707 && GetSizeState().output.w==2560 && !GetSizeState().request_window);
        }},
        {"window position message uses scratch and minimization stays zero", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            WINDOWPOS pos{w.hwnd,nullptr,-1700,120,1000,700,SWP_NOZORDER}; const auto original=pos;
            WINDOWPOS scratch{}; UINT msg=WM_WINDOWPOSCHANGED; WPARAM wp=0; LPARAM lp=reinterpret_cast<LPARAM>(&pos);
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Changed);
            CHECK(lp==reinterpret_cast<LPARAM>(&scratch)); CHECK(std::memcmp(&pos,&original,sizeof pos)==0);
            CHECK(scratch.x==pos.x && scratch.flags==pos.flags);
            CHECK(scratch.cx==1000-native_client(w.hwnd).right+2293);
            msg=WM_SIZE; wp=SIZE_MINIMIZED; lp=0;
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Forward && lp==0);
        }},
        {"ignored resize expires without a window change or a stuck pending request", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960}));
            const auto token=RequestRenderSize({1720,720}); CHECK(token); deliver(w.hwnd,token);
            CHECK(!ExpireResize(5000,1)); CHECK(ExpireResize(5000,GetTickCount64()+5001));
            CHECK(GetSizeState().request==0 && GetSizeState().render.w==2293);
            UINT msg=ResizeMessage(); WPARAM wp=static_cast<WPARAM>(token); LPARAM lp=0; WINDOWPOS scratch{};
            CHECK(PrepareSizeMessage(w.hwnd,msg,wp,lp,scratch)==MessageAction::Consume);
            CHECK(RequestRenderSize({1720,720})!=0);
        }},
        {"roundtrip mapping keeps signed monitor origin and full clip extent", [] {
            const auto g=geometry();
            CHECK(eq(ToGameClient(g,{3440,1440}),{2293,960}));
            CHECK(eq(ToGameScreen(g,{-3440,100}),{-3440,100}));
            for (LONG y=0;y<=1440;y+=37) for (LONG x=0;x<=3440;x+=29) {
                auto a=ToGameClient(g,{x,y}); auto b=ToNativeClient(g,a);
                CHECK(std::abs(b.x-x)<=1 && std::abs(b.y-y)<=1);
                auto c=ToNativeScreen(g,ToGameScreen(g,{x-3440,y+100}));
                CHECK(std::abs(c.x-(x-3440))<=1 && std::abs(c.y-(y+100))<=1);
            }
            auto clip=ToNativeClip(g,{-3440,100,-3440+2293,1060});
            CHECK(std::memcmp(&clip,&g.physical_client,sizeof clip)==0);
            CHECK(eq(ToGameClient({}, {-7,5}),{-7,5}));
        }},
        {"cursor poll and message positions agree and overlay reads bypass mapping", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960})); InputGeometry g{}; CHECK(GetInputGeometry(&g));
            POINT real{g.physical_client.left+100,g.physical_client.top+200};
            POINT polled=real;
            { GameScopeForTest game; CHECK(MapCursorRead(&polled,nullptr)); }
            POINT local{polled.x-g.physical_client.left,polled.y-g.physical_client.top};
            LPARAM lp=MAKELPARAM(100,200); CHECK(MapGameMouseMessage(w.hwnd,WM_MOUSEMOVE,lp));
            CHECK(eq(local,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}));
            LPARAM wheel=MAKELPARAM(real.x,real.y); CHECK(MapGameMouseMessage(w.hwnd,WM_MOUSEWHEEL,wheel));
            CHECK(eq(polled,{GET_X_LPARAM(wheel),GET_Y_LPARAM(wheel)}));
            int x=polled.x,y=polled.y;
            { GameScopeForTest game; CHECK(MapCursorWrite(x,y,nullptr)); }
            CHECK(std::abs(x-real.x)<=1 && std::abs(y-real.y)<=1);
            RECT clip{g.physical_client.left,g.physical_client.top,g.physical_client.right,g.physical_client.bottom};
            { GameScopeForTest game; CHECK(MapClipRead(&clip,nullptr));
                RECT unscaled{}; CHECK(MapClipRect(&clip,unscaled,nullptr)); CHECK(std::memcmp(&unscaled,&g.physical_client,sizeof clip)==0); }
            { GameScopeForTest game; NativeScope native; POINT p=real; CHECK(!MapCursorRead(&p,nullptr)); CHECK(eq(p,real)); }
            LPARAM key=42; CHECK(!MapGameMouseMessage(w.hwnd,WM_KEYDOWN,key)); CHECK(key==42);
        }},
        {"raw relative motion untouched while absolute devices retain buttons", [] {
            const auto g=geometry(); RAWMOUSE relative{}; relative.lLastX=-23; relative.lLastY=17;
            const auto saved=relative; CHECK(!MapAbsoluteRaw(relative,g,{-3440,0,3440,1440}));
            CHECK(std::memcmp(&relative,&saved,sizeof relative)==0);
            RAWMOUSE absolute{}; absolute.usFlags=MOUSE_MOVE_ABSOLUTE|MOUSE_VIRTUAL_DESKTOP;
            absolute.usButtonFlags=RI_MOUSE_LEFT_BUTTON_DOWN; absolute.ulExtraInformation=12345;
            absolute.lLastX=32768; absolute.lLastY=32768;
            CHECK(MapAbsoluteRaw(absolute,g,{-3440,0,3440,1440}));
            CHECK(absolute.usFlags==(MOUSE_MOVE_ABSOLUTE|MOUSE_VIRTUAL_DESKTOP));
            CHECK(absolute.usButtonFlags==RI_MOUSE_LEFT_BUTTON_DOWN && absolute.ulExtraInformation==12345);
            CHECK(absolute.lLastX<32768);
        }},
        {"parallel input snapshots cannot mix dimensions from different commits", [] {
            Window w; CHECK(Attach(w.hwnd,{3440,1440},{2293,960})); std::atomic<bool> bad{false};
            std::thread reader([&] { for (unsigned i=0;i<3000;++i) {
                auto s=GetSizeState(); if (!((s.render.w==2293 && s.render.h==960)||(s.render.w==1720 && s.render.h==720))) bad=true;
            }});
            for (unsigned i=0;i<1000;++i) CHECK(CommitBuffers(w.hwnd,{3440,1440}, i%2 ? Size{2293,960}:Size{1720,720}));
            reader.join(); CHECK(!bad);
        }},
        {"status is bounded and disabled geometry is transparent", [] {
            char short_status[8]; std::memset(short_status,0x55,sizeof short_status); FormatSizeStatus(short_status,sizeof short_status); CHECK(short_status[7]==0);
            char large[1024]; FormatSizeStatus(large,sizeof large); CHECK(std::strstr(large,"queries{client="));
            FormatInputStatus(large,sizeof large); CHECK(std::strstr(large,"raw_absolute="));
            CHECK(GetSizeStats().client_queries>0 && GetSizeStats().completed>0 && GetSizeStats().failed>0);
            InputGeometry g{}; CHECK(!GetInputGeometry(&g));
        }},
        {"size probe records game size queries without changing answers", [] {
            CHECK(InstallSizeProbe());
            ResetSizeProbeForTest();
            const int real_cx = [] { NativeScope n; return GetSystemMetrics(SM_CXSCREEN); }();
            HMONITOR m = nullptr; MONITORINFO mi{}; mi.cbSize = sizeof mi;
            int seen_cx = 0;
            { GameScopeForTest game;
                seen_cx = GetSystemMetrics(SM_CXSCREEN);
                (void)GetSystemMetrics(SM_CXICON); // not a screen metric: not counted
                m = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
                CHECK(m && GetMonitorInfoW(m, &mi));
                DEVMODEW dm{}; dm.dmSize = sizeof dm; CHECK(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm));
            }
            CHECK(seen_cx == real_cx); // the probe never changes a value
            (void)GetSystemMetrics(SM_CXSCREEN); // outside the game scope: not counted
            char line[2400]; FormatSizeProbe(line, sizeof line);
            std::printf("%s\n", line);
            CHECK(std::strstr(line, "system_metrics{n=1 "));
            CHECK(std::strstr(line, "monitor_from_window{n=1 "));
            CHECK(std::strstr(line, "monitor_info{n=1 "));
            CHECK(std::strstr(line, "enum_display_settings{n=1 "));
            char tiny[6]; std::memset(tiny, 0x55, sizeof tiny); FormatSizeProbe(tiny, sizeof tiny); CHECK(tiny[5] == 0);
        }}
    });
}
