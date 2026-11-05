// GPU breadcrumbs (diag/gpu_breadcrumbs.hpp, AMD FidelityFX Breadcrumbs): off = nothing
// recorded; on = a marked pass is registered, written by the GPU and named in the trail.
// Hardware or WARP; SKIP (77) without either.
#include "test_support.hpp"
#include "host_dx12/diag/gpu_breadcrumbs.hpp"

#include <cstdio>
#include <string>

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("gpu_breadcrumbs_test");
    namespace c = na::diag::crumbs;
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    {   // Off: no context, no markers.
        c::OnPresent(d.device.Get());
        { const c::Scope s(l.list.Get(), "off pass"); }
        CHECK(c::StatusLine().find("ready=0") != std::string::npos);
        CHECK(c::Status().empty());
    }
    c::SetEnabled(true);
    c::OnPresent(d.device.Get());
    std::printf("%s\n", c::StatusLine().c_str());
    CHECK(c::StatusLine().find("ready=1") != std::string::npos);
    REQUIRE(SUCCEEDED(l.list->Close()) && SUCCEEDED(l.alloc->Reset()) && SUCCEEDED(l.list->Reset(l.alloc.Get(), nullptr)));
    {
        const c::Scope s(l.list.Get(), "NeuralAlias test pass");
        const c::Scope inner(l.list.Get(), "NeuralAlias nested pass");
    }
    REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    const std::string line = c::StatusLine();
    std::printf("%s\n", line.c_str());
    CHECK(line.find("markers=2") != std::string::npos);
    CHECK(line.find("lists=1") != std::string::npos);
    CHECK(line.find("errors=0") != std::string::npos);
    const std::string trail = c::Status();
    std::printf("%s\n", trail.c_str());
    CHECK(!trail.empty() && trail != "breadcrumbs: status unavailable\r\n");
    c::OnPresent(d.device.Get()); // next frame: the list may register again
    c::SetEnabled(false);
    return test::finish("gpu_breadcrumbs_test");
}
