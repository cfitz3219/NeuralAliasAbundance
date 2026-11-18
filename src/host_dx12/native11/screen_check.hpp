#pragma once

#include "host_dx12/capture/screen_marker.hpp"
#include "host_dx12/native11/frame_model.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <vector>

namespace na::native11 {

// Called on the game's immediate context, under the provider's frame lock.
// The marker is mapped only from a staging copy with DO_NOT_WAIT at Present.
class ScreenCheck {
    using Vote = screen_marker::Vote;
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;

public:
    struct Reader {
        uint64_t output = 0, shader = 0;
        UINT slot = 0, format = 0;
        bool operator==(const Reader&) const = default;
        explicit operator bool() const { return output && shader; }
    };
    struct Mark {
        Texture source, original;
        std::array<D3D11_RECT, 3> rects{};
    };

    void SetScene(Id color, Id depth)
    {
        if (color == color_ && depth == depth_) return;
        color_ = color;
        depth_ = depth;
        verified_ = {};
        rejected_.clear();
        ResetAttempt();
    }

    bool Verified(Id color, Id depth) const { return color_ == color && depth_ == depth && !!verified_; }
    bool Matches(Reader reader) const { return reader == verified_; }
    bool AwaitingSamples() const { return !!candidate_; }
    uint64_t Confirmed() const { return confirmed_; }
    uint64_t Refuted() const { return refuted_; }
    uint64_t Paired() const { return paired_; }
    uint64_t Delivered() const { return delivered_; }

    Mark Begin(ID3D11DeviceContext* context, Reader reader, ID3D11Texture2D* source,
               DXGI_FORMAT view_format, uint64_t frame, uint32_t scene_draws)
    {
        if (verified_ || marked_ || issued_ == screen_marker::kSession ||
            std::find(rejected_.begin(), rejected_.end(), reader) != rejected_.end()) return {};
        if (candidate_ && !(reader == candidate_)) return {};

        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.Width < 8 || desc.Height < 8 || desc.MipLevels != 1 || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) return {};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> immediate;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&immediate)))) return {};
        if (!SameFormat(original_.Get(), desc.Format)) {
            Texture original;
            if (!MakeStrip(context, desc.Format, false, original)) return {};
            original_ = original;
        }

        if (!marker_view_) {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            D3D11_RENDER_TARGET_VIEW_DESC rt{};
            rt.Format = view_format;
            rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateRenderTargetView(source, &rt, &marker_view_))) return {};
        }

        candidate_ = reader;
        if (!issued_) opened_ = frame;
        Mark mark{source, original_, Points(desc.Width, desc.Height)};
        CopyPoints(context, source, original_.Get(), mark.rects);
        const auto value = (issued_ & 1u) ? screen_marker::kHigh : screen_marker::kLow;
        const float rgba[]{value, value, value, 1.f};
        // The marker goes in the source before the draw, so a merely bound but
        // unused SRV cannot earn a vote. Restore the original texels after the draw.
        immediate->ClearView(marker_view_.Get(), rgba, mark.rects.data(), UINT(mark.rects.size()));
        marked_ = true;
        marked_frame_ = frame;
        marked_draws_ = scene_draws;
        ++issued_;
        return mark;
    }

    static void Restore(ID3D11DeviceContext* context, const Mark& mark)
    {
        for (UINT i = 0; i < mark.rects.size(); ++i) {
            const D3D11_BOX box{i, 0, 0, i + 1, 1, 1};
            context->CopySubresourceRegion(mark.source.Get(), 0, mark.rects[i].left, mark.rects[i].top, 0,
                                           mark.original.Get(), 0, &box);
        }
    }

    void NoteDelivery(bool delivered, uint32_t scene_draws)
    {
        if (delivered) {
            delivered_this_frame_ = true;
            delivered_draws_ = scene_draws;
            ++delivered_;
        }
    }

    void Present(ID3D11DeviceContext* context, ID3D11Texture2D* screen, uint64_t frame,
                 uint32_t scene_draws, bool& confirmed, bool& refuted)
    {
        confirmed = refuted = false;
        if (verified_) {
            if (delivered_this_frame_) {
                misses_ = 0;
                if (scene_draws != delivered_draws_) verified_ = {};
            } else if (++misses_ >= 16) verified_ = {};
            delivered_this_frame_ = false;
            if (!verified_) { rejected_.clear(); ResetAttempt(); }
            return;
        }
        if (!candidate_ || !context || !screen) { marked_ = false; return; }
        Collect(context);
        if (marked_ && frame == marked_frame_ && scene_draws == marked_draws_) {
            D3D11_TEXTURE2D_DESC desc{};
            screen->GetDesc(&desc);
            auto slot = std::find_if(slots_.begin(), slots_.end(), [](const auto& s) { return !s.pending; });
            if (slot != slots_.end() && desc.SampleDesc.Count == 1 && Bytes(desc.Format)) {
                if (!SameFormat(slot->image.Get(), desc.Format)) {
                    slot->image.Reset();
                    MakeStrip(context, desc.Format, true, slot->image);
                }
                if (slot->image) {
                    CopyPoints(context, screen, slot->image.Get(), Points(desc.Width, desc.Height));
                    slot->frame = frame;
                    slot->value = (issued_ - 1) & 1u;
                    slot->format = desc.Format;
                    slot->pending = true;
                }
            }
        }
        marked_ = false;
        if (issued_ < screen_marker::kSession && frame - opened_ < 32) return;
        if (std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) { return slot.pending; }) &&
            frame - opened_ < 32) return;
        const auto outcome = votes_.ResultOf();
        if (outcome == Vote::Result::Reached) {
            verified_ = candidate_;
            confirmed = true;
            ++confirmed_;
        } else {
            // Mixed/unavailable samples cannot establish a route. Try the
            // next consumer of this scene rather than weighing a guess.
            if (rejected_.size() >= 16) rejected_.erase(rejected_.begin());
            rejected_.push_back(candidate_);
            refuted = outcome == Vote::Result::Discarded;
            if (refuted) ++refuted_;
        }
        ResetAttempt();
    }

