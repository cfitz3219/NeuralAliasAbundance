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
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    static constexpr UINT kPairs = 3, kPoints = kPairs * 2;
    using ProbeRects = std::array<D3D11_RECT, kPoints>;

    // Every location changes polarity on both frame parities. A fixed edge or
    // an alternating post-process cannot substitute for the challenge pattern.
    static constexpr std::array<UINT, screen_marker::kSession> kPatterns{0, 7, 5, 2, 3, 4, 6, 1};

    static bool FirstIsHigh(UINT sample, UINT pair)
    {
        return (kPatterns[sample] & (1u << pair)) != 0;
    }

public:
    struct Reader {
        uint64_t output = 0, shader = 0;
        UINT slot = 0, format = 0;
        bool operator==(const Reader& other) const
        {
            return output == other.output && shader == other.shader && slot == other.slot && format == other.format;
        }
        explicit operator bool() const { return output && shader; }
    };
    struct Mark {
        Texture source, original;
        ProbeRects rects{};
    };

    void SetScene(Id color, Id depth)
    {
        if (color == color_ && depth == depth_) return;
        color_ = color;
        depth_ = depth;
        verified_ = {};
        rejected_.clear();
        ordinary_gaps_ = 0;
        ordinary_this_frame_ = false;
        ResetAttempt();
    }

    bool Verified(Id color, Id depth) const { return color_ == color && depth_ == depth && !!verified_; }
    bool Matches(Reader reader) const { return reader == verified_; }
    bool MayInspect() const
    {
        return verified_ || (!ordinary_this_frame_ && ordinary_gaps_ >= kOrdinaryGrace);
    }
    bool AwaitingSamples() const { return !!candidate_; }
    uint64_t Confirmed() const { return confirmed_; }
    uint64_t Refuted() const { return refuted_; }
    uint64_t Paired() const { return paired_; }
    uint64_t Delivered() const { return delivered_; }

    Mark Begin(ID3D11DeviceContext* context, Reader reader, ID3D11Texture2D* source,
               DXGI_FORMAT view_format, uint64_t frame, uint32_t scene_draws)
    {
        if (verified_ || marked_ ||
            std::find(rejected_.begin(), rejected_.end(), reader) != rejected_.end()) return {};
        if (candidate_ && !(reader == candidate_)) return {};
        if (candidate_ && !seen_) {
            seen_ = true;
            reader_draws_ = scene_draws;
        }
        if (issued_ == screen_marker::kSession) return {};

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
        std::array<D3D11_RECT, kPairs> high{}, low{};
        for (UINT pair = 0; pair < kPairs; ++pair) {
            const UINT first = 2 * pair;
            const bool high_first = FirstIsHigh(issued_, pair);
            high[pair] = mark.rects[first + !high_first];
            low[pair] = mark.rects[first + high_first];
        }
        constexpr float high_rgba[]{screen_marker::kHigh, screen_marker::kHigh, screen_marker::kHigh, 1.f};
        constexpr float low_rgba[]{screen_marker::kLow, screen_marker::kLow, screen_marker::kLow, 1.f};
        // Write the pair into the source only around this reader. Both sides
        // must be recognizable in the same presented image.
        immediate->ClearView(marker_view_.Get(), high_rgba, high.data(), kPairs);
        immediate->ClearView(marker_view_.Get(), low_rgba, low.data(), kPairs);
        marked_ = seen_ = true;
        reader_draws_ = scene_draws;
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

    void NoteDelivery(bool by_verified_reader, uint32_t scene_draws)
    {
        if (by_verified_reader) {
            delivered_this_frame_ = true;
            delivered_draws_ = scene_draws;
            ++delivered_;
        } else {
            ordinary_this_frame_ = true;
            ordinary_draws_ = scene_draws;
        }
    }

    const char* Present(ID3D11DeviceContext* context, ID3D11Texture2D* screen, uint64_t frame,
                        uint32_t scene_draws)
    {
        const bool ordinary_completed = ordinary_this_frame_ && ordinary_draws_ == scene_draws;
        ordinary_this_frame_ = false;
        if (ordinary_completed) {
            ordinary_gaps_ = 0;
            if (candidate_ || verified_) {
                verified_ = {};
                ResetAttempt();
                return "ordinary_restored";
            }
            return nullptr;
        }
        if (scene_draws && ordinary_gaps_ < kOrdinaryGrace) ++ordinary_gaps_;

        if (verified_) {
            // An eligible success ages a previous miss instead of erasing it. A
            // reader delivering every other frame must lose control promptly.
            const bool missed = !delivered_this_frame_ && scene_draws;
            const bool overwritten = delivered_this_frame_ && scene_draws != delivered_draws_;
            delivered_this_frame_ = false;
            if (overwritten || (missed && recent_miss_)) {
                Reject(verified_);
                verified_ = {};
                ordinary_gaps_ = 0;
                ResetAttempt();
                return "lost_coverage";
            }
            if (missed) recent_miss_ = screen_marker::kSession;
            else if (scene_draws && recent_miss_) --recent_miss_;
            return nullptr;
        }
        if (!candidate_) return nullptr;
        // Only a reader seen at the right point on every scene frame can earn
        // control. The staging samples may finish later than those draws.
        if (scene_draws && (!seen_ || reader_draws_ != scene_draws)) {
            Reject(candidate_);
            ResetAttempt();
            return "intermittent";
        }
        if (context) Collect(context);
        if (marked_ && scene_draws && context && screen) {
            D3D11_TEXTURE2D_DESC desc{};
            screen->GetDesc(&desc);
            auto slot = std::find_if(slots_.begin(), slots_.end(), [](const auto& s) { return !s.pending; });
            if (slot != slots_.end() && desc.Width >= 8 && desc.Height >= 8 &&
                desc.SampleDesc.Count == 1 && Bytes(desc.Format)) {
                if (!SameFormat(slot->image.Get(), desc.Format)) {
                    slot->image.Reset();
                    MakeStrip(context, desc.Format, true, slot->image);
                }
                if (slot->image) {
                    CopyPoints(context, screen, slot->image.Get(), Points(desc.Width, desc.Height));
                    slot->frame = frame;
                    slot->sample = issued_ - 1;
                    slot->format = desc.Format;
                    slot->pending = true;
                }
            }
        }
        marked_ = seen_ = false;
        if (frame - opened_ < 32 && (issued_ < screen_marker::kSession ||
            std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) { return slot.pending; }))) return nullptr;
        const bool sampled_all = samples_ == screen_marker::kSession;
        const bool confirmed = sampled_all && (follow_ == samples_ || invert_ == samples_);
        const bool refuted = sampled_all && flat_ == samples_;
        const bool intermittent = sampled_all && (follow_ || invert_) && !confirmed;
        if (confirmed) {
            verified_ = candidate_;
            ++confirmed_;
        } else {
            Reject(candidate_);
            if (refuted) ++refuted_;
        }
        ResetAttempt();
        if (confirmed) return "confirmed";
        if (refuted) return "refuted";
        return intermittent ? "intermittent_screen" : "inconclusive";
    }

