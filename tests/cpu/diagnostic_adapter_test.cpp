// Test-only activation/transport integration. Never uses a game installation.
#include "../host/test_support.hpp"
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/observe/producers.hpp"
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    const auto dir=std::filesystem::path(exe).parent_path();
    const auto ini=dir/L"NeuralAliasHost.ini",log=dir/L"diagnostic-test.jsonl";
    // Refuse to overwrite a user's configuration beside the test executable.
    REQUIRE(!std::filesystem::exists(ini));
    CHECK(na_adapter_prepare()==0);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove(path);} } cleanup{ini};
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"enabled",L"1",ini.c_str()));
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"read_only",L"0",ini.c_str()));
    CHECK(na_adapter_prepare()==0);
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"read_only",L"1",ini.c_str()));
    REQUIRE(WritePrivateProfileStringW(L"discovery",L"log",log.c_str(),ini.c_str()));
    CHECK(na_adapter_prepare()==1);
    test::Device d;if(!test::CreateDevice(d)) return test::SkipNoDevice("diagnostic_adapter");
    REQUIRE(na::observe::InstallProducers(d.device.Get(),d.queue.Get(),true));
    REQUIRE(na_adapter_start()==1);
    { std::ifstream live(log);CHECK(live.is_open()); }
    CHECK(na_adapter_start()==0);
    auto resource=test::CreateBuffer(d.device.Get(),4096,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(resource);
    // Stop drains the real transport, including its final batch, before freeing context.
    na_adapter_stop();na_adapter_stop();
    std::ifstream file(log);const std::string text((std::istreambuf_iterator<char>(file)),{});
    CHECK(text.find("\"read_only\":true")!=std::string::npos);
    CHECK(text.find("\"phase\":\"stopped\"")!=std::string::npos);
    CHECK(text.find("\"malformed\":0")!=std::string::npos);
    CHECK(text.find("\"confirmed_inputs\":0")!=std::string::npos);
    return test::finish("diagnostic_adapter");
}
