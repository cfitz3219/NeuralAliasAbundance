// A6 bounded CPU snapshot analysis; structural hypotheses, never camera trust.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace na::camera {
using Matrix = std::array<double, 16>;
enum class Kind { Projection, RigidView, ViewProjection, InverseViewProjection };
enum class Layout { RowMajor, ColumnMajor };
enum class Sync { Unknown, AppSyncUnknown, IndependentlyVerified };
struct Snapshot {
    uint64_t session{}, epoch{}, submission{}, resource{}, content_version{}, base_offset{};
    Sync sync{Sync::Unknown};
    bool complete{}, lifetime_valid{};
    std::vector<uint8_t> bytes;
};
struct Candidate {
    Kind kind{};
    Layout layout{};
    uint64_t byte_offset{};
    Matrix matrix{};
    double near_plane{}, far_plane{}, handedness{};
    bool reversed{};
    std::array<double, 3> position{};
};
struct Scan {
    std::vector<Candidate> candidates;
    bool unavailable{}, capacity_exceeded{}, partial_capture{};
    bool projection_overflow{}, rigid_overflow{}, combined_overflow{}, inverse_overflow{};
    Sync sync{Sync::Unknown};
};
inline bool Known(uint64_t value) { return value && value!=UINT64_MAX; }
inline bool Near(double a, double b, double epsilon = 1e-5) { return std::abs(a-b) <= epsilon; }
inline bool Finite(const Matrix& m) {
    for (double v : m) if (!std::isfinite(v)) return false;
    return true;
}
inline bool Projection(const Matrix& m, Candidate& c) {
    if (!Finite(m) || m[0] <= 1e-6 || m[5] <= 1e-6 || m[0] > 1e5 || m[5] > 1e5 ||
        std::abs(m[2]) > 2 || std::abs(m[6]) > 2) return false;
    for (size_t index : {1u,3u,4u,7u,8u,9u,12u,13u,15u}) if (!Near(m[index],0)) return false;
    if (!Near(std::abs(m[14]),1) || std::abs(m[11]) <= 1e-12) return false;
    const double sign = m[14] > 0 ? 1.0 : -1.0;
    const double a = m[10]*sign, b = m[11];
    const bool reverse = b > 0;
    const double near_denominator = reverse ? 1-a : -a;
    const double far_denominator = reverse ? -a : 1-a;
    if (std::abs(near_denominator) < 1e-12) return false;
    const double near_plane = b/near_denominator;
    const double far_plane = std::abs(far_denominator) < 1e-12 ?
        std::numeric_limits<double>::infinity() : b/far_denominator;
    if (!std::isfinite(near_plane) || near_plane <= 0 || !(far_plane > near_plane)) return false;
    c.kind = Kind::Projection; c.matrix = m; c.handedness = sign;
    c.near_plane = near_plane; c.far_plane = far_plane; c.reversed = reverse;
    return true;
}
inline bool RigidView(const Matrix& m, Candidate& c) {
    if (!Finite(m) || !Near(m[12],0) || !Near(m[13],0) || !Near(m[14],0) || !Near(m[15],1)) return false;
    for (size_t a = 0; a < 3; ++a)
        for (size_t b = 0; b < 3; ++b) {
            double dot = 0;
            for (size_t j = 0; j < 3; ++j) dot += m[a*4+j]*m[b*4+j];
            if (!Near(dot,a == b ? 1.0 : 0.0,1e-4)) return false;
        }
    const double determinant = m[0]*(m[5]*m[10]-m[6]*m[9]) -
        m[1]*(m[4]*m[10]-m[6]*m[8]) + m[2]*(m[4]*m[9]-m[5]*m[8]);
    if (!Near(std::abs(determinant),1,1e-4)) return false;
    c.kind = Kind::RigidView; c.matrix = m; c.handedness = determinant > 0 ? 1 : -1;
    for (size_t j = 0; j < 3; ++j)
        c.position[j] = -(m[j]*m[3]+m[4+j]*m[7]+m[8+j]*m[11]);
    return true;
}
inline bool Invert(const Matrix& m,Matrix& inverse) {
    if(!Finite(m)) return false;
    double a[4][8]{};
    for(size_t r=0;r<4;++r) for(size_t col=0;col<4;++col) {a[r][col]=m[r*4+col];a[r][col+4]=r==col?1:0;}
    for(size_t col=0;col<4;++col) {
        size_t pivot=col;for(size_t r=col+1;r<4;++r) if(std::abs(a[r][col])>std::abs(a[pivot][col])) pivot=r;
        if(std::abs(a[pivot][col])<1e-10) return false;
        for(size_t k=0;k<8;++k) std::swap(a[pivot][k],a[col][k]);
        const auto divisor=a[col][col];for(double& v:a[col]) v/=divisor;
        for(size_t r=0;r<4;++r) if(r!=col) {const auto factor=a[r][col];for(size_t k=0;k<8;++k) a[r][k]-=factor*a[col][k];}
    }
    for(size_t r=0;r<4;++r) for(size_t col=0;col<4;++col) inverse[r*4+col]=a[r][col+4];
    return Finite(inverse);
}
// Perspective times a rigid transform has a unit projective direction, a
// parallel depth row, and orthogonal horizontal/vertical axes after removing
// off-axis terms. This rejects arbitrary invertible matrices. Object MVPs can
// also satisfy it: the result remains a hypothesis, never main-camera proof.
inline bool ViewProjection(const Matrix& m,Candidate& c) {
    if(!Finite(m)) return false;
    double norm=0;for(size_t j=0;j<3;++j) norm+=m[12+j]*m[12+j];
    if(!Near(norm,1,2e-3)) return false;
    double depth=0,xoffset=0,yoffset=0;
    for(size_t j=0;j<3;++j) {depth+=m[8+j]*m[12+j];xoffset+=m[j]*m[12+j];yoffset+=m[4+j]*m[12+j];}
    depth/=norm;xoffset/=norm;yoffset/=norm;
    if(std::abs(xoffset)>2 || std::abs(yoffset)>2) return false;
    std::array<double,3> x{},y{};double xx=0,yy=0,xy=0;
    for(size_t j=0;j<3;++j) {
        if(!Near(m[8+j],depth*m[12+j],2e-4*std::max(1.0,std::abs(depth)))) return false;
        x[j]=m[j]-xoffset*m[12+j];y[j]=m[4+j]-yoffset*m[12+j];
        xx+=x[j]*x[j];yy+=y[j]*y[j];xy+=x[j]*y[j];
    }
    if(xx<1e-8 || yy<1e-8 || xx>1e8 || yy>1e8 || std::abs(xy)>2e-3*std::sqrt(xx*yy)) return false;
    Matrix projection{std::sqrt(xx/norm),0,xoffset,0,0,std::sqrt(yy/norm),yoffset,0,0,0,depth,(m[11]-depth*m[15])/std::sqrt(norm),0,0,1,0};
    Candidate p{};if(!Projection(projection,p)) return false;
    c=p;c.kind=Kind::ViewProjection;c.matrix=m;
    const double tx=(m[3]-xoffset*m[15])/std::sqrt(xx),ty=(m[7]-yoffset*m[15])/std::sqrt(yy);
    for(size_t j=0;j<3;++j) c.position[j]=-(x[j]/std::sqrt(xx)*tx+y[j]/std::sqrt(yy)*ty+m[12+j]*m[15]);
    return true;
}
inline bool InverseViewProjection(const Matrix& m,Candidate& c) {
    // Perspective inverse has a zero in the third row's first three entries
    // only before view multiplication; general inverse VP needs full inversion.
    Matrix inverse{};if(!Invert(m,inverse) || !ViewProjection(inverse,c)) return false;
    c.kind=Kind::InverseViewProjection;c.matrix=m;return true;
}
inline Scan FindMatrices(const Snapshot& s,bool allow_partial_hypotheses=false) {
    Scan result{}; result.sync = s.sync;result.partial_capture=!s.complete;
    if (!Known(s.session) || !Known(s.epoch) || !Known(s.submission) || !Known(s.resource) || !Known(s.content_version) ||
        (!s.complete && !allow_partial_hypotheses) || !s.lifetime_valid || s.sync == Sync::Unknown || s.base_offset % 4 ||
        s.bytes.size() % 4 || s.bytes.size() < 64 ||
        s.base_offset > std::numeric_limits<uint64_t>::max()-s.bytes.size()) {
        result.unavailable = true; return result;
    }
    if (s.bytes.size() > 1024*1024) { result.capacity_exceeded = true; return result; }
    std::array<size_t,4> counts{};
    for (size_t offset = 0; offset <= s.bytes.size()-64; offset += 4) {
        Matrix row{};
        for (size_t i = 0; i < 16; ++i) {
            float value{}; std::memcpy(&value,s.bytes.data()+offset+i*4,4);
            row[i] = value;
        }
        Matrix transposed{};
        for(size_t r=0;r<4;++r) for(size_t col=0;col<4;++col) transposed[r*4+col]=row[col*4+r];
        Candidate simple{};
        const bool has_simple=Projection(row,simple) || RigidView(row,simple) ||
            Projection(transposed,simple) || RigidView(transposed,simple);
        for (Layout layout : {Layout::RowMajor,Layout::ColumnMajor}) {
            Matrix m{};
            for (size_t r = 0; r < 4; ++r) for (size_t col = 0; col < 4; ++col)
                m[r*4+col] = layout == Layout::RowMajor ? row[r*4+col] : row[col*4+r];
            Candidate c{};
            if (!(Projection(m,c) || RigidView(m,c) ||
                (!has_simple && (ViewProjection(m,c) || InverseViewProjection(m,c))))) continue;
            c.layout = layout;c.byte_offset = s.base_offset+offset;
            auto& count = counts[static_cast<size_t>(c.kind)];
            auto& overflow = c.kind == Kind::Projection ? result.projection_overflow :
                c.kind == Kind::RigidView ? result.rigid_overflow :
                c.kind == Kind::ViewProjection ? result.combined_overflow : result.inverse_overflow;
            if (overflow) continue;
            if (++count > 256) {
                overflow = true; result.capacity_exceeded = true;
                // An overflowing kind has no apparent truncated winner, but must
                // not erase other kinds or stop scanning for later projections.
                result.candidates.erase(std::remove_if(result.candidates.begin(), result.candidates.end(),
                    [&](const Candidate& prior) { return prior.kind == c.kind; }), result.candidates.end());
            } else {
                result.candidates.push_back(c);
            }
        }
    }
    return result;
}

