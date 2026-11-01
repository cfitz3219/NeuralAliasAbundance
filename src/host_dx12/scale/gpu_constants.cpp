#include "gpu_constants.hpp"
#include <algorithm>
#include <cstring>
namespace na::scale {
namespace {
bool buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE kind, D3D12_RESOURCE_STATES state,
    Microsoft::WRL::ComPtr<ID3D12Resource>& result)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = kind;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)));
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; return b;
}
}
std::shared_ptr<const GpuConstants> GpuConstants::Create(ID3D12Device* device, ID3D12Resource* source,
    uint64_t offset, const void* observed, size_t bytes, const std::vector<ConstantPatch>& patches, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* reason) -> std::shared_ptr<const GpuConstants> { error = reason; return {}; };
    if (!device || !source || !bytes || (offset & 255)) return fail("invalid source constant-buffer binding");
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > desc.Width || bytes > desc.Width - offset)
        return fail("source constant-buffer range is outside the resource");
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<IUnknown> actual, expected;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner->QueryInterface(IID_PPV_ARGS(&actual))) ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&expected))) || actual.Get() != expected.Get())
        return fail("source constant buffer belongs to a different device");
    D3D12_HEAP_PROPERTIES hp{};
    if (FAILED(source->GetHeapProperties(&hp, nullptr)) || hp.Type == D3D12_HEAP_TYPE_READBACK)
        return fail("source constant-buffer heap is not a GPU input");
    std::vector<uint8_t> replaced;
    if (!PatchConstants(observed, bytes, patches, replaced, error)) return {};
    auto copy = std::shared_ptr<GpuConstants>(new GpuConstants);
    copy->source_ = source; copy->source_offset_ = offset; copy->bytes_ = bytes;
    copy->fixed_source_state_ = hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_GPU_UPLOAD;
    for (const auto& p : patches) if (std::find(copy->offsets_.begin(), copy->offsets_.end(), p.offset) == copy->offsets_.end())
        copy->offsets_.push_back(p.offset);
    if (!buffer(device, (bytes + 255) & ~uint64_t(255), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, copy->owned_) ||
        !buffer(device, std::max<size_t>(4, copy->offsets_.size() * 4), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, copy->patch_data_)) return fail("owned constant-buffer preparation failed");
    void* mapped = nullptr;
    if (FAILED(copy->patch_data_->Map(0, nullptr, &mapped))) return fail("constant patch upload map failed");
    for (size_t i = 0; i < copy->offsets_.size(); ++i)
        std::memcpy(static_cast<uint8_t*>(mapped) + i * 4, replaced.data() + copy->offsets_[i], 4);
    copy->patch_data_->Unmap(0, nullptr);
    return copy;
}
bool GpuConstants::RecordBefore(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES source_state) const
{
    if (!list || (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE))
        return false;
    const bool readable = (source_state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
    if (fixed_source_state_ && !readable) return false;
    auto b = transition(owned_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
    if (!readable) {
        b = transition(source_.Get(), source_state, D3D12_RESOURCE_STATE_COPY_SOURCE); list->ResourceBarrier(1, &b);
    }
    list->CopyBufferRegion(owned_.Get(), 0, source_.Get(), source_offset_, bytes_);
    if (!readable) {
        b = transition(source_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_state); list->ResourceBarrier(1, &b);
    }
    for (size_t i = 0; i < offsets_.size(); ++i)
        list->CopyBufferRegion(owned_.Get(), offsets_[i], patch_data_.Get(), i * 4, 4);
    b = transition(owned_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    list->ResourceBarrier(1, &b);
    return true;
}
void GpuConstants::RecordAfter(ID3D12GraphicsCommandList* list) const {
    const auto b = transition(owned_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &b);
}
} // namespace na::scale
