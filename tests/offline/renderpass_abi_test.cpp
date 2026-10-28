// SDK ABI verification for the supplied PostEntryPSO specification.
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include <cstdio>
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl, BeginRenderPass) / sizeof(void*) == 68);
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl, EndRenderPass) / sizeof(void*) == 69);
int main() { std::puts("PASS: SDK BeginRenderPass=68, EndRenderPass=69 (not 62/63)"); }
