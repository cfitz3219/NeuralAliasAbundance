#pragma once
// Our own video memory, item by item (coordinator 2026-11-27, after Cort saw 10 of 15 GB in use: the
// process figure Windows gives holds the game's memory and ours together, so ours is counted here).
// Every module that makes or frees a large GPU resource of ours says so with Set(name, bytes, why):
// one "vram_item" event per change (name, MB, why, and our total), and the status writer prints the
// whole ledger as one "vram_ours" line next to the process usage and budget. Header-only, like the
// event log, so every host library can report without a new link dependency.
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "host_dx12/diag/event_log.hpp"

namespace na::vram {

inline std::mutex g_m;
inline std::map<std::string, uint64_t> g_items; // name -> bytes held now (absent or 0: nothing)

inline double Mb(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

inline uint64_t TotalBytes()
{
    std::lock_guard<std::mutex> lk(g_m);
    uint64_t t = 0;
    for (const auto& [name, bytes] : g_items) t += bytes;
    return t;
}

// The item `name` now holds `bytes` (0: released). `why` is plain words: what it is for, or why it went.
inline void Set(const char* name, uint64_t bytes, const char* why)
{
    if (!name || !*name) return;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_items.find(name);
        const uint64_t before = it == g_items.end() ? 0 : it->second;
        if (before == bytes) return;
        if (bytes) g_items[name] = bytes;
        else if (it != g_items.end()) g_items.erase(it);
        for (const auto& [n, b] : g_items) total += b;
    }
    na::eventlog::Emit("vram_item", "name=%s mb=%.1f reason=\"%s\" ours_total_mb=%.1f", name, Mb(bytes), why ? why : "", Mb(total));
}

inline uint64_t Get(const char* name)
{
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_items.find(name ? name : "");
    return it == g_items.end() ? 0 : it->second;
}

// The memory a D3D12 resource takes on the card (its allocation, alignment included). Null: 0.
inline uint64_t SizeOf(ID3D12Resource* r)
{
    if (!r) return 0;
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    if (FAILED(r->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return 0;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    return dev->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}
// A D3D11 buffer's size (D3D11 gives no allocation figure: its byte width).
inline uint64_t SizeOf(ID3D11Buffer* b)
{
    if (!b) return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return d.ByteWidth;
}

// "vram_ours total_mb=... items{name=mb ...}" for the status log (no line end). 0 when nothing is held.
inline int FormatLine(char* out, size_t size)
{
    if (!out || !size) return 0;
    std::lock_guard<std::mutex> lk(g_m);
    uint64_t total = 0;
    for (const auto& [n, b] : g_items) total += b;
    int off = std::snprintf(out, size, "vram_ours total_mb=%.1f items=%zu {", Mb(total), g_items.size());
    for (const auto& [n, b] : g_items) {
        if (off <= 0 || static_cast<size_t>(off) >= size) break;
        off += std::snprintf(out + off, size - static_cast<size_t>(off), "%s%s=%.1f", off && out[off - 1] == '{' ? "" : " ", n.c_str(), Mb(b));
    }
    if (off > 0 && static_cast<size_t>(off) < size) off += std::snprintf(out + off, size - static_cast<size_t>(off), "}");
    return off > 0 && static_cast<size_t>(off) < size ? off : static_cast<int>(size) - 1;
}

} // namespace na::vram
