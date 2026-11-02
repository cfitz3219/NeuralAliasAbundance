#pragma once
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/observe/producers.hpp"

namespace na::overlay {
// These calls only maintain NeuralAlias's private completion fences. They are
// not reentrant GAME queue work and must not invalidate the game's state trace.
// Keep the actual game forward and subscriber callbacks outside these scopes.
inline void BeforeGameSubmission(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    const na::observe::HostWorkScope host;
    na::dlss::BeforeExecute(q,n,lists);
}
inline void AfterGameSubmission(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    const na::observe::HostWorkScope host;
    na::gputime::OnExecuted(q,n,lists);
    na::dlss::OnExecuted(q,n,lists);
}
}
