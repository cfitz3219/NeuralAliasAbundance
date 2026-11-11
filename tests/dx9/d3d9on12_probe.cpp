// DX9 plan step D0 (docs/DX9-PLAN.md): can a DX9 game's picture AND depth be
// read through D3D9On12, Windows' own DX9-on-DX12 layer?
//
// A tiny DX9 "game" is created through Direct3DCreate9On12 (on our own D3D12
// device and queue when 9On12 accepts it, else on its own). It clears a colour
// target to red and its D24S8 depth to 0.25, then draws the left half green at
// depth 0.5. Then, through IDirect3DDevice9On12, the colour texture, a plain
// depth-stencil surface and (if offered) an INTZ depth texture are unwrapped
// to their D3D12 resources, copied to readback memory on our queue, and
// checked: green/red, 0.5/0.25. Each is returned with our fence, and the DX9
// device then draws and presents again (the hand-back works).
//
// It also times unwrap + copy + return, and runs as 32-bit and 64-bit.
// Exit 0 = pass, 1 = fail, 77 = skipped (no D3D9On12 / no D3D12 here).
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#if __has_include(<d3d9on12.h>)
#include <d3d9on12.h>
#define NA_HAVE_9ON12 1
#endif
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_fail;
}

#ifdef NA_HAVE_9ON12
using PFN_Create9On12 = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);

struct D12 {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    bool warp = false;

    bool Make(ID3D12Device* existing)
    {
        if (existing) device = existing;
        if (!device) {
            if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                ComPtr<IDXGIFactory4> f;
                ComPtr<IDXGIAdapter> a;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumWarpAdapter(IID_PPV_ARGS(&a))) ||
                    FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                    return false;
                warp = true;
            }
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!queue && FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }
    void Wait(UINT64 v)
    {
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, event);
            WaitForSingleObject(event, 10000);
        }
    }
};

struct Vtx { float x, y, z, rhw; DWORD color; };

// Copies subresource 0 of `res` into readback memory on our queue; fills the bytes (tightly packed rows).
bool ReadBack(D12& d, ID3D12Resource* res, D3D12_RESOURCE_STATES state, std::vector<uint8_t>* out, UINT* row_bytes, UINT64* signal)
{
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rb = 0, total = 0;
    d.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rb, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    if (FAILED(d.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf))))
        return false;
    d.alloc->Reset();
    d.list->Reset(d.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) d.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = buf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) d.list->ResourceBarrier(1, &b);
    if (FAILED(d.list->Close())) return false;
    ID3D12CommandList* l[1] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, l);
    *signal = ++d.value;
    d.queue->Signal(d.fence.Get(), *signal);
    d.Wait(*signal);
    void* p = nullptr;
    D3D12_RANGE r{0, static_cast<SIZE_T>(total)};
    if (FAILED(buf->Map(0, &r, &p))) return false;
    *row_bytes = static_cast<UINT>(rb);
    out->resize(static_cast<size_t>(rb) * rows);
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(out->data() + size_t(y) * rb, static_cast<uint8_t*>(p) + fp.Offset + size_t(y) * fp.Footprint.RowPitch, static_cast<size_t>(rb));
    D3D12_RANGE none{0, 0};
    buf->Unmap(0, &none);
    return true;
}

const char* FormatName(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return "B8G8R8X8_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    default: return "other";
    }
}

// Depth value of one texel, whatever the D3D12 depth format behind the DX9 surface.
bool DepthAt(DXGI_FORMAT f, const uint8_t* texel, double* out)
{
    uint32_t u = 0;
    std::memcpy(&u, texel, 4);
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: *out = double(u & 0xffffffu) / double(0xffffff); return true;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: {
        float fl = 0;
        std::memcpy(&fl, texel, 4);
        *out = fl;
        return true;
    }
    default: return false;
    }
}
UINT DepthTexelBytes(DXGI_FORMAT f)
{
    return (f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS) ? 8 : 4;
}

struct Unwrapped {
    bool ok = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT w = 0, h = 0, samples = 0;
    std::vector<uint8_t> bytes;
    UINT row = 0;
    double ms = 0;
};

