// Worker-only asynchronous acquisition. No waiting, GPU mapping or frame guesses.
#pragma once
#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl
#define NA_ACQUIRE_UNDEFINE_CDECL
#endif
#include "host_dx12/host_observe_samples.h"
#ifdef NA_ACQUIRE_UNDEFINE_CDECL
#undef __cdecl
#undef NA_ACQUIRE_UNDEFINE_CDECL
#endif
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <vector>

namespace na::discovery {
struct SampleApi {
    virtual ~SampleApi() = default;
    virtual int Request(const na_sample_request&, uint64_t&) = 0;
    virtual int Poll(uint64_t, na_sample_info&) = 0;
    virtual int Read(uint64_t, void*, uint64_t, uint64_t&) = 0;
    virtual void Release(uint64_t) noexcept = 0;
};
// Instantiate only in a host-linked worker, never in a borrowed event callback.
struct HostSampleApi final : SampleApi {
    int Request(const na_sample_request& r,uint64_t& t) override { return na_observe_sample_request(&r,&t); }
    int Poll(uint64_t t,na_sample_info& i) override { return na_observe_sample_poll(t,&i); }
    int Read(uint64_t t,void* p,uint64_t n,uint64_t& w) override { return na_observe_sample_read(t,p,n,&w); }
    void Release(uint64_t t) noexcept override { (void)na_observe_sample_release(t); }
};
struct SamplePlan {
    uint64_t resource{};
    uint32_t subresource{}, width{}, height{}, format{};
};
enum class AcquisitionStatus { Ready, HostFailure, ScopeMismatch, ShapeMismatch, ReadFailure };
struct AcquiredSample {
    AcquisitionStatus status{AcquisitionStatus::HostFailure};
    na_sample_info info{}; // Actual completion identity; never request-time identity.
    std::vector<uint8_t> bytes;
};
inline bool SampleId(uint64_t id) { return id && id != UINT64_MAX; }
// Packed depth plane 0 is returned with an explicit interpretation by the host.
// Do not accept arbitrary typeless payloads based on their byte size.
inline uint32_t SampleFormat(uint32_t resource_format) {
    if(resource_format==19 || resource_format==20) return 41; // D32S8 family -> R32_FLOAT
    if(resource_format==44 || resource_format==45) return 46; // D24S8 family -> R24_UNORM_X8_TYPELESS
    return resource_format;
}

// Single worker owns this object and its backend; backend must outlive it.
// At most four whole-subresource leases, each at most 64 MiB. Consumers own
// output bytes after host release. Stop/resync cancels leases before accepting
// a new scope. The event consumer supplies scope; no Present-based inference.
class SampleAcquisition {
    struct Lease { uint64_t token; SamplePlan plan; };
    SampleApi& api_;
    uint64_t session_{}, epoch_{};
    std::vector<Lease> leases_;
    static constexpr uint64_t max_bytes = 64ull << 20;
public:
    explicit SampleAcquisition(SampleApi& api) : api_(api) { leases_.reserve(4); }
    SampleAcquisition(const SampleAcquisition&) = delete;
    SampleAcquisition& operator=(const SampleAcquisition&) = delete;
    ~SampleAcquisition() { Clear(); }
    void Clear() noexcept {
        for (const auto& l:leases_) api_.Release(l.token);
        leases_.clear(); session_=epoch_=0;
    }
    void Scope(uint64_t session,uint64_t epoch) {
        if (session==session_ && epoch==epoch_) return;
        Clear();
        if (SampleId(session) && SampleId(epoch)) { session_=session; epoch_=epoch; }
    }
    size_t Pending() const { return leases_.size(); }
    bool Begin(const SamplePlan& p) {
        if (!session_ || !SampleId(p.resource) || !p.width || !p.height ||
            p.width>4096 || p.height>4096 || !p.format || leases_.size()==4) return false;
        for (const auto& l:leases_) if (l.plan.resource==p.resource && l.plan.subresource==p.subresource) return false;
        na_sample_request r{}; r.size_bytes=sizeof(r); r.resource=p.resource; r.subresource=p.subresource;
        r.arm_timeout_ms=1000; r.expiry_ms=2000; // whole subresource, including depth restrictions
        uint64_t token=0;
        if (api_.Request(r,token)!=NA_OK || !SampleId(token)) { if(token) api_.Release(token); return false; }
        leases_.push_back({token,p}); return true;
    }
    std::vector<AcquiredSample> Pump() {
        std::vector<AcquiredSample> out; out.reserve(leases_.size());
        for (auto it=leases_.begin();it!=leases_.end();) {
            AcquiredSample s{}; s.info.size_bytes=sizeof(s.info);
            const int rc=api_.Poll(it->token,s.info);
            if (rc==NA_OK && s.info.session==session_ && s.info.epoch==epoch_ &&
                (s.info.status==NA_SAMPLE_ARMED || s.info.status==NA_SAMPLE_IN_FLIGHT)) { ++it; continue; }
            if (rc!=NA_OK || s.info.status!=NA_SAMPLE_READY) s.status=AcquisitionStatus::HostFailure;
            else if (s.info.session!=session_ || s.info.epoch!=epoch_) s.status=AcquisitionStatus::ScopeMismatch;
            else if (s.info.resource!=it->plan.resource || s.info.subresource!=it->plan.subresource ||
                s.info.width!=it->plan.width || s.info.height!=it->plan.height || s.info.format!=SampleFormat(it->plan.format) ||
                s.info.x || s.info.y || !SampleId(s.info.queue) || !SampleId(s.info.queue_ticket) ||
                !s.info.row_pitch || !s.info.data_bytes || s.info.data_bytes>max_bytes) s.status=AcquisitionStatus::ShapeMismatch;
            else {
                s.bytes.resize(static_cast<size_t>(s.info.data_bytes));
                uint64_t written=0;
                if (api_.Read(it->token,s.bytes.data(),s.bytes.size(),written)==NA_OK && written==s.info.data_bytes)
                    s.status=AcquisitionStatus::Ready;
                else { s.status=AcquisitionStatus::ReadFailure; s.bytes.clear(); }
            }
            api_.Release(it->token); it=leases_.erase(it); out.push_back(std::move(s));
        }
        return out;
    }
};

// Scalar float or explicitly identified 24-bit depth plane; never infer a
// typeless/packed interpretation from byte count.
inline bool DecodeScalarDepth(const AcquiredSample& s,std::vector<double>& depth) {
    depth.clear();
    const auto& i=s.info;
    const uint32_t pixel_bytes=(i.format==54 || i.format==55 || i.format==56) ? 2u : 4u;
    if (s.status!=AcquisitionStatus::Ready || i.status!=NA_SAMPLE_READY || i.data_bytes!=s.bytes.size() ||
        (i.format!=40 && i.format!=41 && i.format!=46 && i.format!=54 && i.format!=55 && i.format!=56) ||
        !i.width || !i.height || i.width>4096 || i.height>4096 ||
        uint64_t(i.width)*pixel_bytes>i.row_pitch ||
        uint64_t(i.row_pitch)*(i.height-1)+uint64_t(i.width)*pixel_bytes>s.bytes.size()) return false;
    depth.reserve(size_t(i.width)*i.height);
    for(uint32_t y=0;y<i.height;++y) for(uint32_t x=0;x<i.width;++x) {
        const auto* pixel=s.bytes.data()+size_t(y)*i.row_pitch+size_t(x)*pixel_bytes;
        if(pixel_bytes==2) {
            uint16_t raw{};std::memcpy(&raw,pixel,2);
            if(i.format!=54) depth.push_back(double(raw)/65535.0);
            else {
                const unsigned exponent=(raw>>10)&31u, mantissa=raw&1023u;
                double value=exponent==0 ? std::ldexp(double(mantissa),-24) :
                    exponent==31 ? (mantissa ? std::numeric_limits<double>::quiet_NaN() :
                        std::numeric_limits<double>::infinity()) : std::ldexp(double(1024u+mantissa),int(exponent)-25);
                depth.push_back((raw&0x8000u) ? -value : value);
            }
        } else if(i.format==46) {
            uint32_t raw{};std::memcpy(&raw,pixel,4);
            depth.push_back(double(raw & 0xFFFFFFu)/double(0xFFFFFFu));
        } else {
            float value{};std::memcpy(&value,pixel,4);depth.push_back(value);
        }
    }
    return true; // Numeric validity is assessed by the evidence check, not hidden.
}
} // namespace na::discovery
