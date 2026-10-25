// A9 read-only consumer, linked via the existing game_adapter.h extension.
// No renderer replacement, game-specific resource constants or host ABI changes.
#include <Windows.h>
#include "host_dx12/host_boot/game_adapter.h"
#include "host_dx12/observe/producers.hpp"
#include "capture_validity.hpp"
#include "sample_acquisition.hpp"
#include "resource_formats.hpp"
#include "submission_evidence.hpp"
#include "providers/classifier/signal_candidates.hpp"
#include "providers/camera/matrix_candidates.hpp"
#include <cstdio>
#include <share.h>
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
    na::discovery::SubmissionEvidence execution;
    std::map<uint64_t,Resource> resources;
    std::map<uint64_t,Record> records;
    struct Retry {uint64_t due{};unsigned failures{};};
    std::map<std::pair<uint64_t,uint64_t>,Retry> retry;
    std::map<std::pair<uint64_t,uint64_t>,uint64_t> scanned;
    std::map<std::pair<uint64_t,uint64_t>,na::discovery::SamplePlan> camera_plans;
    uint64_t scan_window{},scan_count{},matched_samples{},attempts{},failed_samples{};
    FILE* log{};
    std::wstring evidence_dir;
    uint64_t saved_bytes{};
    uint64_t session{},epoch{},batches{},events{},malformed{},submissions{},presents{},snapshots{},matrices{},samples{},last_log{},last_sample{};
    bool overflow{}; unsigned reports{};
    uint64_t draws{}, binds{}, unresolved_views{}, captured_snapshots{}, truncated_snapshots{}, unmapped_snapshots{}, gpu_snapshots{}, matrix_scans{};
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
    return na::discovery::ResourceFormat(f);
}
template<class T> T Read(const unsigned char* p) { T value{};std::memcpy(&value,p,sizeof(value));return value; }
void JsonStrings(FILE* out,const std::vector<std::string>& strings) {
    std::fputc('[',out);
    bool first=true;
    for(const auto& text:strings) {
        if(!first) std::fputc(',',out);first=false;
        std::fputc('"',out);
        for(unsigned char ch:text) {
            if(ch=='"' || ch=='\\') {std::fputc('\\',out);std::fputc(ch,out);}
            else if(ch<32) std::fprintf(out,"\\u%04x",static_cast<unsigned>(ch));
            else std::fputc(ch,out);
        }
        std::fputc('"',out);
    }
    std::fputc(']',out);
}
void Report(Consumer& c,const na_observe_status& status,const char* phase) {
    if(!c.log) return;
    ++c.reports;
    const auto health=na::observe::GetProducerStats();
    std::fprintf(c.log,"{\"type\":\"capture_health\",\"draws\":%llu,\"binds\":%llu,\"unresolved_views\":%llu,\"captured_snapshots\":%llu,\"truncated_snapshots\":%llu,\"unmapped_snapshots\":%llu,\"gpu_snapshots\":%llu,\"sample_missing_recording\":%llu,\"sample_partial_coverage\":%llu,\"sample_partial_states\":%llu,\"sample_no_transition\":%llu}\n",
        c.draws,c.binds,c.unresolved_views,c.captured_snapshots,c.truncated_snapshots,c.unmapped_snapshots,c.gpu_snapshots,
        health.sample_missing_recording,health.sample_partial_coverage,health.sample_partial_states,health.sample_no_transition);
    std::fprintf(c.log,"{\"type\":\"summary\",\"phase\":\"%s\",\"read_only\":true,\"session\":%llu,\"epoch\":%llu,\"batches\":%llu,\"events\":%llu,\"submissions\":%llu,\"presents\":%llu,\"resources\":%zu,\"snapshots\":%llu,\"matrix_hypotheses\":%llu,\"ready_samples\":%llu,\"malformed\":%llu,\"inventory_consistent\":%s,\"capture_contiguous\":%s,\"overflow\":%s,\"foundational_dropped\":%llu,\"detail_dropped\":%llu,\"confirmed_inputs\":0,\"unknowns\":[\"camera execution-time association\",\"preserved pre-HUD composition lineage\",\"motion semantic confirmation\"]}\n",
        phase,status.session,status.epoch,c.batches,c.events,c.submissions,c.presents,c.resources.size(),c.snapshots,c.matrices,c.samples,c.malformed,
        c.validity.InventoryConsistent()?"true":"false",c.validity.CaptureContiguous()?"true":"false",c.overflow?"true":"false",status.foundational_dropped,status.detail_dropped);
    unsigned emitted=0;
    for(const auto& pair:c.execution.Latest()) {
        const auto& r=pair.second;
        auto window=r.window;
        window.loss_free=window.loss_free && c.validity.InventoryConsistent() && !c.overflow;
        const auto& observation=r.observation;
        for(auto signal:{na::classifier::Signal::Depth,na::classifier::Signal::SceneColor}) {
            const auto result=na::classifier::Describe(signal,window,observation);
            if((signal==na::classifier::Signal::Depth && !observation.depth_write) ||
                (signal==na::classifier::Signal::SceneColor && !observation.color_write)) continue;
            std::fprintf(c.log,"{\"type\":\"resource_candidate\",\"signal\":\"%s\",\"resource\":%llu,\"format\":%u,\"width\":%u,\"height\":%u,\"queue\":%llu,\"ticket\":%llu,\"list\":%llu,\"generation\":%llu,\"first_write\":%llu,\"last_write\":%llu,\"content_version\":%llu,\"boundary\":\"recording_end\",\"hud_qualified\":false,\"confidence\":\"%s\",\"acquisition_score\":%u,\"reasons\":",
                signal==na::classifier::Signal::Depth?"depth":"scene_color",observation.resource,r.format,observation.width,observation.height,
                window.scope.queue,window.scope.submission,window.scope.list,window.scope.recording,observation.first_write,observation.last_write,observation.content_version,
                result.confidence==na::classifier::Confidence::Candidate?"candidate":"unavailable",result.acquisition_score);
            JsonStrings(c.log,result.reasons);std::fputs(",\"acquisition_tasks\":",c.log);
            JsonStrings(c.log,result.acquisition_tasks);std::fputs("}\n",c.log);
        }
        if(++emitted==24) break;
    }
    std::fprintf(c.log,"{\"type\":\"acquisition_progress\",\"attempts\":%llu,\"pending\":%zu,\"failures\":%llu,\"matched_texture_samples\":%llu,\"executed_resources\":%zu,\"camera_ranges\":%zu}\n",
        c.attempts,c.acquisition.Pending(),c.failed_samples,c.matched_samples,c.execution.Latest().size(),c.camera_plans.size());
    const auto& evidence=c.execution.Stats();
    std::fprintf(c.log,"{\"type\":\"execution_reducer\",\"executions\":%llu,\"resource_versions\":%llu,\"rejected\":%llu,\"evicted_pending\":%llu,\"sequence_gaps\":%llu,\"unknown_pipeline\":%llu,\"unknown_view\":%llu}\n",
        evidence.executions,evidence.accepted,evidence.rejected,evidence.evicted,evidence.sequence_gaps,evidence.unknown_pipeline,evidence.unknown_view);
    std::fflush(c.log);
}
void __cdecl Sink(const na_event_batch* batch,void* user) {
    auto& c=*static_cast<Consumer*>(user);
    try {
        na_observe_status status{};status.size_bytes=sizeof(status);
        if(na_observe_query_status(&status)!=NA_OK) return;
        const size_t bytes=size_t(batch->header_bytes)+batch->payload_bytes;
        const auto decoded=na::discovery::DecodeBatch(batch,bytes);
        c.validity.Consume(decoded,status);
        c.execution.Consume(decoded,status);
        ++c.batches;
        if(!decoded.valid) {++c.malformed;return;}
        if(batch->session!=status.session || batch->epoch!=status.epoch) return;
        if(c.session!=status.session || c.epoch!=status.epoch) {
            c.resources.clear();c.records.clear();c.retry.clear();c.scanned.clear();c.camera_plans.clear();
            c.session=status.session;c.epoch=status.epoch;c.acquisition.Scope(c.session,c.epoch);
        }
        const uint64_t now=GetTickCount64();
        if(now-c.scan_window>=100) {c.scan_window=now;c.scan_count=0;}
        for(const auto& item:decoded.events) {
            ++c.events;const auto* p=decoded.bytes.data()+item.offset;
            switch(item.header.kind) {
            case NA_EV_RESOURCE_CREATED: {
                const auto e=Read<na_ev_resource_created>(p);
                if(c.resources.size()<32768 || c.resources.count(e.resource)) c.resources[e.resource].info=e;
                else c.overflow=true;
                break;
            }
            case NA_EV_OBJECT_RETIRED: {
                const auto e=Read<na_ev_object_retired>(p);c.resources.erase(e.object);c.records.erase(e.object);
                for(auto it=c.retry.begin();it!=c.retry.end();) it=it->first.first==e.object?c.retry.erase(it):std::next(it);
                for(auto it=c.scanned.begin();it!=c.scanned.end();) it=it->first.first==e.object?c.scanned.erase(it):std::next(it);
                for(auto it=c.camera_plans.begin();it!=c.camera_plans.end();) it=it->first.first==e.object?c.camera_plans.erase(it):std::next(it);
                break;
            }
            case NA_EV_PRESENT: ++c.presents;break;
            case NA_EV_QUEUE_EXECUTE: ++c.submissions;break;
            case NA_EV_LIST_OP: {
                const auto e=Read<na_ev_list_op>(p);
                if(c.records.size()>=4096 && !c.records.count(e.list)) {c.overflow=true;break;}
                auto& rec=c.records[e.list];
                if(rec.generation!=e.recording_generation) {rec={};rec.generation=e.recording_generation;}
                rec.incomplete=rec.incomplete || (e.hdr.flags&(NA_EVF_METHOD_UNOBSERVED|NA_EVF_TRUNCATED|NA_EVF_ORDER_AMBIGUOUS));
                if(e.op==NA_OP_OM_SET_RENDER_TARGETS && !(e.hdr.flags&NA_EVF_TRUNCATED)) {
                    ++c.binds;
                    rec.targets.clear();
                    const uint64_t count=e.a[0]+e.a[1];
                    for(uint64_t k=0;k<count;++k) {
                        auto view=Read<na_view_ref>(p+e.payload_offset+k*e.payload_stride);
                        if(!view.resource || view.resource==NA_UNKNOWN_U64) ++c.unresolved_views;
                        rec.targets.push_back(view);
                    }
                } else if(e.op==NA_OP_DRAW || e.op==NA_OP_DRAW_INDEXED) {
                    ++c.draws;
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
                c.captured_snapshots+=e.status==NA_SNAP_CAPTURED;
                c.truncated_snapshots+=(e.hdr.flags&NA_EVF_TRUNCATED)!=0;
                c.unmapped_snapshots+=e.status==NA_SNAP_NOT_MAPPED;
                c.gpu_snapshots+=e.status==NA_SNAP_GPU_WRITTEN;
                // Bounded structural hypotheses only; APP_SYNC_UNKNOWN is never upgraded.
                // Count captured records, not all statuses: a repeating stream
                // of unmapped bindings must not phase-lock every scan away
                // from the few usable snapshots.
                const auto key=std::make_pair(e.resource,e.offset);
                const auto prior=c.scanned.find(key);
                if(e.status==NA_SNAP_CAPTURED && e.captured_bytes>=64 && e.captured_bytes<=4096 && c.scan_count<4 &&
                    (prior==c.scanned.end() || now-prior->second>=2000)) {
                    if(c.scanned.size()==4096 && prior==c.scanned.end()) c.scanned.erase(c.scanned.begin());
                    c.scanned[key]=now;++c.scan_count;
                    na::camera::Snapshot s{status.session,status.epoch,e.queue_ticket,e.resource,e.snapshot_id,e.offset,
                        na::camera::Sync::AppSyncUnknown,(e.hdr.flags&(NA_EVF_TRUNCATED|NA_EVF_METHOD_UNOBSERVED))==0,true,{}};
                    s.bytes.assign(p+e.payload_offset,p+e.payload_offset+e.captured_bytes);
                    const auto scan=na::camera::FindMatrices(s,true);++c.matrix_scans;
                    c.matrices+=scan.candidates.size();
                    if(!scan.candidates.empty() && c.camera_plans.size()<256)
                        c.camera_plans[key]={e.resource,0,e.captured_bytes,1,0,true,e.offset};
                    std::fprintf(c.log,"{\"type\":\"matrix_scan\",\"resource\":%llu,\"offset\":%llu,\"queue\":%llu,\"ticket\":%llu,\"snapshot\":%llu,\"bytes\":%u,\"flags\":%u,\"hypotheses\":%zu,\"unavailable\":%s,\"partial\":%s,\"execution_qualified\":false}\n",
                        e.resource,e.offset,e.queue,e.queue_ticket,e.snapshot_id,e.captured_bytes,e.hdr.flags,scan.candidates.size(),scan.unavailable?"true":"false",scan.partial_capture?"true":"false");
                }
                break;
            }
            default:break;
            }
        }
        if(now-c.last_sample>=500) {
            c.last_sample=now;
            std::vector<na::discovery::SamplePlan> plans;
            for(const auto& pair:c.camera_plans) plans.push_back(pair.second);
            for(const auto& pair:c.resources) {
                const auto& r=pair.second;
                if((!r.depth_writes && !r.color_writes) || r.info.resource_flags==NA_UNKNOWN_U64 || !(r.info.resource_flags&3) || r.info.width<128 || r.info.height<128 ||
                    r.info.width>4096 || r.info.height>4096 || r.info.sample_count!=1 || Format(r.info.format)==na::classifier::Format::Unknown) continue;
                plans.push_back({pair.first,0,static_cast<uint32_t>(r.info.width),r.info.height,r.info.format});
            }
            // Oldest due first: persistent failures cannot monopolize slots or
            // blacklist future resources after a lifetime attempt cap.
            std::stable_sort(plans.begin(),plans.end(),[&](const auto& a,const auto& b) {
                const auto x=c.retry.find({a.resource,a.offset}),y=c.retry.find({b.resource,b.offset});
                return (x==c.retry.end()?0:x->second.due)<(y==c.retry.end()?0:y->second.due);
            });
            for(const auto& plan:plans) {
                if(c.acquisition.Pending()==4) break;
                auto& retry=c.retry[{plan.resource,plan.offset}];if(now<retry.due) continue;
                ++c.attempts;
                if(c.acquisition.Begin(plan)) retry.due=now+30000;
                else {retry.failures=std::min(retry.failures+1,5u);retry.due=now+(1000ull<<retry.failures);}
            }
        }
        for(const auto& sample:c.acquisition.Pump()) {
            if(sample.status==na::discovery::AcquisitionStatus::Ready) ++c.samples;
            else ++c.failed_samples;
            auto& retry=c.retry[{sample.plan.resource,sample.plan.offset}];
            if(sample.status==na::discovery::AcquisitionStatus::Ready) {retry.failures=0;retry.due=now+30000;}
            else {retry.failures=std::min(retry.failures+1,5u);retry.due=now+(1000ull<<retry.failures);}
            const auto* matched=c.execution.Match(sample.info.queue,sample.info.queue_ticket,sample.info.resource,sample.info.subresource);
            if(matched && sample.status==na::discovery::AcquisitionStatus::Ready) ++c.matched_samples;
            if(sample.plan.buffer && sample.status==na::discovery::AcquisitionStatus::Ready) {
                na::camera::Snapshot s{status.session,status.epoch,sample.info.queue_ticket,sample.info.resource,sample.info.queue_ticket,sample.plan.offset,
                    na::camera::Sync::IndependentlyVerified,true,true,sample.bytes};
                const auto scan=na::camera::FindMatrices(s);
                for(const auto& candidate:scan.candidates) {
                    std::fprintf(c.log,"{\"type\":\"gpu_camera_hypothesis\",\"resource\":%llu,\"offset\":%llu,\"queue\":%llu,\"ticket\":%llu,\"kind\":%u,\"layout\":%u,\"near\":%.9g,\"far\":%.9g,\"main_camera_qualified\":false,\"matrix\":[",
                        sample.info.resource,candidate.byte_offset,sample.info.queue,sample.info.queue_ticket,static_cast<unsigned>(candidate.kind),static_cast<unsigned>(candidate.layout),
                        candidate.near_plane,std::isfinite(candidate.far_plane)?candidate.far_plane:0);
                    for(size_t k=0;k<16;++k) std::fprintf(c.log,"%s%.12g",k?",":"",candidate.matrix[k]);
                    std::fputs("]}\n",c.log);
                }
            }
            char evidence_file[128]{};
            if(!c.evidence_dir.empty() && sample.status==na::discovery::AcquisitionStatus::Ready &&
                sample.bytes.size()<=(128ull<<20)-c.saved_bytes) {
                sprintf_s(evidence_file,"sample-%llu-%llu-%llu-%llu.bin",sample.info.session,sample.info.queue,sample.info.queue_ticket,sample.info.resource);
                const std::string name=evidence_file;const auto path=c.evidence_dir+L"\\"+std::wstring(name.begin(),name.end());
                FILE* file=GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES?_wfsopen(path.c_str(),L"wb",_SH_DENYWR):nullptr;
                if(file) {
                    const auto written=std::fwrite(sample.bytes.data(),1,sample.bytes.size(),file);std::fclose(file);
                    c.saved_bytes+=written;
                    if(written!=sample.bytes.size()) {DeleteFileW(path.c_str());evidence_file[0]=0;}
                } else evidence_file[0]=0;
            }
            std::vector<double> values;const bool scalar=na::discovery::DecodeScalarDepth(sample,values);
            double low=0,high=0;if(scalar && !values.empty()) {const auto mm=std::minmax_element(values.begin(),values.end());low=*mm.first;high=*mm.second;}
            // Scalar range is a diagnostic statistic, not a camera-qualified depth verdict.
            std::fprintf(c.log,"{\"type\":\"sample\",\"resource\":%llu,\"queue\":%llu,\"ticket\":%llu,\"status\":%u,\"host_status\":%u,\"reason\":%u,\"format\":%u,\"bytes\":%llu,\"scalar_decoded\":%s,\"min\":%.9g,\"max\":%.9g,\"matched_execution\":%s,\"list\":%llu,\"generation\":%llu,\"width\":%u,\"height\":%u,\"row_pitch\":%u,\"evidence_file\":\"%s\"}\n",
                sample.info.resource,sample.info.queue,sample.info.queue_ticket,static_cast<unsigned>(sample.status),sample.info.status,sample.info.reason,sample.info.format,sample.info.data_bytes,
                scalar?"true":"false",std::isfinite(low)?low:0,std::isfinite(high)?high:0,
                matched?"true":"false",matched?matched->window.scope.list:0,matched?matched->window.scope.recording:0,
                sample.info.width,sample.info.height,sample.info.row_pitch,evidence_file);
        }
        if(now-c.last_log>=(c.reports<180?2000u:10000u) || (batch->batch_flags&NA_BATCHF_FINAL)) {
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
    wchar_t evidence_dir[MAX_PATH]{};GetPrivateProfileStringW(L"discovery",L"evidence_dir",L"",evidence_dir,MAX_PATH,ini_path);
    c->evidence_dir=evidence_dir; // optional pre-created local directory; at most 128 MiB per session
    c->log=_wfsopen(path,L"w",_SH_DENYWR); // Permit live readers; refuse a second writer.
    if(!c->log) {delete c;return 0;}
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