// Unwrap -> read back on our queue -> return with our fence. Tries common states for the copy.
Unwrapped UnwrapRead(IDirect3DDevice9On12* on12, D12& d, IDirect3DResource9* r9, const char* what)
{
    Unwrapped u;
    const auto t0 = std::chrono::steady_clock::now();
    ComPtr<ID3D12Resource> r12;
    HRESULT hr = on12->UnwrapUnderlyingResource(r9, d.queue.Get(), IID_PPV_ARGS(&r12));
    if (FAILED(hr) || !r12) {
        std::printf("     %s: unwrap failed (hr 0x%08lx)\n", what, static_cast<unsigned long>(hr));
        return u;
    }
    const D3D12_RESOURCE_DESC rd = r12->GetDesc();
    u.format = rd.Format;
    u.w = static_cast<UINT>(rd.Width);
    u.h = rd.Height;
    u.samples = rd.SampleDesc.Count;
    std::printf("     %s: D3D12 resource %ux%u, %s (%d), %u sample(s), flags 0x%x\n", what, u.w, u.h, FormatName(rd.Format), static_cast<int>(rd.Format),
                u.samples, static_cast<unsigned>(rd.Flags));
    // The spec does not say which state the resource is in; D3D9On12 keeps resources in COMMON between uses.
    UINT64 signal = 0;
    u.ok = rd.SampleDesc.Count == 1 && ReadBack(d, r12.Get(), D3D12_RESOURCE_STATE_COMMON, &u.bytes, &u.row, &signal);
    UINT64 v = signal ? signal : d.value;
    ID3D12Fence* f[1] = {d.fence.Get()};
    hr = on12->ReturnUnderlyingResource(r9, 1, &v, f);
    if (FAILED(hr)) {
        std::printf("     %s: return failed (hr 0x%08lx)\n", what, static_cast<unsigned long>(hr));
        u.ok = false;
    }
    u.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return u;
}
#endif

} // namespace

