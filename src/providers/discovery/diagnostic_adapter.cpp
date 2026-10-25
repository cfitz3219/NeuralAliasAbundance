// A9 read-only consumer, linked via the existing game_adapter.h extension.
// No renderer replacement, game-specific resource constants or host ABI changes.
#include <Windows.h>
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/observe/producers.hpp"
#include "capture_validity.hpp"
#include "sample_acquisition.hpp"
#include "providers/classifier/signal_candidates.hpp"
#include "providers/camera/matrix_candidates.hpp"
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace {
struct Resource { na_ev_resource_created info{};uint64_t depth_writes{},color_writes{}; };
struct Record {
    uint64_t generation{};
    std::vector<na_view_ref> targets;
    bool incomplete{};
};
struct Consumer {
    na::discovery::CaptureValidity validity;
    na::discovery::HostSampleApi api;
    na::discovery::SampleAcquisition acquisition{api};
    std::map<uint64_t,Resource> resources;
    std::map<uint64_t,Record> records;
    std::set<uint64_t> sampled;
    FILE* log{};
    uint64_t session{},epoch{},batches{},events{},malformed{},submissions{},presents{},snapshots{},matrices{},samples{},last_log{},last_sample{};
    bool overflow{}; unsigned reports{};
};
// The host owns process lifetime. No destructors race its callback at exit.
Consumer*& consumer() { static auto* slot=new Consumer*(nullptr);return *slot; }
wchar_t ini_path[MAX_PATH]{};

void PathFromModule() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PathFromModule),&module);
    GetModuleFileNameW(module,ini_path,MAX_PATH);
    for(int i=lstrlenW(ini_path)-1;i>=0;--i) if(ini_path[i]==L'\\' || ini_path[i]==L'/') {ini_path[i+1]=0;break;}
    wcscat_s(ini_path,L"NeuralAliasHost.ini");
}
na::classifier::Format Format(uint32_t f) {
    if(f==40 || f==45 || f==55 || f==20) return na::classifier::Format::Depth;
    if(f==39 || f==41 || f==53 || f==54 || f==56) return na::classifier::Format::ScalarFloat;
    if(f==2 || f==10) return na::classifier::Format::ColorFloat;
    if(f==28 || f==29 || f==87 || f==91) return na::classifier::Format::ColorUnorm;
    if(f==16 || f==34) return na::classifier::Format::TwoChannelFloat;
    return na::classifier::Format::Unknown;
}
template<class T> T Read(const unsigned char* p) { T value{};std::memcpy(&value,p,sizeof(value));return value; }
void Report(Consumer& c,const na_observe_status& status,const char* phase) {
    if(!c.log || c.reports>=180) return;
    ++c.reports;
    std::fprintf(c.log,"{\"type\":\"summary\",\"phase\":\"%s\",\"read_only\":true,\"session\":%llu,\"epoch\":%llu,\"batches\":%llu,\"events\":%llu,\"submissions\":%llu,\"presents\":%llu,\"resources\":%zu,\"snapshots\":%llu,\"matrix_hypotheses\":%llu,\"ready_samples\":%llu,\"malformed\":%llu,\"inventory_consistent\":%s,\"capture_contiguous\":%s,\"overflow\":%s,\"foundational_dropped\":%llu,\"detail_dropped\":%llu,\"confirmed_inputs\":0,\"unknowns\":[\"camera execution-time association\",\"preserved pre-HUD composition lineage\",\"motion semantic confirmation\"]}\n",
        phase,status.session,status.epoch,c.batches,c.events,c.submissions,c.presents,c.resources.size(),c.snapshots,c.matrices,c.samples,c.malformed,
        c.validity.InventoryConsistent()?"true":"false",c.validity.CaptureContiguous()?"true":"false",c.overflow?"true":"false",status.foundational_dropped,status.detail_dropped);
    unsigned emitted=0;
    for(const auto& pair:c.resources) {
        const auto& r=pair.second;
        if((!r.depth_writes && !r.color_writes) || r.info.width<128 || r.info.height<128) continue;
        std::fprintf(c.log,"{\"type\":\"resource_candidate\",\"resource\":%llu,\"format\":%u,\"width\":%llu,\"height\":%u,\"depth_writes\":%llu,\"color_writes\":%llu,\"confidence\":\"unconfirmed_candidate\",\"reason\":\"observed render-target/depth use; missing independent semantic association\"}\n",
            pair.first,r.info.format,r.info.width,r.info.height,r.depth_writes,r.color_writes);
        if(++emitted==24) break;
    }
    std::fflush(c.log);
}
void __cdecl Sink(const na_event_batch* batch,void* user) {
    auto& c=*static_cast<Consumer*>(user);
    try {
        na_observe_status status{};status.size_bytes=sizeof(status);
        if(na_observe_query_status(&status)!=NA_OK) return;
        const size_t bytes=size_t(batch->header_bytes)+batch->payload_bytes;
        const auto decoded=na::discovery::DecodeBatch(batch,bytes);
        c.validity.Consume(batch,bytes,status);
        ++c.batches;
        if(!decoded.valid) {++c.malformed;return;}
        if(batch->session!=status.session || batch->epoch!=status.epoch) return;
        if(c.session!=status.session || c.epoch!=status.epoch) {
            c.resources.clear();c.records.clear();c.sampled.clear();
            c.session=status.session;c.epoch=status.epoch;c.acquisition.Scope(c.session,c.epoch);
        }
        for(const auto& item:decoded.events) {
            ++c.events;const auto* p=decoded.bytes.data()+item.offset;
            switch(item.header.kind) {
            case NA_EV_RESOURCE_CREATED: {
                const auto e=Read<na_ev_resource_created>(p);
                if(c.resources.size()<32768 || c.resources.count(e.resource)) c.resources[e.resource].info=e;
                else c.overflow=true;
                break;
            }
            case NA_EV_OBJECT_RETIRED: {const auto e=Read<na_ev_object_retired>(p);c.resources.erase(e.object);c.records.erase(e.object);break;}
            case NA_EV_PRESENT: ++c.presents;break;
            case NA_EV_QUEUE_EXECUTE: ++c.submissions;break;
            case NA_EV_LIST_OP: {
                const auto e=Read<na_ev_list_op>(p);
                if(c.records.size()>=4096 && !c.records.count(e.list)) {c.overflow=true;break;}
                auto& rec=c.records[e.list];
                if(rec.generation!=e.recording_generation) {rec={};rec.generation=e.recording_generation;}
                rec.incomplete=rec.incomplete || (e.hdr.flags&(NA_EVF_METHOD_UNOBSERVED|NA_EVF_TRUNCATED|NA_EVF_ORDER_AMBIGUOUS));
                if(e.op==NA_OP_OM_SET_RENDER_TARGETS && !(e.hdr.flags&NA_EVF_TRUNCATED)) {
                    rec.targets.clear();
                    const uint64_t count=e.a[0]+e.a[1];
                    for(uint64_t k=0;k<count;++k) rec.targets.push_back(Read<na_view_ref>(p+e.payload_offset+k*e.payload_stride));
                } else if(e.op==NA_OP_DRAW || e.op==NA_OP_DRAW_INDEXED) {
                    for(const auto& view:rec.targets) {
                        const auto found=c.resources.find(view.resource);if(found==c.resources.end()) continue;
                        if(view.descriptor_kind==NA_DESC_DSV) ++found->second.depth_writes;
                        if(view.descriptor_kind==NA_DESC_RTV) ++found->second.color_writes;
                    }
                }
                break;
            }
            case NA_EV_SNAPSHOT: {
                ++c.snapshots;const auto e=Read<na_ev_snapshot>(p);
                // Bounded structural hypotheses only; APP_SYNC_UNKNOWN is never upgraded.
                if(e.status==NA_SNAP_CAPTURED && e.captured_bytes>=64 && e.captured_bytes<=4096 && c.snapshots%32==0) {
                    na::camera::Snapshot s{status.session,status.epoch,e.queue_ticket,e.resource,e.snapshot_id,e.offset,
                        na::camera::Sync::AppSyncUnknown,(e.hdr.flags&(NA_EVF_TRUNCATED|NA_EVF_METHOD_UNOBSERVED))==0,true,{}};
                    s.bytes.assign(p+e.payload_offset,p+e.payload_offset+e.captured_bytes);
                    c.matrices+=na::camera::FindMatrices(s).candidates.size();
                }
                break;
            }
            default:break;
            }
        }
        const uint64_t now=GetTickCount64();
        if(c.reports<180 && now-c.last_sample>=2000 && c.sampled.size()<32) {
            c.last_sample=now;
            for(const auto& pair:c.resources) {
                const auto& r=pair.second;
                if(c.sampled.count(pair.first) || (!r.depth_writes && !r.color_writes) || r.info.width<128 || r.info.height<128 ||
                    r.info.width>4096 || r.info.height>4096 || r.info.sample_count!=1 || Format(r.info.format)==na::classifier::Format::Unknown) continue;
                if(c.acquisition.Begin({pair.first,0,static_cast<uint32_t>(r.info.width),r.info.height,r.info.format})) c.sampled.insert(pair.first);
                if(c.acquisition.Pending()==4) break;
            }
        }
        for(const auto& sample:c.acquisition.Pump()) {
            if(sample.status==na::discovery::AcquisitionStatus::Ready) ++c.samples;
            std::vector<double> values;const bool scalar=na::discovery::DecodeScalarDepth(sample,values);
            double low=0,high=0;if(scalar && !values.empty()) {const auto mm=std::minmax_element(values.begin(),values.end());low=*mm.first;high=*mm.second;}
            // Scalar range is a diagnostic statistic, not a camera-qualified depth verdict.
            std::fprintf(c.log,"{\"type\":\"sample\",\"resource\":%llu,\"queue\":%llu,\"ticket\":%llu,\"status\":%u,\"host_status\":%u,\"reason\":%u,\"format\":%u,\"bytes\":%llu,\"scalar_decoded\":%s,\"min\":%.9g,\"max\":%.9g}\n",
                sample.info.resource,sample.info.queue,sample.info.queue_ticket,static_cast<unsigned>(sample.status),sample.info.status,sample.info.reason,sample.info.format,sample.info.data_bytes,
                scalar?"true":"false",std::isfinite(low)?low:0,std::isfinite(high)?high:0);
        }
        if(now-c.last_log>=2000 || (batch->batch_flags&NA_BATCHF_FINAL)) {
            c.last_log=now;Report(c,status,(batch->batch_flags&NA_BATCHF_FINAL)?"stopped":"observing");
        }
    } catch(...) {
        // Exceptions never escape into the transport worker; discard evidence.
        ++c.malformed;c.overflow=true;c.acquisition.Clear();
    }
}
}
extern "C" int na_adapter_prepare() {
    PathFromModule();
    return GetPrivateProfileIntW(L"discovery",L"enabled",0,ini_path)==1 &&
        GetPrivateProfileIntW(L"discovery",L"read_only",1,ini_path)==1;
}
extern "C" int na_adapter_start() {
    if(consumer() || !na_adapter_prepare()) return 0;
    wchar_t path[MAX_PATH]{};GetPrivateProfileStringW(L"discovery",L"log",L"",path,MAX_PATH,ini_path);
    if(!path[0]) return 0;
    auto* c=new Consumer;
    if(_wfopen_s(&c->log,path,L"w") || !c->log) {delete c;return 0;}
    for(unsigned n=0;n<200 && !na::observe::ProducersActive();++n) Sleep(10); // host init thread only
    if(!na::observe::ProducersActive()) {std::fputs("{\"type\":\"error\",\"reason\":\"producers unavailable\"}\n",c->log);std::fclose(c->log);delete c;return 0;}
    na_observe_config cfg{};cfg.size_bytes=sizeof(cfg);
    if(na_observe_start(&cfg,Sink,c)!=NA_OK) {std::fclose(c->log);delete c;return 0;}
    consumer()=c;return 1;
}
extern "C" void na_adapter_stop() {
    if(auto* c=consumer()) {
        na_observe_stop();c->acquisition.Clear();std::fclose(c->log);delete c;consumer()=nullptr;
    }
}
extern "C" const char* na_adapter_name() {return "Discovery diagnostics (read-only)";}
