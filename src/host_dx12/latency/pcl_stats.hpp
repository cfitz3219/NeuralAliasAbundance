#pragma once
#include <windows.h>
#include <cstdint>
namespace na::latency::pcl {
void Initialize();
void Mark(uint32_t marker, uint64_t frame);
void Message(UINT msg, WPARAM key);
// Called at the engine's actual input-copy stage. Returns pending left click.
bool Sample(uint64_t frame);
bool Available();
}
