// Home menu font images without dropped menu frames (both APIs: native DX12,
// native DX11 and 11on12 all draw the menu through this one D3D12 path).
//
// ImGui 1.92 bakes text on demand. A page, slider value, switch label, help
// line or text size that has not been drawn before adds glyphs to the font
// image (status WantUpdates); a full image grows or is repacked into a NEW
// image and the old one is retired (WantDestroy). Earlier menu draws can
// still be sampling that image on the GPU.
//
// The overlay used to SKIP the whole menu draw whenever any font request was
// pending while an earlier menu draw was unfinished. With the GPU one to three
// frames behind, that is nearly every frame where text changes, and the skip
// repeats until the GPU catches up: the menu vanished for a few presents and
// came back ("the whole UI flickers a few times then goes solid"), mostly on
// page switches, slider drags and switch changes, which show new text.
//
// The keeper never drops the draw. While earlier menu draws are unfinished:
//  - an image that wants new glyphs is copied whole into a fresh image (the
//    backend's WantCreate path, from ImGui's CPU copy), so nothing on the GPU
//    is written while it may be read;
//  - an image ImGui wants destroyed is detached;
// and the detached image is released only once every menu draw that was
// unfinished at that moment has completed (checked by fence, never waited).
// With no menu draw on the GPU the backend updates in place, as before.
#pragma once

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace na::overlay {

// One earlier menu draw that has not completed: its completion fence and value.
struct MenuDrawInFlight {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};

class MenuTextureKeeper {
public:
    // Well inside the overlay's 64-entry SRV heap: each detached image keeps
    // its descriptor until its draws complete.
    static constexpr size_t kMaxDetached = 24;

    struct Stats {
        std::atomic<uint64_t> fresh_copies{0}; // glyph updates moved to a fresh image while draws were in flight
        std::atomic<uint64_t> detached{0};     // images detached (fresh copies + retired images)
        std::atomic<uint64_t> released{0};     // detached images released after their draws completed
        std::atomic<uint64_t> refused{0};      // draws still skipped: a draw's completion is unreadable, or too many detached
        std::atomic<uint64_t> pending{0};      // detached images waiting for their draws
    };

    // Call after ImGui::Render and before ImGui_ImplDX12_RenderDrawData.
    // `in_flight`: the earlier menu draws that have not completed.
    // `unreadable`: an earlier draw whose completion cannot be read (its
    // Signal failed). Returns false only when the draw must still be skipped
    // (`unreadable`, or kMaxDetached reached); the request then stays pending.
    bool Prepare(ImVector<ImTextureData*>* textures, const std::vector<MenuDrawInFlight>& in_flight, bool unreadable)
    {
        if (!textures || (in_flight.empty() && !unreadable)) return true; // nothing reads the images: update in place
        size_t needed = 0;
        for (ImTextureData* t : *textures)
            if (Detachable(t)) ++needed;
        if (!needed) return true;
        if (unreadable || detached_.size() + needed > kMaxDetached) {
            stats_.refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        for (ImTextureData* t : *textures) {
            if (!Detachable(t)) continue;
            auto* ghost = IM_NEW(ImTextureData)();
            ghost->SetTexID(t->TexID);
            ghost->BackendUserData = t->BackendUserData;
            t->SetTexID(ImTextureID_Invalid);
            t->BackendUserData = nullptr;
            if (t->Status == ImTextureStatus_WantUpdates) {
                // The whole image from ImGui's CPU copy, into a new GPU image.
                t->SetStatus(ImTextureStatus_WantCreate);
                stats_.fresh_copies.fetch_add(1, std::memory_order_relaxed);
            } else {
                t->SetStatus(ImTextureStatus_Destroyed); // ImGui drops it next frame
            }
            detached_.push_back({ghost, in_flight});
            stats_.detached.fetch_add(1, std::memory_order_relaxed);
        }
        stats_.pending.store(detached_.size(), std::memory_order_relaxed);
        return true;
    }

    // Releases detached images whose draws have completed. `all_complete`:
    // the caller knows every menu draw has retired (teardown paths). Must run
    // while the ImGui DX12 backend is still initialised.
    void Collect(bool all_complete)
    {
        size_t kept = 0;
        for (size_t i = 0; i < detached_.size(); ++i) {
            if (all_complete || Done(detached_[i])) {
                Release(detached_[i].ghost);
                stats_.released.fetch_add(1, std::memory_order_relaxed);
            } else {
                detached_[kept++] = std::move(detached_[i]);
            }
        }
        detached_.resize(kept);
        stats_.pending.store(detached_.size(), std::memory_order_relaxed);
    }

    size_t Pending() const { return detached_.size(); }
    const Stats& GetStats() const { return stats_; }

private:
    struct Detached {
        ImTextureData* ghost = nullptr; // holds the backend's image (TexID + BackendUserData)
        std::vector<MenuDrawInFlight> draws;
    };

    static bool Detachable(const ImTextureData* t)
    {
        return t && t->BackendUserData &&
               (t->Status == ImTextureStatus_WantUpdates || t->Status == ImTextureStatus_WantDestroy);
    }
    static bool Done(const Detached& d)
    {
        for (const auto& draw : d.draws) {
            const UINT64 done = draw.fence ? draw.fence->GetCompletedValue() : UINT64_MAX;
            if (done == UINT64_MAX || done < draw.value) return false; // device removed: keep it, never guess
        }
        return true;
    }
    static void Release(ImTextureData* ghost)
    {
        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData) {
            ghost->SetStatus(ImTextureStatus_WantDestroy);
            ghost->UnusedFrames = INT_MAX; // past the backend's frames-in-flight count: destroy now
            ImGui_ImplDX12_UpdateTexture(ghost); // frees its descriptor and GPU image
        }
        IM_DELETE(ghost);
    }

    std::vector<Detached> detached_;
    Stats stats_;
};

} // namespace na::overlay