private:
    struct Sample {
        float luma[3]{};
        UINT value = 0;
    };
    struct Slot {
        Texture image;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t frame = 0;
        UINT value = 0;
        bool pending = false;
    };

    static UINT Bytes(DXGI_FORMAT format)
    {
        switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
        default: return 0;
        }
    }

    static bool SameFormat(ID3D11Texture2D* texture, DXGI_FORMAT format)
    {
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        return desc.Format == format;
    }

    static bool MakeStrip(ID3D11DeviceContext* context, DXGI_FORMAT format, bool readback, Texture& out)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 3;
        desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        if (readback) {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        }
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &out));
    }

    static std::array<D3D11_RECT, 3> Points(UINT width, UINT height)
    {
        std::array<D3D11_RECT, 3> points{};
        for (UINT i = 0; i < points.size(); ++i) {
            const LONG x = LONG(width * screen_marker::kAt[i][0]);
            const LONG y = LONG(height * screen_marker::kAt[i][1]);
            points[i] = {x, y, x + 1, y + 1};
        }
        return points;
    }

    static void CopyPoints(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                           ID3D11Texture2D* destination, const std::array<D3D11_RECT, 3>& points)
    {
        for (UINT i = 0; i < points.size(); ++i) {
            const auto& r = points[i];
            const D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
            context->CopySubresourceRegion(destination, 0, i, 0, 0, source, 0, &box);
        }
    }

    void Collect(ID3D11DeviceContext* context)
    {
        for (;;) {
            Slot* next = nullptr;
            for (auto& slot : slots_)
                if (slot.pending && (!next || slot.frame < next->frame)) next = &slot;
            if (!next) return;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = context->Map(next->image.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            next->pending = false;
            if (FAILED(hr)) continue;
            Sample sample{};
            sample.value = next->value;
            const auto* data = static_cast<const uint8_t*>(map.pData);
            for (UINT i = 0; i < 3; ++i) sample.luma[i] = screen_marker::Luma(data + i * Bytes(next->format), next->format);
            context->Unmap(next->image.Get(), 0);
            if (have_previous_) votes_.Add(previous_.value, previous_.luma, sample.value, sample.luma);
            previous_ = sample;
            have_previous_ = true;
            ++paired_;
        }
    }

    void ResetAttempt()
    {
        candidate_ = {};
        slots_ = {};
        original_.Reset();
        marker_view_.Reset();
        issued_ = misses_ = 0;
        votes_ = {};
        marked_ = have_previous_ = delivered_this_frame_ = false;
    }

    Id color_ = 0, depth_ = 0;
    Reader candidate_{}, verified_{};
    std::vector<Reader> rejected_;
    Texture original_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> marker_view_;
    std::array<Slot, 6> slots_{};
    Vote votes_;
    Sample previous_{};
    uint64_t opened_ = 0, marked_frame_ = 0;
    uint32_t issued_ = 0, marked_draws_ = 0, misses_ = 0, delivered_draws_ = 0;
    bool marked_ = false, have_previous_ = false, delivered_this_frame_ = false;
    uint64_t confirmed_ = 0, refuted_ = 0, paired_ = 0, delivered_ = 0;
};

} // namespace na::native11
