#pragma once

#include "host_dx12/capture/screen_marker.hpp"
#include "core/referee.hpp"
#include "host_dx12/native11/frame_model.hpp"
#include "host_dx12/j2/post_entry.hpp"

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
    static bool FirstIsHigh(UINT sample, UINT pair)
    {
        return (screen_marker::kPatterns[sample] & (1u << pair)) != 0;
    }

public:
    struct Reader {
        uint64_t output = 0, shader = 0;
        UINT slot = 0, format = 0;
        uint64_t color = 0, depth = 0, writer = 0;
        uint32_t context = 0;
        bool operator==(const Reader& other) const
        {
            return output == other.output && shader == other.shader && slot == other.slot && format == other.format &&
                   color == other.color && depth == other.depth && writer == other.writer && context == other.context;
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
        steering_ = false;
        consecutive_proven_ = 0;
        adaptive_ = std::find(adaptive_scenes_.begin(), adaptive_scenes_.end(), std::pair{color, depth}) != adaptive_scenes_.end();
        legacy_rejected_.clear();
        ordinary_gaps_ = 0;
        ordinary_this_frame_ = false;
        ResetAttempt();
        ResetFrame();
    }

    void ResetPlacement()
    {
        SetScene(0, 0);
        adaptive_ = false;
        adaptive_scenes_.clear();
        routes_.clear();
        rejected_.clear();
        entries_.Clear();
        ResetFrame();
    }

    bool Adaptive() const { return adaptive_; }
    bool Active() const { return color_ || depth_ || !adaptive_scenes_.empty(); }

    void NoteOrdinalPixel(uint32_t draws) { ordinal_draws_ = draws; }
    void NoteActualPixel(uint32_t draws) { reader_after_draws_ = draws; }

    void Write(uint64_t writer)
    {
        ++epoch_;
        writer_ = writer;
        entries_since_write_ = 0;
        if (!adaptive_) return;
        na::j2::ColorEvent entry{};
        entry.write = true;
        entry.producer = writer;
        if (!sequence_.empty() && sequence_.back().write) sequence_.back() = entry;
        else Append(entry);
    }

    // Use the DX12 role profile on executed reads. The input binding slot is
    // the DX11 equivalent of the consumer's resource layout. Consecutive
    // writes are collapsed: only the producer of the newest content matters.
    bool Learn(Reader& reader, UINT width, UINT height)
    {
        if (!epoch_ || !reader.color || !reader.depth || truncated_) return false;
        na::j2::InsertionEvent event;
        event.op = na::j2::Op::GraphicsEntry;
        event.pipeline = reader.shader;
        event.root = (reader.color << 8) | (uint64_t(reader.slot) + 1);
        event.destination = reader.output;
        event.dst_w = width;
        event.dst_h = height;
        event.dst_fmt = reader.format;
        event.producer = writer_;
        event.context = na::j2::EntryContext(true, ++entries_since_write_);
        na::j2::ColorEvent entry{};
        entry.op = event.op;
        entry.raw = event;
        Append(entry);
        // A proven base role can follow different final objects. Binding its
        // proof to the last object's shader would discard that proof every time
        // the last object changes. Only contextual acceptance requires both.
        if (entries_.AcceptedByContext(event)) {
            if (!writer_) return false;
            reader.writer = writer_;
            reader.context = event.context;
        }
        return !truncated_ && entries_.Accepts(event);
    }

    bool Verified(Id color, Id depth) const { return color_ == color && depth_ == depth && !!verified_; }
    bool Matches(Reader reader) const { return reader == verified_; }
    uint64_t Proof() const { return adaptive_ ? frame_proof_ : verified_ ? proof_ : 0; }

    bool ReservesPixelRead() const { return adaptive_ && steering_; }

    bool Select(Reader reader, bool available)
    {
        if (!adaptive_) return Matches(reader);
        for (const auto& route : routes_) {
            if (reader == route.reader) {
                seen_proven_epoch_ = epoch_;
                if (available) {
                    frame_proof_ = route.proof;
                    delivery_epoch_ = epoch_;
                }
                return true;
            }
        }
        return false;
    }

    bool RejectProof(uint64_t proof, uint64_t frame)
    {
        if (!proof) return false;
        const auto match = std::find_if(routes_.begin(), routes_.end(),
            [proof](const Route& route) { return route.proof == proof; });
        if (match != routes_.end()) {
            Defer(match->reader, frame);
            routes_.erase(match);
            if (routes_.empty()) {
                steering_ = false;
                consecutive_proven_ = 0;
            }
            return true;
        }
        if (!verified_ || proof != proof_) return false;
        RetireVerified(frame);
        return true;
    }
    bool MayInspect() const
    {
        return adaptive_ || verified_ || (!ordinary_this_frame_ && ordinary_gaps_ >= kOrdinaryGrace);
    }
    bool AwaitingSamples() const { return !!candidate_; }
    void CancelChallenge() { if (candidate_) ResetAttempt(); }
    // A rotating scene owner supplies its own completed-frame qualification.
    // Reuse the same asynchronous reader challenge and independent proof IDs.
    void IndependentReaders() { adaptive_ = true; }
    uint64_t Confirmed() const { return confirmed_; }
    uint64_t Refuted() const { return refuted_; }
    uint64_t Paired() const { return paired_; }
    uint64_t Delivered() const { return delivered_; }

    Mark Begin(ID3D11DeviceContext* context, Reader reader, ID3D11Texture2D* source,
               DXGI_FORMAT view_format, uint64_t frame, uint32_t scene_draws)
    {
        // A rare branch may disappear while another needs qualification.
        // Keep known routes and only abandon the incomplete sampling attempt.
        if (adaptive_ && candidate_ && !marked_ && issued_ < screen_marker::kSession &&
            frame - last_marked_ >= 2 * screen_marker::kSession) ResetAttempt();
        const auto& rejected = adaptive_ ? rejected_ : legacy_rejected_;
        if (verified_ || marked_ || (adaptive_ && frame_proof_) ||
            std::any_of(rejected.begin(), rejected.end(), [&](const Retry& retry) {
                return retry.reader == reader && frame < retry.after;
            })) return {};
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
        marker_width_ = desc.Width;
        marker_height_ = desc.Height;
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
        marked_epoch_ = epoch_;
        last_marked_ = frame;
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

    void PresentResult(uint64_t frame, bool accepted)
    {
        for (auto& slot : slots_) {
            if (!slot.pending || slot.frame != frame) continue;
            if (accepted) slot.awaiting_result = false;
            else slot.pending = false;
        }
    }

    const char* Present(ID3D11DeviceContext* context, ID3D11Texture2D* screen, uint64_t frame,
                        uint32_t scene_draws, bool attempted, bool await_result = false, bool centered_only = false)
    {
        const auto finish = [&](const char* outcome) {
            ResetFrame();
            return outcome;
        };
        const bool ordinary_completed = ordinary_this_frame_ && ordinary_draws_ == scene_draws;
        ordinary_this_frame_ = false;
        const bool early = ordinal_draws_ && scene_draws > ordinal_draws_;
        const bool missed_reader = !attempted && scene_draws && reader_after_draws_ == scene_draws;
        if (!adaptive_ && (early || missed_reader)) {
            adaptive_ = true;
            if (adaptive_scenes_.size() >= 16) adaptive_scenes_.erase(adaptive_scenes_.begin());
            adaptive_scenes_.emplace_back(color_, depth_);
            verified_ = {};
            legacy_rejected_.clear();
            ResetAttempt();
            return finish(early ? "draw_order_changed" : "draw_order_missed");
        }
        if (adaptive_) {
            // Qualifying a replacement must never silence the ordinary evaluator.
            // A missing or overwritten reader releases the next frame immediately.
            // Re-arm only after consecutive sightings on complete scene frames, so
            // an intermittent proof cannot repeatedly take the other order offline.
            if (scene_draws && !truncated_ && seen_proven_epoch_ && seen_proven_epoch_ == epoch_ &&
                (!frame_proof_ || delivered_this_frame_)) {
                if (consecutive_proven_ < 3) ++consecutive_proven_;
                steering_ = consecutive_proven_ >= 3;
            } else {
                steering_ = false;
                consecutive_proven_ = 0;
            }
            if (!truncated_) entries_.Observe(sequence_, true, frame);
            if (frame_proof_ && delivery_epoch_ != epoch_) {
                RejectProof(frame_proof_, frame);
                if (marked_ && (marked_epoch_ != epoch_ || truncated_)) ResetAttempt();
                else marked_ = false;
                return finish("adaptive_later_scene_write");
            }
            // The candidate may be absent on a different draw order. Only
            // its own sampled frames can qualify or disprove it.
            if (marked_ && (marked_epoch_ != epoch_ || truncated_)) {
                Defer(candidate_, frame);
                ResetAttempt();
                return finish("adaptive_candidate_mid_scene");
            }
        }
        if (!adaptive_ && ordinary_completed) {
            ordinary_gaps_ = 0;
            if (candidate_ || verified_) {
                verified_ = {};
                ResetAttempt();
                return finish("ordinary_restored");
            }
            return finish(nullptr);
        }
        if (!adaptive_ && scene_draws && ordinary_gaps_ < kOrdinaryGrace) ++ordinary_gaps_;

        if (verified_) {
            // An eligible success ages a previous miss instead of erasing it. A
            // reader delivering every other frame must lose control promptly.
            const bool missed = !delivered_this_frame_ && scene_draws;
            const bool overwritten = delivered_this_frame_ && scene_draws != delivered_draws_;
            delivered_this_frame_ = false;
            if (overwritten || (missed && recent_miss_)) {
                RetireVerified(frame);
                return finish("lost_coverage");
            }
            if (missed) recent_miss_ = screen_marker::kSession;
            else if (scene_draws && recent_miss_) --recent_miss_;
            return finish(nullptr);
        }
        if (!candidate_) return finish(nullptr);
        // Only a reader seen at the right point on every scene frame can earn
        // control. The staging samples may finish later than those draws.
        if (!adaptive_ && scene_draws && (!seen_ || reader_draws_ != scene_draws)) {
            Defer(candidate_, frame);
            ResetAttempt();
            return finish("intermittent");
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
                    slot->layouts = na::referee::PaddedLayouts(marker_width_, marker_height_, desc.Width, desc.Height);
                    for (UINT layout = 0; layout < slot->layouts; ++layout) {
                        const UINT dx = desc.Width - marker_width_, dy = desc.Height - marker_height_;
                        const LONG x = LONG(dx / 2 + layout % (1 + (dx & 1)));
                        const LONG y = LONG(dy / 2 + layout / (1 + (dx & 1)));
                        auto points = Points(marker_width_, marker_height_);
                        for (auto& r : points) { r.left += x; r.right += x; r.top += y; r.bottom += y; }
                        CopyPoints(context, screen, slot->image.Get(), points, (layout + 1) * kPoints);
                    }
                    slot->frame = frame;
                    slot->sample = issued_ - 1;
                    slot->format = desc.Format;
                    slot->pending = true;
                    slot->awaiting_result = await_result;
                }
            }
        }
        marked_ = seen_ = false;
        const uint64_t deadline = adaptive_ ? 96 : 32;
        if (frame - opened_ < deadline && (issued_ < screen_marker::kSession ||
            std::any_of(slots_.begin(), slots_.end(), [](const auto& slot) { return slot.pending; }))) return finish(nullptr);
        const bool sampled_all = samples_ == screen_marker::kSession;
        bool confirmed = false, any_response = false;
        bool refuted = sampled_all;
        for (size_t i = centered_only ? 1 : 0; i < votes_.size(); ++i) {
            const auto& layout = votes_[i];
            confirmed |= sampled_all && (layout.follow == samples_ || layout.invert == samples_);
            any_response |= layout.follow || layout.invert;
            if (layout.seen && layout.flat != samples_) refuted = false;
        }
        const bool intermittent = sampled_all && any_response && !confirmed;
        if (confirmed) {
            if (adaptive_) {
                if (routes_.size() >= 24) routes_.erase(routes_.begin());
                routes_.push_back({candidate_, ++next_proof_});
                consecutive_proven_ = 3;
                steering_ = true; // the completed screen challenge is direct positive evidence
            } else {
                verified_ = candidate_;
                proof_ = ++next_proof_;
            }
            ++confirmed_;
        } else {
            // A timeout or a display change is not a permanent property of a
            // shader and target. Prove the reader again after a quiet interval.
            Defer(candidate_, frame, refuted ? 1024 : 128);
            if (refuted) ++refuted_;
        }
        ResetAttempt();
        if (confirmed) return finish(adaptive_ ? "adaptive_confirmed" : "confirmed");
        if (refuted) return finish("refuted");
        return finish(intermittent ? "intermittent_screen" : "inconclusive");
    }