private:
    struct Slot {
        Texture image;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t frame = 0;
        UINT sample = 0;
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
        desc.Width = kPoints;
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

    static ProbeRects Points(UINT width, UINT height)
    {
        ProbeRects points{};
        const LONG spacing = LONG(std::max(1u, width / 512u));
        for (UINT i = 0; i < kPairs; ++i) {
            const LONG x = LONG(width * screen_marker::kAt[i][0]);
            const LONG y = LONG(height * screen_marker::kAt[i][1]);
            points[2 * i] = {x, y, x + 1, y + 1};
            points[2 * i + 1] = {x + spacing, y, x + spacing + 1, y + 1};
        }
        return points;
    }

    static void CopyPoints(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                           ID3D11Texture2D* destination, const ProbeRects& points)
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
            const auto* data = static_cast<const uint8_t*>(map.pData);
            const UINT stride = Bytes(next->format);
            UINT follows = 0, inverts = 0, flat = 0;
            for (UINT pair = 0; pair < kPairs; ++pair) {
                const float first = screen_marker::Luma(data + 2 * pair * stride, next->format);
                const float second = screen_marker::Luma(data + (2 * pair + 1) * stride, next->format);
                const float contrast = FirstIsHigh(next->sample, pair) ? first - second : second - first;
                if (!std::isfinite(contrast)) continue;
                follows += contrast > .02f;
                inverts += contrast < -.02f;
                flat += std::abs(contrast) <= .02f;
            }
            context->Unmap(next->image.Get(), 0);
            follow_ += follows >= 2;
            invert_ += inverts >= 2;
            flat_ += flat >= 2;
            ++samples_;
            ++paired_;
        }
    }

    void Reject(Reader reader)
    {
        if (rejected_.size() >= 16) rejected_.erase(rejected_.begin());
        rejected_.push_back(reader);
    }

    void ResetAttempt()
    {
        candidate_ = {};
        slots_ = {};
        original_.Reset();
        marker_view_.Reset();
        issued_ = recent_miss_ = 0;
        samples_ = follow_ = invert_ = flat_ = 0;
        marked_ = seen_ = delivered_this_frame_ = false;
    }

    Id color_ = 0, depth_ = 0;
    Reader candidate_{}, verified_{};
    std::vector<Reader> rejected_;
    Texture original_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> marker_view_;
    std::array<Slot, screen_marker::kSession> slots_{};
    UINT samples_ = 0, follow_ = 0, invert_ = 0, flat_ = 0;
    static constexpr uint32_t kOrdinaryGrace = 4;
    uint64_t opened_ = 0;
    uint32_t issued_ = 0, reader_draws_ = 0, recent_miss_ = 0, delivered_draws_ = 0;
    uint32_t ordinary_gaps_ = 0, ordinary_draws_ = 0;
    bool marked_ = false, seen_ = false;
    bool delivered_this_frame_ = false, ordinary_this_frame_ = false;
    uint64_t confirmed_ = 0, refuted_ = 0, paired_ = 0, delivered_ = 0;
};

} // namespace na::native11
