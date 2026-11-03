#pragma once
#include "host_dx12/nr/nr_pass.hpp"
namespace na::slb { struct PresentEvent; }
namespace na::nr::slconsumer {
bool Start();
void SetRuntime(const Runtime& runtime);
void Shutdown(const Runtime& runtime);
bool OwnsDevice(ID3D12Device* device);
bool OwnsAny();
void ForgetStream(uint32_t proxy_id);
// Same consumer body, with the fixture's exact queue/color binding.
void ConsumeForTests(const slb::PresentEvent&,ID3D12CommandQueue*,DXGI_COLOR_SPACE_TYPE);
}
