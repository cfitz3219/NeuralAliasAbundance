#include "adapters/camera_cut.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
using alien_isolation::CameraCutDetector;
static std::array<float,16> identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
static void expect(bool ok,const char* name){if(!ok)throw std::runtime_error(name);}
int main(){try{
    CameraCutDetector c;auto m=identity();expect(c.observe(m,0,0).reset,"first");
    for(unsigned i=1;i<40;++i){m[3]=float(i)*.01f;m[7]=(i%2?1.f:-1.f)/1200;
        expect(!c.observe(m,i,i*17).reset,"continuous motion and subpixel jitter");}
    m[3]+=2;auto cut=c.observe(m,40,680);expect(cut.reset && cut.reason==CameraCutDetector::Reason::jump,"consecutive-frame teleport");
    expect(!c.observe(m,41,697).reset,"history resumes after cut");
    m[0]=4;expect(c.observe(m,42,714).reset,"abrupt FOV change");
    c.clear();m=identity();c.observe(m,0,0);m[0]=0;m[2]=1;m[8]=-1;m[10]=0;
    expect(c.observe(m,1,17).reset,"90 degree rotation");
    expect(c.observe(m,3,34).reset,"missing frame");expect(c.observe(m,4,400).reset,"time gap");
    m[0]=std::numeric_limits<float>::quiet_NaN();expect(c.observe(m,5,417).reset,"nonfinite");
    m={};expect(c.observe(m,6,434).reset,"singular");
    c.clear();m=identity();c.observe(m,0,0);
    // Equivalent homogeneous matrix scaling must not be interpreted as movement.
    for(auto& f:m)f*=2;expect(!c.observe(m,1,17).reset,"homogeneous scale invariance");
    std::puts("PASS: cut heuristic: continuous motion/jitter, teleport, rotation, FOV, gaps, invalid data, homogeneous scaling.");
    return 0;
}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}}
