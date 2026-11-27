// Live Frame Generation: the HUD sheet's state at the final draw comes only from barriers recorded in
// the same recording (core/live_frame_gen.hpp ResolveSheetState), here with the real per-list
// barrier tracker (nr/fg_states) on real D3D12 command lists:
//  - a transition naming subresource 0 only (the case Cort's 2026-11-26 run refused) is known;
//  - a split barrier is not usable until its END half;
//  - a barrier recorded in ANOTHER list never resolves the state of this one (recording order and
//    execution order may differ), and the HUD copy in another recording is refused;
//  - a Reset forgets everything (a reused list is a new recording).
#include "test_support.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "core/live_frame_gen.hpp"

#include <cstring>

using Microsoft::WRL::ComPtr;
using na::fglive::Lookup;

namespace {
na::fglive::StateSource tracker(ID3D12GraphicsCommandList* l, ID3D12Resource* r)
{
    D3D12_RESOURCE_STATES s{};
    const auto e = na::nr::states::Inspect(l, r, s);
    if (e == na::nr::states::Evidence::Known) return {Lookup::Known, static_cast<uint32_t>(s)};
    if (e == na::nr::states::Evidence::NoTransition) return {Lookup::Absent, 0};
    return {Lookup::Unavailable, 0};
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, D3D12_RESOURCE_BARRIER_FLAGS f = D3D12_RESOURCE_BARRIER_FLAG_NONE)
{
    D3D12_RESOURCE_BARRIER t{};
    t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Flags = f;
    t.Transition = {r, sub, a, b};
    return t;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("fg_sheet_state_test");
    auto sheet = test::CreateTexture(d.device.Get(), 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(sheet);
    test::List a, b;
    REQUIRE(a.Create(d.device.Get()) && b.Create(d.device.Get()));
    constexpr auto RT = D3D12_RESOURCE_STATE_RENDER_TARGET, PSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // (a) HUD draws and the final draw in list A; the game moves subresource 0 only.
    na::nr::states::Reset(a.list.Get());
    const auto t0 = transition(sheet.Get(), 0, RT, PSR);
    na::nr::states::Observe(a.list.Get(), 1, &t0);
    {
        na::fglive::SheetStateInputs in;
        in.same_recording_as_copy = true;
        in.whole = {Lookup::Absent, 0};  // the main record has no whole-resource transition
        in.sub0 = {Lookup::Known, PSR};  // what the subresource lookup of the main record answers
        in.tracker = tracker(a.list.Get(), sheet.Get());
        CHECK(in.tracker.lookup == Lookup::Known && in.tracker.state == PSR);
        const auto r = na::fglive::ResolveSheetState(in);
        CHECK(r.known && r.state == PSR);
        // the tracker alone (main record incomplete) is enough as well
        in.whole = in.sub0 = {Lookup::Unavailable, 0};
        CHECK(na::fglive::ResolveSheetState(in).known);
    }
    // Split barrier: BEGIN only is not a state; END makes it one.
    na::nr::states::Reset(a.list.Get());
    const auto begin = transition(sheet.Get(), 0, RT, PSR, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
    na::nr::states::Observe(a.list.Get(), 1, &begin);
    CHECK(tracker(a.list.Get(), sheet.Get()).lookup == Lookup::Unavailable);
    const auto end = transition(sheet.Get(), 0, RT, PSR, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);
    na::nr::states::Observe(a.list.Get(), 1, &end);
    CHECK(tracker(a.list.Get(), sheet.Get()).lookup == Lookup::Known);

    // (b) The barrier was recorded in list A, the final draw is in list B: B knows nothing, and a
    // HUD copy taken in another recording is refused outright.
    na::nr::states::Reset(b.list.Get());
    {
        na::fglive::SheetStateInputs in;
        in.same_recording_as_copy = false;
        in.whole = {Lookup::Absent, 0};
        in.sub0 = {Lookup::Absent, 0};
        in.tracker = tracker(b.list.Get(), sheet.Get());
        CHECK(in.tracker.lookup == Lookup::Absent);
        const auto r = na::fglive::ResolveSheetState(in);
        CHECK(!r.known && std::strstr(r.why, "another command list"));
        in.same_recording_as_copy = true; // even claiming the same recording, nothing was seen in B
        CHECK(!na::fglive::ResolveSheetState(in).known);
    }
    // A Reset of A (the list reused for the next frame) forgets its barriers.
    na::nr::states::Reset(a.list.Get());
    CHECK(tracker(a.list.Get(), sheet.Get()).lookup == Lookup::Absent);
    a.list->Close();
    b.list->Close();
    return test::finish("fg_sheet_state_test");
}
