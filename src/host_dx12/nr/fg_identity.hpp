#pragma once
#include "host_dx12/nr/fg_contract.hpp"
#include <optional>
namespace na::nr {
// The NGX ID is optional. Without it, one legacy FG invocation describes one
// real-frame pair; explicit MFG indices describe the inner frames of that pair.
// NVIDIA nvsdk_ngx_params_dlssg.h defaults count/index to 1/1. This uses the FG
// protocol, not SR calls, global Presents, focus or texture-pointer reuse.
class FgIdentity {
public:
    enum Mode : uint32_t { Native=1, LegacyPair=2, MultiPair=3 };
    std::optional<uint64_t> Resolve(const FgContract& f) {
        if(f.has_frame_id){mode=Native;return f.frame_id;}
        const auto count=f.has_multi_count?f.multi_count:1u;
        const auto index=f.has_multi_index?f.multi_index:1u;
        if(!count || !index || index>count || (count>1 && !f.has_multi_index))return {};
        mode=count==1?LegacyPair:MultiPair;
        if(index==1){++serial;last_index=1;last_count=count;return serial;}
        if(!serial || last_count!=count || (index!=last_index && index!=last_index+1))return {};
        last_index=index;return serial;
    }
    uint32_t mode=0;
private:
    uint64_t serial=0;
    uint32_t last_index=0,last_count=0;
};
}
