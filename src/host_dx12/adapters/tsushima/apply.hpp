#pragma once
#include "host_dx12/adapters/tlou1/apply.hpp"
#include <array>
#include <cstring>
namespace na::tsushima {
using na::tlou1::Choice;
using na::tlou1::ApplyStatus;
using na::tlou1::Stage;
// Installed executable's GraphicsSettings POD, verified by its menu setter,
// registry schema and native request/apply copies. See docs/GHOST-LIVE-QUALITY.md.
using GraphicsSettings = std::array<unsigned char, 0x148>;
inline uint32_t Read(const GraphicsSettings& s, size_t offset) {
    uint32_t value; std::memcpy(&value, s.data()+offset, sizeof(value)); return value;
}
inline bool SetQuality(GraphicsSettings& s, uint32_t mode) {
    if (mode > 4) return false;
    auto put=[&](size_t o,uint32_t v){std::memcpy(s.data()+o,&v,sizeof(v));};
    // Ghost: method 1=DLSS, AA 4=DLAA with method Off. Quality 0=Ultra
    // Performance,1=Performance,2=Balanced,3=Quality. DRS must yield to fixed mode.
    put(0x4c,0);
    put(0xb0,mode==0?0:1);
    if(mode==0) put(0x88,4);
    else put(0xb4,4-mode);
    return true;
}
void InitializeApply();
bool RequestApply(Choice choice);
ApplyStatus GetApplyStatus();
} // namespace na::tsushima
