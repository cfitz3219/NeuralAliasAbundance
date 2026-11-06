#pragma once
#include <array>
#include <cstdint>

namespace na::j2::hud_routes {
// Four acquisition routes, reusing the two existing native-replay/layer
// compositors. This policy never owns or changes the DLSS enable state.
enum class Method : uint32_t { NativeReplay, DirectOrCopy, ShaderHandoff, PixelDiscovery, Off };
inline const char* Name(Method m) {
    constexpr const char* names[]={"native_replay","direct_or_copy","shader_handoff","pixel_discovery","off"};
    return names[static_cast<uint32_t>(m)];
}
struct Key {
    uintptr_t device=0,queue=0;
    uint32_t rw=0,rh=0,ow=0,oh=0,format=0;
    bool operator==(const Key& o)const{return device==o.device&&queue==o.queue&&rw==o.rw&&rh==o.rh&&ow==o.ow&&oh==o.oh&&format==o.format;}
};
struct Ticket {uint64_t epoch=0,trial=0;Method method=Method::Off;};
struct Attempt {uint32_t frames=0,receipts=0,passes=0,failures=0,hud_pixels=0,separated_pixels=0;};
class Policy {
    Key key_{};
    Method method_=Method::NativeReplay;
    uint64_t epoch_=0,trial_=0;
    uint32_t consecutive_=0,failed_=0,no_receipt_=0;
    bool verified_=false;
    std::array<Attempt,4> attempts_{};
    void next(){method_=static_cast<Method>(static_cast<uint32_t>(method_)+1);++trial_;consecutive_=failed_=no_receipt_=0;verified_=false;}
public:
    void Restart(){key_={};method_=Method::NativeReplay;++epoch_;++trial_;attempts_={};consecutive_=failed_=no_receipt_=0;verified_=false;}
    // Only actual, reconstructable scene frames consume a trial. Menus,
    // generated/repeated presents and missing scene inputs do not exhaust it.
    void Frame(const Key& key,bool eligible){
        if(!epoch_||!(key==key_)){key_=key;method_=Method::NativeReplay;++epoch_;++trial_;attempts_={};consecutive_=failed_=no_receipt_=0;verified_=false;}
        if(!eligible||method_==Method::Off)return;
        auto& a=attempts_[static_cast<uint32_t>(method_)];++a.frames;
        if(++no_receipt_>90)next();
    }
    Ticket Current()const{return {epoch_,trial_,method_};}
    Method Selected()const{return method_;}
    bool Verified()const{return verified_;}
    const std::array<Attempt,4>& Attempts()const{return attempts_;}
    bool Receipt(Ticket t,uint32_t hud,uint32_t separated){
        // A late GPU receipt cannot approve/reject a different route, device
        // or resolution. A matching whole image with ZERO HUD is not success.
        if(t.epoch!=epoch_||t.trial!=trial_||t.method!=method_||method_==Method::Off)return false;
        auto& a=attempts_[static_cast<uint32_t>(method_)];++a.receipts;a.hud_pixels=hud;a.separated_pixels=separated;
        if(hud&&uint64_t(separated)*1000>=uint64_t(hud)*980){
            ++a.passes;failed_=no_receipt_=0;if(++consecutive_>=3)verified_=true;
        }else{
            ++a.failures;consecutive_=0;
            // No-HUD frames use the ordinary reconstruction, without revoking
            // a verified route. They still count toward its finite absence timeout.
            if(hud&&++failed_>=8)next();
        }
        return true;
    }
};
}
