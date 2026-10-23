#include <windows.h>
#include <atomic>
#include "proxy_api.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
namespace {
std::atomic<uint64_t> factories{0};
std::atomic<bool> loaded{false};
// Lazy initialization occurs on first exported call, never inside DllMain.
// The system library stays loaded for process lifetime, matching proxy lifetime.
HMODULE system_dxgi() {
    static HMODULE module=[] {
        wchar_t directory[MAX_PATH+16]{};
        const UINT count=GetSystemDirectoryW(directory,MAX_PATH);
        if(!count || count>=MAX_PATH) return static_cast<HMODULE>(nullptr);
        if(wcscat_s(directory,L"\\dxgi.dll")!=0) return static_cast<HMODULE>(nullptr);
        const HMODULE result=LoadLibraryExW(directory,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        loaded.store(result!=nullptr,std::memory_order_release);return result;
    }();
    return module;
}
template<class Function> Function resolve(const char* name) {
    const auto module=system_dxgi();
    return module?reinterpret_cast<Function>(GetProcAddress(module,name)):nullptr;
}
HRESULT factory(const char* name,REFIID iid,void** result) {
    if(!result) return E_POINTER;
    *result=nullptr;
    using Function=HRESULT(WINAPI*)(REFIID,void**);
    const auto function=resolve<Function>(name);
    if(!function) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    factories.fetch_add(1,std::memory_order_relaxed);return function(iid,result);
}
}
extern "C" HRESULT WINAPI AbundanceCreateDXGIFactory(REFIID iid,void** result) {
    return factory("CreateDXGIFactory",iid,result);
}
extern "C" HRESULT WINAPI AbundanceCreateDXGIFactory1(REFIID iid,void** result) {
    return factory("CreateDXGIFactory1",iid,result);
}
extern "C" HRESULT WINAPI AbundanceCreateDXGIFactory2(UINT flags,REFIID iid,void** result) {
    if(!result) return E_POINTER;
    *result=nullptr;
    using Function=HRESULT(WINAPI*)(UINT,REFIID,void**);
    const auto function=resolve<Function>("CreateDXGIFactory2");
    if(!function) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    factories.fetch_add(1,std::memory_order_relaxed);return function(flags,iid,result);
}
extern "C" HRESULT WINAPI AbundanceDXGIGetDebugInterface1(UINT flags,REFIID iid,void** result) {
    if(!result) return E_POINTER;
    *result=nullptr;
    using Function=HRESULT(WINAPI*)(UINT,REFIID,void**);
    const auto function=resolve<Function>("DXGIGetDebugInterface1");
    return function?function(flags,iid,result):HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
extern "C" HRESULT WINAPI AbundanceDXGIDeclareAdapterRemovalSupport() {
    using Function=HRESULT(WINAPI*)();
    const auto function=resolve<Function>("DXGIDeclareAdapterRemovalSupport");
    return function?function():HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
extern "C" int __cdecl AbundanceQueryProxyStatus(abundance::ProxyStatus* status) {
    if(!status || status->size!=sizeof(*status)) return 0;
    // A54: `reserved` reports the host bootstrap: bit0 host enabled by ini, bit1 host
    // started, bit2 adapter prepared, bit3 adapter started; A61: bit4 Route A
    // services enabled by ini, bit5 Route A services started (NGX core watched).
    const na::host::Status h=na::host::GetStatus();
    const uint32_t host_bits=(h.host_enabled?1u:0u)|(h.host_started?2u:0u)|(h.adapter_prepared?4u:0u)|(h.adapter_started?8u:0u)|
                             (h.route_a_enabled?16u:0u)|(h.route_a_started?32u:0u);
    *status={sizeof(*status),1,factories.load(std::memory_order_relaxed),loaded.load(std::memory_order_acquire)?1u:0u,host_bits};
    return 1;
}
// A54: the DXGI proxy is the host bootstrap for games without Streamline
// (Anno 1800; A61: also DLSS games that load nvngx_dlss.dll directly, with
// [route_a] enabled=1 and [host] tag=ROUTE A in the ini). DllMain only spawns
// the init thread when NeuralAliasHost.ini beside the DLL enables the host;
// otherwise this stays a pure forwarder.
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        na::host::StartFromIni(instance,L"NeuralAliasHost.ini","ROUTE B");
    }
    return TRUE;
}
