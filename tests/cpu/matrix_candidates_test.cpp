#include "test_support.hpp"
#include "providers/camera/matrix_candidates.hpp"
using namespace na::camera;
static Matrix perspective(bool reverse=false, double sign=1) {
    const double n=0.1,f=30;
    return {1.3,0,0,0, 0,1.8,0,0, 0,0,
        sign*(reverse ? n/(n-f) : f/(f-n)), reverse ? n*f/(f-n) : -n*f/(f-n),
        0,0,sign,0};
}
static Matrix view(double x=0) { return {1,0,0,-x, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
static Snapshot snapshot(const Matrix& m, bool transpose=false) {
    Snapshot s{1,2,3,4,5,16,Sync::AppSyncUnknown,true,true,{}};
    s.bytes.resize(64);
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) {
        const float v=static_cast<float>(m[transpose ? c*4+r : r*4+c]);
        std::memcpy(s.bytes.data()+(r*4+c)*4,&v,4);
    }
    return s;
}
int main() { return test::run({
    {"projection conventions and layouts recover near far", [] {
        for (bool reverse : {false,true}) for (double sign : {-1.0,1.0}) for (bool column : {false,true}) {
            const auto r=FindMatrices(snapshot(perspective(reverse,sign),column));
            CHECK(!r.unavailable && !r.capacity_exceeded && r.candidates.size()==1);
            const auto& c=r.candidates[0];
            CHECK(c.kind==Kind::Projection && c.reversed==reverse && c.handedness==sign);
            CHECK(c.layout==(column ? Layout::ColumnMajor : Layout::RowMajor));
            NEAR(c.near_plane,0.1,1e-6); NEAR(c.far_plane,30,0.001);
            CHECK(r.sync==Sync::AppSyncUnknown); // no synchronization upgrade
        }
    }},
    {"infinite far supported but orthographic and invalid projection rejected", [] {
        auto m=perspective(); m[10]=1; m[11]=-0.1;
        Candidate c{}; CHECK(Projection(m,c) && std::isinf(c.far_plane));
        CHECK(!Projection(view(),c)); m[11]=0; CHECK(!Projection(m,c));
        m=perspective(); m[0]=std::nan(""); CHECK(!Projection(m,c));
    }},
    {"rigid view recovers position but object transforms stay hypotheses", [] {
        Candidate c{}; CHECK(RigidView(view(2),c)); NEAR(c.position[0],2,1e-9);
        auto m=view(); m[0]=2; CHECK(!RigidView(m,c));
        m=view(); m[0]=-1; CHECK(!RigidView(m,c));
        CHECK(FindMatrices(snapshot(view())).candidates.size()==2); // symmetric layout ambiguity
    }},
    {"snapshot losses unknown sync alignment and overflow fail visibly", [] {
        auto s=snapshot(perspective()); s.complete=false; CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.sync=Sync::Unknown; CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.base_offset=2; CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.base_offset=UINT64_MAX-3; CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.bytes.pop_back(); CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.content_version=UINT64_MAX; CHECK(FindMatrices(s).unavailable);
        s=snapshot(perspective()); s.bytes.resize(1024*1024+4); CHECK(FindMatrices(s).capacity_exceeded);
    }},
    {"candidate flood never returns a truncated apparent winner", [] {
        auto s=snapshot(view()); const auto bytes=s.bytes;
        for (unsigned i=0;i<300;++i) s.bytes.insert(s.bytes.end(),bytes.begin(),bytes.end());
        const auto r=FindMatrices(s); CHECK(r.capacity_exceeded && r.candidates.empty());
    }},
    {"jitter measurement requires matching independent reference", [] {
        Candidate reference{},current{}; CHECK(Projection(perspective(),reference));
        auto m=perspective(); m[2]=2*0.25/1920; m[6]=-2*(-0.375)/1080;
        CHECK(Projection(m,current)); const auto offset=MeasureOffset(current,reference,1920,1080);
        CHECK(offset.available); NEAR(offset.x,0.25,1e-9); NEAR(offset.y,-0.375,1e-9);
        current.matrix[0]*=2; CHECK(!MeasureOffset(current,reference,1920,1080).available);
    }},
    {"continuity detects jumps and rejects missing lineage", [] {
        auto prior=snapshot(view()), next=snapshot(view(0.12)); ++next.submission; ++next.content_version;
        Candidate a{},b{}; CHECK(RigidView(view(),a)); CHECK(RigidView(view(0.12),b));
        CHECK(CompareViews(prior,a,next,b,true,0.5,0.5)==Continuity::Continuous);
        CHECK(RigidView(view(2),b));
        CHECK(CompareViews(prior,a,next,b,true,0.5,0.5)==Continuity::Discontinuity);
        CHECK(CompareViews(prior,a,next,b,false,0.5,0.5)==Continuity::Unknown);
        ++next.epoch; CHECK(CompareViews(prior,a,next,b,true,0.5,0.5)==Continuity::Unknown);
    }}
}); }
