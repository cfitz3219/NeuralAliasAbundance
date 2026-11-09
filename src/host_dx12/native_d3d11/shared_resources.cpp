#include "shared_resources.hpp"

#include <dxgi1_2.h>
#include <algorithm>

namespace na::native_d3d11 {
using Microsoft::WRL::ComPtr;
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};

HRESULT SameAdapter(ID3D11Device* native, ID3D12Device* effects)
{
    if (!native || !effects) return E_INVALIDARG;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = native->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = dxgi->GetAdapter(&adapter))) return hr;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(hr = adapter->GetDesc(&desc))) return hr;
    const LUID effects_luid = effects->GetAdapterLuid();
    return desc.AdapterLuid.LowPart == effects_luid.LowPart &&
           desc.AdapterLuid.HighPart == effects_luid.HighPart ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

DXGI_FORMAT CopyFamily(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return format;
    }
}

HRESULT CreateFencePair(ID3D11Device5* native, ID3D12Device* effects,
                       ComPtr<ID3D11Fence>& f11, ComPtr<ID3D12Fence>& f12)
{
    HRESULT hr = effects->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12));
    if (FAILED(hr)) return hr;
    Handle shared;
    if (FAILED(hr = effects->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value))) return hr;
    return native->OpenSharedFence(shared.value, IID_PPV_ARGS(&f11));
}
} // namespace

bool CompatibleImageCopy(const D3D11_TEXTURE2D_DESC& native, UINT subresource,
                         const ImageDesc& shared) noexcept
{
    if (!native.MipLevels || !native.ArraySize || !shared.width || !shared.height ||
        native.SampleDesc.Count != 1 || shared.format == DXGI_FORMAT_UNKNOWN ||
        subresource >= static_cast<uint64_t>(native.MipLevels) * native.ArraySize ||
        CopyFamily(native.Format) != CopyFamily(shared.format)) return false;
    const UINT mip = subresource % native.MipLevels;
    if (mip >= 32) return false;
    return std::max(1u, native.Width >> mip) == shared.width &&
           std::max(1u, native.Height >> mip) == shared.height;
}

HRESULT SharedTexture::Create(ID3D11Device* native, ID3D12Device* effects, const ImageDesc& desc)
{
    // Objects are created once per resource generation, never overwritten while
    // an old generation might be submitted. The owner retires the whole old slot.
    if (native_ || effects_) return DXGI_ERROR_INVALID_CALL;
    if (!desc.width || !desc.height || desc.format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
    HRESULT hr = SameAdapter(native, effects);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Device1> d11;
    if (FAILED(hr = native->QueryInterface(IID_PPV_ARGS(&d11)))) return hr;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC image{};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width = desc.width; image.Height = desc.height;
    image.DepthOrArraySize = 1; image.MipLevels = 1;
    image.Format = desc.format; image.SampleDesc.Count = 1;
    image.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // D3D11 shared images use simultaneous-access layout semantics. Ownership
    // is still serialized by fences; this flag does not permit concurrent writes.
    image.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc.writable12) image.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r12;
    if (FAILED(hr = effects->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &image,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r12)))) return hr;
    Handle shared;
    if (FAILED(hr = effects->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value))) return hr;
    ComPtr<ID3D11Texture2D> r11;
    if (FAILED(hr = d11->OpenSharedResource1(shared.value, IID_PPV_ARGS(&r11)))) return hr;
    D3D11_TEXTURE2D_DESC opened{};
    r11->GetDesc(&opened);
    if (opened.Width != desc.width || opened.Height != desc.height || opened.Format != desc.format ||
        opened.ArraySize != 1 || opened.MipLevels != 1 || opened.SampleDesc.Count != 1) return E_UNEXPECTED;
    native_ = std::move(r11); effects_ = std::move(r12); desc_ = desc;
    return S_OK;
}

HRESULT SharedFences::Create(ID3D11Device* native, ID3D12Device* effects, ID3D12CommandQueue* queue)
{
    if (context_ || queue_) return DXGI_ERROR_INVALID_CALL;
    if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    HRESULT hr = SameAdapter(native, effects);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D12Device> queue_device;
    if (FAILED(hr = queue->GetDevice(IID_PPV_ARGS(&queue_device)))) return hr;
    // D3D12 devices are singleton per adapter, but check object identity as well.
    ComPtr<IUnknown> a, b;
    if (FAILED(hr = queue_device.As(&a)) || FAILED(hr = effects->QueryInterface(IID_PPV_ARGS(&b)))) return hr;
    if (a.Get() != b.Get()) return E_INVALIDARG;
    ComPtr<ID3D11Device5> d11;
    if (FAILED(hr = native->QueryInterface(IID_PPV_ARGS(&d11)))) return hr;
    ComPtr<ID3D11DeviceContext> immediate;
    native->GetImmediateContext(&immediate);
    ComPtr<ID3D11DeviceContext4> context;
    if (FAILED(hr = immediate.As(&context))) return hr;
    ComPtr<ID3D11Fence> n11, e11;
    ComPtr<ID3D12Fence> n12, e12;
    if (FAILED(hr = CreateFencePair(d11.Get(), effects, n11, n12)) ||
        FAILED(hr = CreateFencePair(d11.Get(), effects, e11, e12))) return hr;
    context_ = std::move(context); queue_ = queue;
    native11_ = std::move(n11); native12_ = std::move(n12);
    effects11_ = std::move(e11); effects12_ = std::move(e12);
    return S_OK;
}

HRESULT SharedFences::Fail(HRESULT hr) { if (FAILED(hr)) failure_ = hr; return hr; }

HRESULT SharedFences::SignalNative(uint64_t* value)
{
    if (value) *value = 0;
    if (FAILED(failure_)) return failure_;
    if (!value || !context_ || native_value_ >= UINT64_MAX - 1) return E_INVALIDARG;
    const uint64_t next = native_value_ + 1;
    const HRESULT hr = context_->Signal(native11_.Get(), next);
    if (FAILED(hr)) return Fail(hr);
    // Submission, not completion: prevents waiting on DX11 work still buffered
    // on the CPU. Do not replace with GetData, CPU Wait, or Unwrap.
    context_->Flush();
    native_value_ = next; *value = next;
    return S_OK;
}

HRESULT SharedFences::WaitEffects(uint64_t value)
{
    if (FAILED(failure_)) return failure_;
    if (!queue_ || !value || value > native_value_) return E_INVALIDARG;
    return Fail(queue_->Wait(native12_.Get(), value));
}

HRESULT SharedFences::SignalEffects(uint64_t* value)
{
    if (value) *value = 0;
    if (FAILED(failure_)) return failure_;
    if (!value || !queue_ || effects_value_ >= UINT64_MAX - 1) return E_INVALIDARG;
    const uint64_t next = effects_value_ + 1;
    const HRESULT hr = queue_->Signal(effects12_.Get(), next);
    if (FAILED(hr)) return Fail(hr);
    effects_value_ = next; *value = next;
    return S_OK;
}

HRESULT SharedFences::WaitNative(uint64_t value)
{
    if (FAILED(failure_)) return failure_;
    if (!context_ || !value || value > effects_value_) return E_INVALIDARG;
    return Fail(context_->Wait(effects11_.Get(), value));
}

bool SharedFences::Retired(uint64_t native_value, uint64_t effects_value) const
{
    return native12_ && effects12_ && Completed(native12_->GetCompletedValue(), effects12_->GetCompletedValue(),
                                              native_value, effects_value);
}
} // namespace na::native_d3d11
