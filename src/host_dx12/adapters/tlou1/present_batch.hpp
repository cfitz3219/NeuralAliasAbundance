#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>
namespace na::tlou1 {
// Pinned 1.1.5 submit-worker records: stride80, opcode at77, Present=6,
// engine frame at48 (passed to RenderFrameParams lookup immediately before
// DXGI Present). A multi-Present batch is deliberately not attributed.
inline uint64_t PresentBatchFrame(const uint8_t* records, size_t size) {
    if (size%80) return 0;
    uint64_t result=0; unsigned presents=0;
    for(size_t i=0;i<size;i+=80) if(records[i+77]==6) {
        if(++presents!=1) return 0;
        uint64_t frame; std::memcpy(&frame,records+i+48,8);
        if(frame==UINT64_MAX) return 0;
        result=frame+1;
    }
    return result;
}
}
