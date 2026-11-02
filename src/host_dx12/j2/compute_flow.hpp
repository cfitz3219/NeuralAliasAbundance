#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <cstddef>
namespace na::motion::gpu {struct Inputs;}
namespace na::j2::compute_flow {
struct Frame {
    // slot is the caller's ring hint; private resources follow recording leases.
    uint64_t frame=0,previous=0;uint32_t generation=0,slot=0;bool reset=false,hdr=false;
    float jitter[2]{};
};
struct Ticket {std::shared_ptr<void> context;uint32_t image=0,previous=0,slot=0;Frame frame;bool pair=false;};
struct Tier3 {ID3D12Resource *residual=nullptr,*valid=nullptr,*mask=nullptr;};
// Called inside J2's restorable inline scope, AFTER its colour/depth resolve.
// All inputs NPSR. No CPU waits; allocations/retirement reuse RecordingLease.
Ticket Record(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs&,const Frame&,Tier3&);
void Committed(const Ticket&); // only after J2's actual submission, never on record
void Reset(); // old contexts stay retained by their executable/GPU recordings
struct Stats {
    uint64_t recorded=0,submitted=0,pairs=0,busy=0,resets=0,accepted=0,total=0;
    uint64_t attempts=0,submitted_pairs=0,waiting_history=0,unavailable=0,samples=0;
    uint64_t last_submitted_frame=0,sample_frame=0,submit_age_ms=0,sample_age_ms=0;
    uint64_t rejected_invalid=0,rejected_small=0,rejected_cost=0,rejected_offscreen=0,rejected_image=0,rejected_flicker=0; // flicker: passed every check, no agreeing previous pair yet
    uint32_t recording_slots=0,image_slots=0;
    double ms=0;char reason[160]{};
};
Stats GetStats();
// The result is explicitly a last completed PIXEL sample, with its age; it
// never substitutes an old percentage for evidence that frames still run.
void DescribeStats(const Stats&,char* text,size_t capacity);
}
