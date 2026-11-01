#include "scene_group.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace na::scale {
namespace {
bool valid(Extent e) { return e.width && e.height; }
bool identity(ID3D12Resource* resource, Microsoft::WRL::ComPtr<IUnknown>& out) {
    return resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&out)));
}
float value(Dimension d, Extent e) {
    switch (d) {
    case Dimension::Width: return static_cast<float>(e.width);
    case Dimension::Height: return static_cast<float>(e.height);
    case Dimension::InverseWidth: return 1.f / e.width;
    case Dimension::InverseHeight: return 1.f / e.height;
    }
    return 0;
}
}
std::shared_ptr<const SceneGroup> SceneGroup::Create(ID3D12Device* device, uint64_t generation,
    Extent output, Extent render, const std::vector<MemberRequest>& requests, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* reason) -> std::shared_ptr<const SceneGroup> { error = reason; return {}; };
    if (!device || !generation || !valid(output) || !valid(render) || render.width > output.width ||
        render.height > output.height || requests.empty() || requests.size() > 128)
        return fail("invalid scene group dimensions or membership");
    auto group = std::shared_ptr<SceneGroup>(new SceneGroup);
    group->generation_ = generation;
    group->output_ = output;
    group->render_ = render;
    // Preflight the complete selection before allocating anything. Duplicate COM
    // interfaces to one resource are duplicates even if the caller uses two IDs.
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> identities;
    Microsoft::WRL::ComPtr<IUnknown> device_id;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device_id)))) return fail("device identity unavailable");
    for (const auto& request : requests) {
        Microsoft::WRL::ComPtr<IUnknown> id, owner_id;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if (!request.id || !valid(request.reduced) || !identity(request.native, id) ||
            FAILED(request.native->GetDevice(IID_PPV_ARGS(&owner))) ||
            FAILED(owner->QueryInterface(IID_PPV_ARGS(&owner_id))) || owner_id.Get() != device_id.Get())
            return fail("invalid member identity, dimensions or device");
        if (group->Find(request.id) || std::any_of(identities.begin(), identities.end(),
            [&](const auto& prior) { return prior.Get() == id.Get(); })) return fail("duplicate scene member");
        Member member;
        member.id = request.id;
        member.role = request.role;
        member.native = request.native;
        member.native_desc = request.native->GetDesc();
        const auto& d = member.native_desc;
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || request.reduced.width > d.Width ||
            request.reduced.height > d.Height || d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN ||
            (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)))
            return fail("member cannot use an independent reduced Texture2D");
        if ((request.role == Role::Depth) != ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0))
            return fail("depth role disagrees with resource capabilities");
        member.reduced_desc = d;
        member.reduced_desc.Width = request.reduced.width;
        member.reduced_desc.Height = request.reduced.height;
        member.reduced_desc.Alignment = 0; // independent committed layout, not the game's placed offset
        uint32_t max_mips = 1;
        for (uint32_t n = std::max(request.reduced.width, request.reduced.height); n > 1; n >>= 1) ++max_mips;
        if (!d.MipLevels || d.MipLevels > max_mips)
            return fail("reduced size cannot preserve the original mip view range");
        member.initial_state = request.initial_state;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &member.reduced_desc);
        if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes == 0)
            return fail("reduced resource descriptor is unsupported");
        identities.push_back(std::move(id));
        group->members_.push_back(std::move(member));
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    for (auto& member : group->members_) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &member.reduced_desc,
            member.initial_state, nullptr, IID_PPV_ARGS(&member.reduced))))
            return fail("reduced scene group allocation failed; native group preserved");
    }
    return group;
}
const Member* SceneGroup::Find(uint64_t id) const {
    for (const auto& m : members_) if (m.id == id) return &m;
    return nullptr;
}
void GroupSelection::Publish(std::shared_ptr<const SceneGroup> group) {
    std::atomic_store_explicit(&current_, std::move(group), std::memory_order_release);
}
void GroupSelection::Disable() { Publish({}); }
std::shared_ptr<const SceneGroup> GroupSelection::Acquire() const {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}
bool ScaleViewport(const D3D12_VIEWPORT& input, Extent from, Extent to, D3D12_VIEWPORT& result) {
    if (!valid(from) || !valid(to) || !std::isfinite(input.TopLeftX) || !std::isfinite(input.TopLeftY) ||
        !std::isfinite(input.Width) || !std::isfinite(input.Height) || input.Width < 0 || input.Height < 0 ||
        !std::isfinite(input.MinDepth) || !std::isfinite(input.MaxDepth)) return false;
    D3D12_VIEWPORT v = input;
    v.TopLeftX *= static_cast<float>(to.width) / from.width;
    v.TopLeftY *= static_cast<float>(to.height) / from.height;
    v.Width *= static_cast<float>(to.width) / from.width;
    v.Height *= static_cast<float>(to.height) / from.height;
    result = v;
    return true;
}
bool ScaleScissor(const D3D12_RECT& input, Extent from, Extent to, D3D12_RECT& result) {
    if (!valid(from) || !valid(to) || to.width > LONG_MAX || to.height > LONG_MAX ||
        input.right < input.left || input.bottom < input.top) return false;
    const auto edge = [](LONG n, uint32_t a, uint32_t b, bool upper) {
        const double scaled = static_cast<double>(n) * b / a;
        return static_cast<LONG>(std::clamp(upper ? std::ceil(scaled) : std::floor(scaled), 0., static_cast<double>(b)));
    };
    D3D12_RECT r{edge(input.left, from.width, to.width, false), edge(input.top, from.height, to.height, false),
        edge(input.right, from.width, to.width, true), edge(input.bottom, from.height, to.height, true)};
    // Empty scissors must remain empty; floor/ceil must not create a one-pixel draw.
    if (input.left == input.right) r.right = r.left;
    if (input.top == input.bottom) r.bottom = r.top;
    result = r;
    return true;
}
bool DispatchForExtent(Extent extent, uint32_t tx, uint32_t ty, UINT& x, UINT& y) {
    if (!valid(extent) || !tx || !ty) return false;
    const uint64_t gx = (static_cast<uint64_t>(extent.width) + tx - 1) / tx;
    const uint64_t gy = (static_cast<uint64_t>(extent.height) + ty - 1) / ty;
    if (gx > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION || gy > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
        return false;
    x = static_cast<UINT>(gx); y = static_cast<UINT>(gy);
    return true;
}
bool PatchConstants(const void* source, size_t bytes, const std::vector<ConstantPatch>& patches,
    std::vector<uint8_t>& destination, std::string& error)
{
    error.clear();
    if ((!source && bytes) || bytes > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16ull) {
        error = "invalid constant buffer"; return false;
    }
    struct Write { uint32_t offset, bits; };
    std::vector<Write> writes;
    for (const auto& p : patches) {
        if (!valid(p.native) || !valid(p.reduced) || p.offset > bytes || bytes - p.offset < sizeof(uint32_t) || (p.offset & 3)) {
            error = "constant patch outside its explicit binding"; return false;
        }
        uint32_t old_bits = 0, new_bits = 0;
        std::memcpy(&old_bits, static_cast<const uint8_t*>(source) + p.offset, sizeof old_bits);
        if (p.scalar == Scalar::UInt) {
            if (p.dimension != Dimension::Width && p.dimension != Dimension::Height) {
                error = "integer inverse extent is unsupported"; return false;
            }
            const uint32_t expected = p.dimension == Dimension::Width ? p.native.width : p.native.height;
            if (old_bits != expected) { error = "constant patch does not match this execution's dimensions"; return false; }
            new_bits = p.dimension == Dimension::Width ? p.reduced.width : p.reduced.height;
        } else {
            float old = 0;
            std::memcpy(&old, &old_bits, sizeof old);
            const float expected = value(p.dimension, p.native), replacement = value(p.dimension, p.reduced);
            if (!std::isfinite(old) || std::fabs(old - expected) > std::max(std::fabs(expected) * 1e-6f, 1e-9f)) {
                error = "constant patch does not match this execution's dimensions"; return false;
            }
            std::memcpy(&new_bits, &replacement, sizeof new_bits);
        }
        const auto prior = std::find_if(writes.begin(), writes.end(), [&](const Write& w) { return w.offset == p.offset; });
        if (prior != writes.end() && prior->bits != new_bits) { error = "conflicting constant patch roles"; return false; }
        writes.push_back({p.offset, new_bits});
    }
    std::vector<uint8_t> next(bytes);
    if (bytes) std::memcpy(next.data(), source, bytes);
    for (const auto& w : writes) std::memcpy(next.data() + w.offset, &w.bits, sizeof w.bits);
    destination = std::move(next);
    return true;
}
} // namespace na::scale