struct OffsetMeasurement { bool available{}; double x{}, y{}; };
// The reference must be an independently established UNJITTERED projection of
// the same camera. Otherwise this is merely off-axis projection, not jitter.
inline OffsetMeasurement MeasureOffset(const Candidate& current, const Candidate& reference,
    uint32_t width, uint32_t height) {
    if (current.kind != Kind::Projection || reference.kind != Kind::Projection || !width || !height)
        return {};
    Candidate a{}, b{};
    if (!Projection(current.matrix,a) || !Projection(reference.matrix,b)) return {};
    for (size_t i = 0; i < 16; ++i)
        if (i != 2 && i != 6 && !Near(current.matrix[i],reference.matrix[i])) return {};
    return {true, (current.matrix[2]-reference.matrix[2])*a.handedness*width*0.5,
        -(current.matrix[6]-reference.matrix[6])*a.handedness*height*0.5};
}
enum class Continuity { Unknown, Continuous, Discontinuity };
inline Continuity CompareViews(const Snapshot& previous_scope, const Candidate& previous,
    const Snapshot& current_scope, const Candidate& current, bool adjacent_frames,
    double max_translation, double max_rotation_radians) {
    if (!adjacent_frames || !previous_scope.complete || !current_scope.complete || !previous_scope.lifetime_valid ||
        !current_scope.lifetime_valid || previous_scope.sync == Sync::Unknown || current_scope.sync == Sync::Unknown ||
        !Known(previous_scope.session) || !Known(previous_scope.epoch) || !Known(previous_scope.resource) ||
        !Known(previous_scope.submission) || !Known(current_scope.submission) ||
        !Known(previous_scope.content_version) || !Known(current_scope.content_version) ||
        previous_scope.session != current_scope.session || previous_scope.epoch != current_scope.epoch ||
        previous_scope.resource != current_scope.resource || current_scope.submission <= previous_scope.submission ||
        previous.byte_offset != current.byte_offset || previous.layout != current.layout ||
        previous.kind != Kind::RigidView || current.kind != Kind::RigidView ||
        !std::isfinite(max_translation) || max_translation <= 0 || !std::isfinite(max_rotation_radians) ||
        max_rotation_radians <= 0 || max_rotation_radians > 3.141592653589793) return Continuity::Unknown;
    Candidate a{}, b{};
    if (!RigidView(previous.matrix,a) || !RigidView(current.matrix,b)) return Continuity::Unknown;
    if (a.handedness != b.handedness) return Continuity::Discontinuity;
    double distance_squared = 0, trace = 0;
    for (size_t i = 0; i < 3; ++i) {
        const double delta = a.position[i]-b.position[i]; distance_squared += delta*delta;
        for (size_t j = 0; j < 3; ++j) trace += a.matrix[i*4+j]*b.matrix[i*4+j];
    }
    const double cosine = (trace-1)*0.5;
    const double angle = std::acos(cosine < -1 ? -1 : (cosine > 1 ? 1 : cosine));
    return std::sqrt(distance_squared) > max_translation || angle > max_rotation_radians ?
        Continuity::Discontinuity : Continuity::Continuous;
}
} // namespace na::camera