private:
    struct Route { Reader reader; uint64_t proof; };
    struct Retry { Reader reader; uint64_t after = 0, delay = 0; };

    void Append(const na::j2::ColorEvent& event)
    {
        if (sequence_.size() >= 64) truncated_ = true;
        else sequence_.push_back(event);
    }

    void ResetFrame()
    {
        sequence_.clear();
        entries_since_write_ = ordinal_draws_ = reader_after_draws_ = 0;
        writer_ = frame_proof_ = seen_proven_epoch_ = epoch_ = 0;
        delivered_this_frame_ = truncated_ = false;
    }

    struct Slot {
        Texture image;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT layouts = 0;
        uint64_t frame = 0;
        UINT sample = 0;
        bool pending = false, awaiting_result = false;
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
        desc.Width = readback ? kPoints * 5 : kPoints;
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
                           ID3D11Texture2D* destination, const ProbeRects& points, UINT offset = 0)
    {
        for (UINT i = 0; i < points.size(); ++i) {
            const auto& r = points[i];
            const D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
            context->CopySubresourceRegion(destination, 0, offset + i, 0, 0, source, 0, &box);
        }
    }

    void Collect(ID3D11DeviceContext* context)
    {
        for (;;) {
            Slot* next = nullptr;
            for (auto& slot : slots_)
                if (slot.pending && (!next || slot.frame < next->frame)) next = &slot;
            if (!next || next->awaiting_result) return;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = context->Map(next->image.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            next->pending = false;
            if (FAILED(hr)) continue;
            const auto* data = static_cast<const uint8_t*>(map.pData);
            const UINT stride = Bytes(next->format);
            for (UINT i = 0; i <= next->layouts; ++i) {
                UINT follows = 0, inverts = 0, flat = 0;
                for (UINT pair = 0; pair < kPairs; ++pair) {
                    const auto* point = data + (i * kPoints + 2 * pair) * stride;
                    const float first = screen_marker::Luma(point, next->format);
                    const float second = screen_marker::Luma(point + stride, next->format);
                    const float contrast = FirstIsHigh(next->sample, pair) ? first - second : second - first;
                    if (!std::isfinite(contrast)) continue;
                    follows += contrast > .02f;
                    inverts += contrast < -.02f;
                    flat += std::abs(contrast) <= .02f;
                }
                auto& v = votes_[i];
                ++v.seen;
                v.follow += follows >= 2;
                v.invert += inverts >= 2;
                v.flat += flat >= 2;
            }
            context->Unmap(next->image.Get(), 0);
            ++samples_;
            ++paired_;
        }
    }

    void Defer(Reader reader, uint64_t frame, uint64_t initial_delay = 128)
    {
        auto& rejected = adaptive_ ? rejected_ : legacy_rejected_;
        const auto existing = std::find_if(rejected.begin(), rejected.end(),
            [&](const Retry& retry) { return retry.reader == reader; });
        const uint64_t delay = existing == rejected.end() ? initial_delay :
            (std::clamp)(existing->delay * 2, initial_delay, uint64_t(4096));
        if (existing != rejected.end()) rejected.erase(existing);
        else if (rejected.size() >= 24) rejected.erase(rejected.begin());
        rejected.push_back({reader, frame + delay, delay});
    }

    void RetireVerified(uint64_t frame)
    {
        Defer(verified_, frame);
        verified_ = {};
        ordinary_gaps_ = 0;
        ResetAttempt();
    }

    void ResetAttempt()
    {
        candidate_ = {};
        slots_ = {};
        original_.Reset();
        marker_view_.Reset();
        issued_ = recent_miss_ = 0;
        samples_ = 0;
        votes_ = {};
        marked_ = seen_ = delivered_this_frame_ = false;
    }

    na::j2::PostEntryProfile entries_;
    std::vector<na::j2::ColorEvent> sequence_;
    std::vector<Route> routes_;
    std::vector<std::pair<Id, Id>> adaptive_scenes_;
    bool adaptive_ = false, truncated_ = false, steering_ = false;
    unsigned consecutive_proven_ = 0;
    uint64_t seen_proven_epoch_ = 0;
    uint32_t ordinal_draws_ = 0, reader_after_draws_ = 0, entries_since_write_ = 0;
    uint64_t writer_ = 0, epoch_ = 0, marked_epoch_ = 0, delivery_epoch_ = 0, frame_proof_ = 0;

    Id color_ = 0, depth_ = 0;
    Reader candidate_{}, verified_{};
    std::vector<Retry> rejected_, legacy_rejected_;
    Texture original_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> marker_view_;
    std::array<Slot, screen_marker::kSession> slots_{};
    UINT samples_ = 0, marker_width_ = 0, marker_height_ = 0;
    struct LayoutVote { UINT seen = 0, follow = 0, invert = 0, flat = 0; };
    std::array<LayoutVote, 5> votes_{};
    static constexpr uint32_t kOrdinaryGrace = 4;
    uint64_t opened_ = 0, last_marked_ = 0;
    static inline uint64_t next_proof_ = 0; // all provider screen state is under the frame lock
    uint64_t proof_ = 0;
    uint32_t issued_ = 0, reader_draws_ = 0, recent_miss_ = 0, delivered_draws_ = 0;
    uint32_t ordinary_gaps_ = 0, ordinary_draws_ = 0;
    bool marked_ = false, seen_ = false;
    bool delivered_this_frame_ = false, ordinary_this_frame_ = false;
    uint64_t confirmed_ = 0, refuted_ = 0, paired_ = 0, delivered_ = 0;
};

} // namespace na::native11
