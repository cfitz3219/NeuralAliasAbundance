#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace alien_isolation {
// Heuristic, not an engine cut flag. Matrix layout follows the pinned motion PS dp4 rows.
// Compare frustum points in normalized screen space so world-coordinate scale is irrelevant.
class CameraCutDetector {
public:
    enum class Reason { none, first_or_gap, invalid_matrix, jump };
    struct Result { bool reset; Reason reason; double displacement; };
    void clear(){valid_=false;previous_motion_=0;}
    Result observe(const std::array<float,16>& current,uint64_t frame,uint64_t time){
        std::array<double,16> inverse{};
        if(!invert(current,inverse)){clear();return {true,Reason::invalid_matrix,0};}
        if(!valid_ || frame!=frame_+1 || time<time_ || time-time_>250){
            save(current,frame,time,0);return {true,Reason::first_or_gap,0};
        }
        std::array<double,27> distances{};unsigned n=0,behind=0;
        for(double z:{.1,.5,.9})for(double y:{-.7,0.,.7})for(double x:{-.7,0.,.7}){
            const std::array<double,4> p{x,y,z,1};auto world=multiply(inverse,p);
            if(std::abs(world[3])<1e-10){++behind;continue;}
            const double w=world[3];for(auto& v:world)v/=w;
            auto old=multiply(previous_,world);
            if(old[3]<=1e-10){++behind;continue;}
            const double dx=old[0]/old[3]-x,dy=old[1]/old[3]-y;
            distances[n++]=.5*std::hypot(dx,dy);
        }
        if(n)std::sort(distances.begin(),distances.begin()+n);
        const double movement=n?distances[n/2]:1;
        const double dt=std::clamp(double(time-time_)/16.6667,.5,3.);
        const bool jump=behind>=14 || movement>.75*dt ||
            (movement>.20*dt && movement>4*previous_motion_+.02);
        save(current,frame,time,jump?0:movement);
        return {jump,jump?Reason::jump:Reason::none,movement};
    }
private:
    template<class T>static std::array<double,4> multiply(const std::array<T,16>& m,const std::array<double,4>& p){
        std::array<double,4> r{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)r[y]+=m[y*4+x]*p[x];return r;
    }
    static bool invert(const std::array<float,16>& m,std::array<double,16>& out){
        double a[4][8]{};
        for(unsigned y=0;y<4;++y){for(unsigned x=0;x<4;++x){if(!std::isfinite(m[y*4+x]))return false;a[y][x]=m[y*4+x];}a[y][y+4]=1;}
        for(unsigned col=0;col<4;++col){
            unsigned pivot=col;for(unsigned y=col+1;y<4;++y)if(std::abs(a[y][col])>std::abs(a[pivot][col]))pivot=y;
            if(std::abs(a[pivot][col])<1e-12)return false;
            for(unsigned x=0;x<8;++x)std::swap(a[col][x],a[pivot][x]);
            const double scale=a[col][col];for(auto& v:a[col])v/=scale;
            for(unsigned y=0;y<4;++y)if(y!=col){const double f=a[y][col];for(unsigned x=0;x<8;++x)a[y][x]-=f*a[col][x];}
        }
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){out[y*4+x]=a[y][x+4];if(!std::isfinite(out[y*4+x]))return false;}
        return true;
    }
    void save(const std::array<float,16>& m,uint64_t f,uint64_t t,double motion){previous_=m;frame_=f;time_=t;previous_motion_=motion;valid_=true;}
    std::array<float,16> previous_{};uint64_t frame_=0,time_=0;double previous_motion_=0;bool valid_=false;
};
}
