// Test-only activation/transport integration. Never uses a game installation.
#include "../host/test_support.hpp"
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/host_observe_internal.hpp"
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
    auto candidate=test::CreateTexture(d.device.Get(),128,128,DXGI_FORMAT_D32_FLOAT,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(candidate);
    // Mixed-status fixture through the real transport: 32 unavailable records
    // followed by a captured matrix must not suppress all structural scans.
    struct SnapshotFixture { na_ev_snapshot event; float matrix[16]; } fixture{};
    fixture.event.hdr.kind=NA_EV_SNAPSHOT;
    fixture.event.hdr.size_bytes=sizeof(na_ev_snapshot);
    fixture.event.resource=na::observe::IdOf(resource.Get());
    fixture.event.queue=na::observe::IdOf(d.queue.Get());
    fixture.event.queue_ticket=1;fixture.event.requested_bytes=64;
    fixture.event.sync=NA_SNAP_SYNC_APP_UNKNOWN;
    fixture.event.status=NA_SNAP_NOT_MAPPED;
    for(unsigned i=1;i<=32;++i){fixture.event.snapshot_id=i;REQUIRE(na::observe_internal::Emit(&fixture,NA_GROUP_SNAPSHOT));}
    fixture.event.snapshot_id=33;fixture.event.status=NA_SNAP_CAPTURED;
    fixture.event.captured_bytes=64;fixture.event.payload_offset=offsetof(SnapshotFixture,matrix);
    fixture.event.hdr.size_bytes=sizeof(fixture);
    fixture.matrix[0]=fixture.matrix[5]=fixture.matrix[10]=fixture.matrix[15]=1;
    REQUIRE(na::observe_internal::Emit(&fixture,NA_GROUP_SNAPSHOT));
    // Stop drains the real transport, including its final batch, before freeing context.
    na_adapter_stop();na_adapter_stop();
    std::ifstream file(log);const std::string text((std::istreambuf_iterator<char>(file)),{});
    CHECK(text.find("\"read_only\":true")!=std::string::npos);
    CHECK(text.find("\"phase\":\"stopped\"")!=std::string::npos);
    CHECK(text.find("\"malformed\":0")!=std::string::npos);
    CHECK(text.find("\"confirmed_inputs\":0")!=std::string::npos);
    CHECK(text.find("\"hypotheses\":2")!=std::string::npos);
    CHECK(text.find("\"execution_qualified\":false")!=std::string::npos);
    CHECK(text.find("\"signal\":\"depth\"")!=std::string::npos);
    CHECK(text.find("\"signal\":\"scene_color\"")!=std::string::npos);
    CHECK(text.find("Missing or mismatched session/epoch/submission/queue/recording identity")!=std::string::npos);
    CHECK(text.find("No intact content version at scene boundary; acquire earlier snapshot")!=std::string::npos);
    CHECK(text.find("\"confidence\":\"candidate\"")==std::string::npos);
    return test::finish("diagnostic_adapter");
}
