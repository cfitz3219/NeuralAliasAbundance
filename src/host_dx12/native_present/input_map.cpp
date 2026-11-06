#include "host_dx12/native_present/input_map.hpp"
#include <windowsx.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>

namespace na::native_present {
namespace {
std::atomic<uint64_t> g_read{0}, g_write{0}, g_clip{0}, g_messages{0}, g_absolute{0};
LONG rounded(double x) {
    const double lo = (std::numeric_limits<LONG>::min)(), hi = (std::numeric_limits<LONG>::max)();
    return static_cast<LONG>(std::round((x < lo) ? lo : (x > hi) ? hi : x));
}
bool valid(const InputGeometry& g) {
    return g.render.w && g.render.h && g.physical_client.right > g.physical_client.left &&
        g.physical_client.bottom > g.physical_client.top;
}
LONG axis(LONG p, LONG origin, double from, double to) {
    return rounded(double(origin) + (double(p) - origin) * to / from);
}
LONG short_coord(LONG p) { return (p < -32768) ? -32768 : (p > 32767) ? 32767 : p; }
}
POINT ToGameClient(const InputGeometry& g, POINT p) {
    if (!valid(g)) return p;
    return {axis(p.x,0,double(g.physical_client.right)-g.physical_client.left,g.render.w),
            axis(p.y,0,double(g.physical_client.bottom)-g.physical_client.top,g.render.h)};
}
POINT ToNativeClient(const InputGeometry& g, POINT p) {
    if (!valid(g)) return p;
    return {axis(p.x,0,g.render.w,double(g.physical_client.right)-g.physical_client.left),
            axis(p.y,0,g.render.h,double(g.physical_client.bottom)-g.physical_client.top)};
}
POINT ToGameScreen(const InputGeometry& g, POINT p) {
    if (!valid(g)) return p;
    return {axis(p.x,g.physical_client.left,double(g.physical_client.right)-g.physical_client.left,g.render.w),
            axis(p.y,g.physical_client.top,double(g.physical_client.bottom)-g.physical_client.top,g.render.h)};
}
POINT ToNativeScreen(const InputGeometry& g, POINT p) {
    if (!valid(g)) return p;
    return {axis(p.x,g.physical_client.left,g.render.w,double(g.physical_client.right)-g.physical_client.left),
            axis(p.y,g.physical_client.top,g.render.h,double(g.physical_client.bottom)-g.physical_client.top)};
}
RECT ToNativeClip(const InputGeometry& g, RECT r) {
    if (!valid(g)) return r;
    const auto a = ToNativeScreen(g,{r.left,r.top}), b = ToNativeScreen(g,{r.right,r.bottom});
    return {a.x,a.y,b.x,b.y};
}
bool MapCursorRead(POINT* p, const void* caller) {
    InputGeometry g;
    if (!p || !GameCaller(caller) || !GetInputGeometry(&g)) return false;
    *p = ToGameScreen(g,*p); ++g_read; return true;
}
bool MapCursorWrite(int& x, int& y, const void* caller) {
    InputGeometry g;
    if (!GameCaller(caller) || !GetInputGeometry(&g)) return false;
    const auto p = ToNativeScreen(g,{x,y}); x=p.x; y=p.y; ++g_write; return true;
}
bool MapClipRect(const RECT* requested, RECT& physical, const void* caller) {
    InputGeometry g;
    if (!requested || !GameCaller(caller) || !GetInputGeometry(&g)) return false;
    physical = ToNativeClip(g,*requested); ++g_clip; return true;
}
bool MapClipRead(RECT* physical, const void* caller) {
    InputGeometry g;
    if (!physical || !GameCaller(caller) || !GetInputGeometry(&g)) return false;
    const auto a=ToGameScreen(g,{physical->left,physical->top}), b=ToGameScreen(g,{physical->right,physical->bottom});
    *physical={a.x,a.y,b.x,b.y}; return true;
}
bool MapGameMouseMessage(HWND hwnd, UINT msg, LPARAM& lp) {
    const auto s = GetSizeState();
    InputGeometry g;
    if (!s.enabled || s.hwnd != hwnd || !GetInputGeometry(&g)) return false;
    bool screen = false;
    switch (msg) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_POINTERUPDATE: case WM_POINTERDOWN: case WM_POINTERUP:
    case WM_POINTERENTER: case WM_POINTERLEAVE: case WM_POINTERWHEEL: case WM_POINTERHWHEEL:
        screen = true; break;
    default: return false;
    }
    POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
    p = screen ? ToGameScreen(g,p) : ToGameClient(g,p);
    lp = MAKELPARAM(static_cast<WORD>(short_coord(p.x)),static_cast<WORD>(short_coord(p.y)));
    ++g_messages; return true;
}
bool MapAbsoluteRaw(RAWMOUSE& m, const InputGeometry& g, RECT desktop) {
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) || !valid(g) || desktop.right-desktop.left < 2 || desktop.bottom-desktop.top < 2) return false;
    POINT p{rounded(desktop.left + double(m.lLastX) * (double(desktop.right)-desktop.left-1) / 65535.),
            rounded(desktop.top + double(m.lLastY) * (double(desktop.bottom)-desktop.top-1) / 65535.)};
    p=ToGameScreen(g,p);
    auto normalized=[](double v) { return static_cast<LONG>((v < 0.) ? 0. : (v > 65535.) ? 65535. : std::round(v)); };
    m.lLastX=normalized((double(p.x)-desktop.left)*65535./(double(desktop.right)-desktop.left-1));
    m.lLastY=normalized((double(p.y)-desktop.top)*65535./(double(desktop.bottom)-desktop.top-1));
    ++g_absolute; return true;
}
bool MapRawMouseRead(RAWMOUSE& m, const void* caller) {
    InputGeometry g;
    if (!(m.usFlags&MOUSE_MOVE_ABSOLUTE) || !GameCaller(caller) || !GetInputGeometry(&g)) return false;
    NativeScope native;
    RECT desktop{};
    if(m.usFlags&MOUSE_VIRTUAL_DESKTOP) {
        desktop.left=GetSystemMetrics(SM_XVIRTUALSCREEN); desktop.top=GetSystemMetrics(SM_YVIRTUALSCREEN);
        desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN); desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else { desktop.right=GetSystemMetrics(SM_CXSCREEN); desktop.bottom=GetSystemMetrics(SM_CYSCREEN); }
    return MapAbsoluteRaw(m,g,desktop);
}
InputStats GetInputStats() { return {g_read.load(),g_write.load(),g_clip.load(),g_messages.load(),g_absolute.load()}; }
void FormatInputStatus(char* out, uint32_t capacity) {
    if (!out || !capacity) return;
    const auto s=GetInputStats();
    std::snprintf(out,capacity,"native_input cursor_read=%llu cursor_write=%llu clip=%llu messages=%llu raw_absolute=%llu",
        static_cast<unsigned long long>(s.cursor_read),static_cast<unsigned long long>(s.cursor_write),
        static_cast<unsigned long long>(s.clipped),static_cast<unsigned long long>(s.messages),static_cast<unsigned long long>(s.absolute_raw));
}
} // namespace na::native_present
