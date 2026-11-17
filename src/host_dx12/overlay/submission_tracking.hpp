#pragma once
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"
#include "host_dx12/observe/producers.hpp"

namespace na::overlay {
// Reused from Astra's 9853a49, independently qualified on the 6f baseline.
// Only our private completion bookkeeping is host work. The actual game
// submission and subscriber callbacks stay outside these scopes.
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
