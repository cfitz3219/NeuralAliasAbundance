#pragma once
// Native NGX FG inputs, decoded without borrowing SR/RR parameter keys.
// Presence is retained: absence of a frame ID is not frame zero.
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <cstdint>
#include <cstdio>

namespace na::nr {
struct FgRegion {
    uint32_t x=0, y=0, width=0, height=0;
    uint32_t fields=0;
};
struct FgSurface {
    ID3D12Resource* resource=nullptr; // borrowed for the evaluate call only
    FgRegion region;
};
struct FgContract {
    FgSurface color, hudless, depth, motion, ui, alpha, distortion, real_output, generated_output;
    unsigned long long frame_id=0;
    uint32_t multi_index=0, multi_count=0, reset=0, hdr=0, depth_inverted=0;
    uint32_t camera_motion=1, motion_jittered=0;
    float motion_x=0, motion_y=0;
    bool has_frame_id=false, has_multi_index=false, has_multi_count=false;
    bool has_motion_x=false, has_motion_y=false, has_hdr=false;
};
template<class T>
inline bool FgRead(const NVSDK_NGX_Parameter* p, const char* key, T* value) {
    return p && p->Get(key, value) == NVSDK_NGX_Result_Success;
}
inline FgSurface ReadFgSurface(const NVSDK_NGX_Parameter* p, const char* name, const char* region_name=nullptr) {
    FgSurface out;
    char key[128];
    std::snprintf(key, sizeof key, "DLSSG.%s", name); FgRead(p,key,&out.resource);
    const char* suffixes[]={"BaseX","BaseY","Width","Height"};
    uint32_t* values[]={&out.region.x,&out.region.y,&out.region.width,&out.region.height};
    for(uint32_t i=0;i<4;++i) {
        std::snprintf(key,sizeof key,"DLSSG.%sSubrect%s",region_name?region_name:name,suffixes[i]);
        if(FgRead(p,key,values[i])) out.region.fields|=1u<<i;
    }
    return out;
}
inline FgContract ReadFgContract(const NVSDK_NGX_Parameter* p) {
    FgContract f;
    f.color=ReadFgSurface(p,"Backbuffer","InputBackbuffer");
    // The old Backbuffer subrect applies to both input and output. Only use
    // it if the new input region is wholly absent; never mix partial tuples.
    if(!f.color.region.fields) f.color.region=ReadFgSurface(p,"Backbuffer").region;
    f.hudless=ReadFgSurface(p,"HUDLess"); f.depth=ReadFgSurface(p,"Depth");
    f.motion=ReadFgSurface(p,"MVecs"); f.ui=ReadFgSurface(p,"UI"); f.alpha=ReadFgSurface(p,"UIAlpha");
    f.distortion=ReadFgSurface(p,"BidirectionalDistortionField");
    f.real_output=ReadFgSurface(p,"OutputReal"); f.generated_output=ReadFgSurface(p,"OutputInterpolated");
    f.has_frame_id=FgRead(p,"DLSSG.BackbufferFrameID",&f.frame_id);
    f.has_multi_index=FgRead(p,"DLSSG.MultiFrameIndex",&f.multi_index);
    f.has_multi_count=FgRead(p,"DLSSG.MultiFrameCount",&f.multi_count);
    f.has_motion_x=FgRead(p,"DLSSG.MvecScaleX",&f.motion_x);
    f.has_motion_y=FgRead(p,"DLSSG.MvecScaleY",&f.motion_y);
    f.has_hdr=FgRead(p,"DLSSG.ColorBuffersHDR",&f.hdr);
    FgRead(p,"DLSSG.Reset",&f.reset); FgRead(p,"DLSSG.DepthInverted",&f.depth_inverted);
    FgRead(p,"DLSSG.CameraMotionIncluded",&f.camera_motion);FgRead(p,"DLSSG.MvecJittered",&f.motion_jittered);
    return f;
}
} // namespace na::nr