int main()
{
    std::printf("D3D9On12 probe, %d-bit\n", static_cast<int>(sizeof(void*) * 8));
#ifndef NA_HAVE_9ON12
    std::printf("SKIP: this Windows SDK has no d3d9on12.h\n");
    return 77;
#else
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<PFN_Create9On12>(GetProcAddress(d3d9, "Direct3DCreate9On12")) : nullptr;
    if (!create) { std::printf("SKIP: this Windows has no Direct3DCreate9On12\n"); return 77; }

    // Our own D3D12 device and queue first (that is how the real proxy will run); else let 9On12 make its own.
    D12 d;
    if (!d.Make(nullptr)) { std::printf("SKIP: no D3D12 device here\n"); return 77; }
    std::printf("our D3D12 device: %s\n", d.warp ? "WARP" : "hardware");

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"na_9on12_probe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, nullptr, nullptr, wc.hInstance, nullptr);

    const UINT W = 64, H = 32;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> dev;
    bool own_device = false;
    for (int attempt = 0; attempt < 2 && !dev; ++attempt) {
        D3D9ON12_ARGS args{};
        args.Enable9On12 = TRUE;
        if (attempt == 0) {
            args.pD3D12Device = d.device.Get();
            args.ppD3D12Queues[0] = d.queue.Get();
            args.NumQueues = 1;
        }
        d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
        if (!d3d) continue;
        for (DWORD vp : {DWORD(D3DCREATE_HARDWARE_VERTEXPROCESSING), DWORD(D3DCREATE_SOFTWARE_VERTEXPROCESSING)}) {
            if (SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp | D3DCREATE_FPU_PRESERVE, &pp, &dev))) break;
            dev.Reset();
        }
        if (dev) own_device = attempt == 0;
        else {
            std::printf("     CreateDevice through 9On12 %s our device failed\n", attempt == 0 ? "on" : "without");
            d3d.Reset();
        }
    }
    if (!dev) { std::printf("SKIP: no DX9 device through D3D9On12 here\n"); return 77; }

    ComPtr<IDirect3DDevice9On12> on12;
    Check(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&on12))) && on12, "the DX9 device is a D3D9On12 device");
    if (!on12) return 1;
    if (!own_device) {
        // 9On12 made its own D3D12 device: work on that one, with our own queue on it.
        ComPtr<ID3D12Device> inner;
        if (FAILED(on12->GetD3D12Device(IID_PPV_ARGS(&inner))) || !inner) { Check(false, "GetD3D12Device"); return 1; }
        d = D12{};
        if (!d.Make(inner.Get())) { Check(false, "a queue on 9On12's D3D12 device"); return 1; }
    }
    std::printf("9On12 runs on %s D3D12 device\n", own_device ? "OUR" : "its own");

    // ---- the "game": colour target + D24S8 depth (+ INTZ when offered)
    ComPtr<IDirect3DTexture9> color_tex;
    ComPtr<IDirect3DSurface9> color, depth, intz_surf;
    ComPtr<IDirect3DTexture9> intz;
    Check(SUCCEEDED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &color_tex, nullptr)), "colour render target (A8R8G8B8)");
    if (color_tex) color_tex->GetSurfaceLevel(0, &color);
    Check(SUCCEEDED(dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr)), "depth-stencil surface (D24S8)");
    const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const bool intz_ok = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntz)) &&
                         SUCCEEDED(dev->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &intz, nullptr));
    std::printf("     INTZ depth texture: %s\n", intz_ok ? "offered" : "not offered");
    if (intz) intz->GetSurfaceLevel(0, &intz_surf);
    if (!color || !depth) return 1;

    const D3DCOLOR kRed = D3DCOLOR_XRGB(255, 0, 0), kGreen = D3DCOLOR_XRGB(0, 255, 0);
    auto draw_frame = [&](IDirect3DSurface9* ds) {
        dev->SetRenderTarget(0, color.Get());
        dev->SetDepthStencilSurface(ds);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, kRed, 0.25f, 0);
        dev->BeginScene();
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        const float x1 = W / 2.f;
        const Vtx quad[4] = {{-0.5f, -0.5f, 0.5f, 1, kGreen}, {x1 - 0.5f, -0.5f, 0.5f, 1, kGreen},
                             {-0.5f, H - 0.5f, 0.5f, 1, kGreen}, {x1 - 0.5f, H - 0.5f, 0.5f, 1, kGreen}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));
        dev->EndScene();
    };

    auto check_color = [&](const Unwrapped& u, const char* what) {
        if (!u.ok || u.bytes.empty()) { Check(false, what); return; }
        // B8G8R8A8 / R8G8B8A8 at 4 bytes per texel; find green and red channels by format.
        const bool bgra = u.format == DXGI_FORMAT_B8G8R8A8_UNORM || u.format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                          u.format == DXGI_FORMAT_B8G8R8X8_UNORM || u.format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
        const uint8_t* l = u.bytes.data() + size_t(H / 2) * u.row + size_t(W / 4) * 4;
        const uint8_t* r = u.bytes.data() + size_t(H / 2) * u.row + size_t(3 * W / 4) * 4;
        const int lr = bgra ? l[2] : l[0], lg = l[1], rr = bgra ? r[2] : r[0], rg = r[1];
        std::printf("     %s: left rgb(%d,%d,..) right rgb(%d,%d,..)\n", what, lr, lg, rr, rg);
        Check(lg > 200 && lr < 50 && rr > 200 && rg < 50, what);
    };
    auto check_depth = [&](const Unwrapped& u, const char* what) {
        if (!u.ok || u.bytes.empty()) { Check(false, what); return; }
        const UINT tb = DepthTexelBytes(u.format);
        double dl = -1, dr = -1;
        const bool known = DepthAt(u.format, u.bytes.data() + size_t(H / 2) * u.row + size_t(W / 4) * tb, &dl) &&
                           DepthAt(u.format, u.bytes.data() + size_t(H / 2) * u.row + size_t(3 * W / 4) * tb, &dr);
        std::printf("     %s: left %.4f right %.4f\n", what, dl, dr);
        Check(known && std::fabs(dl - 0.5) < 0.01 && std::fabs(dr - 0.25) < 0.01, what);
    };

    // 1) depth-stencil surface
    draw_frame(depth.Get());
    dev->SetDepthStencilSurface(nullptr); // a game unbinds (or not) before the read; unwrap must not need it
    const Unwrapped uc = UnwrapRead(on12.Get(), d, color_tex.Get(), "colour texture");
    check_color(uc, "colour read through D3D12 (left green, right red)");
    const Unwrapped ud = UnwrapRead(on12.Get(), d, depth.Get(), "D24S8 depth surface");
    check_depth(ud, "D24S8 depth read through D3D12 (left 0.5, right 0.25)");
    std::printf("     unwrap + copy + return: colour %.2f ms, depth %.2f ms\n", uc.ms, ud.ms);

    // 2) INTZ depth texture (the classic readable-depth trick), when offered
    if (intz_surf) {
        draw_frame(intz_surf.Get());
        dev->SetDepthStencilSurface(nullptr);
        const Unwrapped ui = UnwrapRead(on12.Get(), d, intz.Get(), "INTZ depth texture");
        check_depth(ui, "INTZ depth read through D3D12 (left 0.5, right 0.25)");
    }

    // 3) after the hand-back, the DX9 device keeps working: draw again, read again, present
    draw_frame(depth.Get());
    const Unwrapped uc2 = UnwrapRead(on12.Get(), d, color_tex.Get(), "colour texture (second frame)");
    check_color(uc2, "DX9 keeps drawing after the resources were handed back");
    dev->SetRenderTarget(0, nullptr);
    ComPtr<IDirect3DSurface9> bb;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (bb) dev->SetRenderTarget(0, bb.Get());
    Check(SUCCEEDED(dev->StretchRect(color.Get(), nullptr, bb.Get(), nullptr, D3DTEXF_NONE)), "StretchRect to the back buffer");
    Check(SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");

    // 4) the steady-state cost of one round trip (what a frame would pay), averaged
    {
        const int n = 20;
        double total = 0;
        for (int i = 0; i < n; ++i) {
            draw_frame(depth.Get());
            const auto t0 = std::chrono::steady_clock::now();
            ComPtr<ID3D12Resource> r12;
            if (FAILED(on12->UnwrapUnderlyingResource(color_tex.Get(), d.queue.Get(), IID_PPV_ARGS(&r12)))) break;
            UINT64 v = ++d.value;
            d.queue->Signal(d.fence.Get(), v);
            ID3D12Fence* f[1] = {d.fence.Get()};
            on12->ReturnUnderlyingResource(color_tex.Get(), 1, &v, f);
            total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        std::printf("     unwrap + return (no copy, CPU side), average of %d: %.3f ms\n", n, total / n);
    }

    std::printf("%s: D3D9On12 probe (%d-bit)\n", g_fail ? "FAIL" : "PASS", static_cast<int>(sizeof(void*) * 8));
    return g_fail ? 1 : 0;
#endif
}
