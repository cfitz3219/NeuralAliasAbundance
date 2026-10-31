#include "test_support.hpp"
#include "host_dx12/overlay/boundary_fallback.hpp"
#include "host_dx12/host_boot/process_log.hpp"
int main() {
    na::overlay::BoundaryRoutes r;
    CHECK(!r.Select(0,2));CHECK(!r.Select(1,0));CHECK(!r.Select(1,1));
    CHECK(!r.Suppresses(2));CHECK(r.Select(1,2));
    for(int generated=0;generated<1000;++generated)CHECK(r.Suppresses(2));
    CHECK(r.OwnsProxy(1));CHECK(!r.Suppresses(3));
    CHECK(r.Select(3,4));CHECK(r.Suppresses(4));
    CHECK(r.Select(1,5));CHECK(!r.Suppresses(2));CHECK(r.Suppresses(5));
    r.Forget(1);CHECK(!r.Suppresses(5));CHECK(r.Suppresses(4));
    r.Forget(4);CHECK(!r.OwnsProxy(3));CHECK(!r.Suppresses(4));
    for(uintptr_t i=1;i<=8;++i)CHECK(r.Select(i,i+10));
    CHECK(!r.Select(9,19));r.Forget(1);CHECK(r.Select(9,19));
    r.Reset();CHECK(!r.Suppresses(19));
    wchar_t path[MAX_PATH]{};
    CHECK(na::host::ProcessLogPath(path,MAX_PATH,L"C:\\logs\\status.log"));
    CHECK(std::wstring(path)==L"C:\\logs\\status-"+std::to_wstring(GetCurrentProcessId())+L".log");
    CHECK(!na::host::ProcessLogPath(path,3,L"long.log") && !path[0]);
    return test::finish("overlay_boundary_fallback_test");
}
