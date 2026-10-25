// Production-obtainable temporal evidence: two completed depth versions and
// independently established cameras. No world-position or visibility oracle.
#pragma once
#include "content_checks.hpp"
#include "providers/discovery/sample_acquisition.hpp"

namespace na::classifier {
struct CameraAssociation {
    uint64_t session{},epoch{},queue{},ticket{},frame{};
    // UNJITTERED D3D [0,1] view-projection, including reverse Z. Jitter is
    // supplied separately in pixels; a captured jittered projection must be
    // unjittered using an independently established convention before use.
    Matrix view_projection{};
    double jitter_x{},jitter_y{},clear_depth{1};
    bool qualified{},complete{},reset{};
    bool view_projection_unjittered{}; // Explicit caller assertion; unknown fails closed.
};
struct DepthFrame {
    CameraAssociation camera;
    uint64_t resource{};
    uint32_t width{},height{};
    std::vector<double> depth;
};
inline bool AcquireDepthFrame(const discovery::AcquiredSample& sample,const CameraAssociation& c,DepthFrame& out) {
    out={}; const auto& i=sample.info;
    if (!c.qualified || !c.complete || !c.view_projection_unjittered || !Known(c.frame) || c.session!=i.session || c.epoch!=i.epoch ||
        c.queue!=i.queue || c.ticket!=i.queue_ticket || !Known(c.session) || !Known(c.epoch) ||
        !Known(c.queue) || !Known(c.ticket) || !Known(i.resource) ||
        !std::isfinite(c.jitter_x) || !std::isfinite(c.jitter_y) ||
        (c.clear_depth!=0 && c.clear_depth!=1)) return false;
    for(double v:c.view_projection) if(!std::isfinite(v)) return false;
    if(!discovery::DecodeScalarDepth(sample,out.depth)) return false;
    out.camera=c; out.resource=i.resource; out.width=i.width; out.height=i.height; return true;
}
inline bool Inverse(const Matrix& m,Matrix& inverse) {
    double a[4][8]{};
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) {
        if(!std::isfinite(m[r*4+c])) return false;
        a[r][c]=m[r*4+c]; a[r][c+4]=r==c?1:0;
    }
    for(size_t c=0;c<4;++c) {
        size_t pivot=c;
        for(size_t r=c+1;r<4;++r) if(std::abs(a[r][c])>std::abs(a[pivot][c])) pivot=r;
        if(std::abs(a[pivot][c])<1e-12) return false;
        for(size_t k=0;k<8;++k) std::swap(a[c][k],a[pivot][k]);
        const double divisor=a[c][c]; for(double& v:a[c]) v/=divisor;
        for(size_t r=0;r<4;++r) if(r!=c) {
            const double factor=a[r][c]; for(size_t k=0;k<8;++k) a[r][k]-=factor*a[c][k];
        }
    }
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) {
        inverse[r*4+c]=a[r][c+4]; if(!std::isfinite(inverse[r*4+c])) return false;
    }
    return true;
}
inline std::array<double,4> Transform(const Matrix& m,const std::array<double,4>& v) {
    std::array<double,4> out{};
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) out[r]+=m[r*4+c]*v[c];
    return out;
}
enum class TemporalVerdict { Unavailable, Uninformative, Contradicted, Consistent };
struct TemporalDepthReport {
    TemporalVerdict verdict{TemporalVerdict::Unavailable};
    size_t eligible{},checked{},inliers{},excluded{},excited{};
    double coverage{},inlier_fraction{},max_error{},max_pixel_shift{};
    // Agreement on a static-surface hypothesis, not a proven visibility mask.
    // Derived without any candidate motion vectors; never upgrades camera trust.
    std::vector<uint8_t> current_agreement, previous_agreement;
};
inline bool ValidDepthFrame(const DepthFrame& f) {
    const auto& c=f.camera;
    if(!c.complete || !c.qualified || !c.view_projection_unjittered || !Known(c.session) || !Known(c.epoch) || !Known(c.frame) ||
        !Known(c.queue) || !Known(c.ticket) || !Known(f.resource) || !f.width || !f.height ||
        f.width>4096 || f.height>4096 || f.depth.size()!=size_t(f.width)*f.height ||
        !std::isfinite(c.jitter_x) || !std::isfinite(c.jitter_y) || (c.clear_depth!=0 && c.clear_depth!=1)) return false;
    for(double v:c.view_projection) if(!std::isfinite(v)) return false;
    for(double d:f.depth) if(!std::isfinite(d) || d<0 || d>1) return false;
    return true;
}
inline TemporalDepthReport CheckTemporalDepth(const DepthFrame& previous,const DepthFrame& current,
    double tolerance=0.001) {
    TemporalDepthReport r{};
    const auto& p=previous.camera; const auto& c=current.camera;
    if(!ValidDepthFrame(previous) || !ValidDepthFrame(current) || c.reset ||
        p.session!=c.session || p.epoch!=c.epoch || c.frame<=p.frame || c.frame-p.frame!=1 ||
        p.queue!=c.queue || c.ticket<=p.ticket || previous.width!=current.width || previous.height!=current.height ||
        !Tolerance(tolerance)) return r;
    Matrix inv{},prior_inv{};
    if(!Inverse(c.view_projection,inv) || !Inverse(p.view_projection,prior_inv)) return r;
    const size_t n=current.depth.size();
    r.current_agreement.resize(n); r.previous_agreement.resize(n);
    for(size_t i=0;i<n;++i) {
        const double d=current.depth[i];
        if(std::abs(d-c.clear_depth)<=1e-7) continue;
        ++r.eligible;
        const double x=double(i%current.width),y=double(i/current.width);
        auto world=Transform(inv,{{2*(x+0.5-c.jitter_x)/current.width-1,
            1-2*(y+0.5-c.jitter_y)/current.height,d,1}});
        if(!std::isfinite(world[3]) || std::abs(world[3])<1e-12) { ++r.excluded; continue; }
        const double w=world[3]; for(double& v:world) v/=w;
        const auto clip=Transform(p.view_projection,world);
        if(!std::isfinite(clip[3]) || clip[3]<=1e-12) { ++r.excluded; continue; }
        const double px=(clip[0]/clip[3]+1)*0.5*previous.width+p.jitter_x-0.5;
        const double py=(1-clip[1]/clip[3])*0.5*previous.height+p.jitter_y-0.5;
        const double z=clip[2]/clip[3];
        if(!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(z) || z<0 || z>1 ||
            px<0 || py<0 || px>previous.width-1 || py>previous.height-1) { ++r.excluded; continue; }
        const auto x0=static_cast<uint32_t>(px),y0=static_cast<uint32_t>(py);
        const auto x1=std::min(x0+1,previous.width-1),y1=std::min(y0+1,previous.height-1);
        const size_t indices[]={size_t(y0)*previous.width+x0,size_t(y0)*previous.width+x1,
            size_t(y1)*previous.width+x0,size_t(y1)*previous.width+x1};
        const double dx=px-x0,dy=py-y0;
        const double weights[]={(1-dx)*(1-dy),dx*(1-dy),(1-dx)*dy,dx*dy};
        double observed=0,lo=1,hi=0; bool sky=false;
        for(size_t k=0;k<4;++k) if(weights[k]>0) {
            const double value=previous.depth[indices[k]];
            sky=sky || std::abs(value-p.clear_depth)<=1e-7;
            lo=std::min(lo,value); hi=std::max(hi,value); observed+=weights[k]*value;
        }
        // Discontinuous bilinear footprints cannot corroborate a surface.
        if(sky || hi-lo>4*tolerance) { ++r.excluded; continue; }
        ++r.checked;
        const double error=std::abs(observed-z); r.max_error=std::max(r.max_error,error);
        const double shift=std::hypot(px-x-(p.jitter_x-c.jitter_x),py-y-(p.jitter_y-c.jitter_y));
        r.max_pixel_shift=std::max(r.max_pixel_shift,shift);
        if(error<=tolerance) {
            ++r.inliers; r.current_agreement[i]=1;
            if(shift>=0.25) ++r.excited;
            for(size_t k=0;k<4;++k) if(weights[k]>0) r.previous_agreement[indices[k]]=1;
        }
    }
    if(r.eligible) r.coverage=double(r.checked)/double(r.eligible);
    if(r.checked) r.inlier_fraction=double(r.inliers)/double(r.checked);
    if(r.checked<16 || r.coverage<0.75) return r;
    // Moving geometry can disagree with a valid static-surface hypothesis.
    // Only gross disagreement contradicts; the middle band remains unknown.
    if(r.inlier_fraction<0.50) r.verdict=TemporalVerdict::Contradicted;
    else if(r.inlier_fraction<0.80 || double(r.excited)<0.25*double(r.checked)) r.verdict=TemporalVerdict::Uninformative;
    else r.verdict=TemporalVerdict::Consistent;
    return r;
}
} // namespace na::classifier
