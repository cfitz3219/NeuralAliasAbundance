#include "host_dx12/adapters/mode_control.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/adapters/tsushima/apply.hpp"
#include "host_dx12/adapters/unreal/screen_percentage.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/sl_boundary/sl_dlss_mode.hpp"

namespace na::mode {
namespace {

std::mutex g_m;
std::mutex g_actions; // serialize request admission with cancellation, never held while waiting
Status g_status;
Request g_pending;
bool g_has_pending = false;
bool g_running=false;
std::atomic<uint64_t> g_generation{0};
std::atomic<bool> g_started{false};
HANDLE g_wake = nullptr;
uint64_t g_next_id = 0;

void set_reason(const char* text)
{
    std::lock_guard<std::mutex> lock(g_m);
    strncpy_s(g_status.reason, text, _TRUNCATE);
}

bool cancelled(uint64_t generation) { return g_generation.load(std::memory_order_acquire) != generation; }

void remember(Layer layer)
{
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_status.learned = layer;
    }

}

enum class Outcome { Confirmed, CannotAct, Failed, Cancelled };

// Waits on the shared size-confirmation monitor for request `id`.
Outcome wait_confirmation(uint64_t id, uint64_t generation, uint32_t timeout_ms)
{
    const ULONGLONG end = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < end) {
        if (cancelled(generation)) return Outcome::Cancelled;
        const na::ngx::SizeConfirmation c = na::ngx::GetSizeConfirmation();
        if (c.id != id) return Outcome::Failed;
        if (c.state == 3) return Outcome::Confirmed;
        if (c.state == 4) return c.creations ? Outcome::Failed : Outcome::CannotAct;
        Sleep(50);
    }
    return Outcome::CannotAct;
}

// Streamline: request the mode; it can act only if the game asks Streamline
// for its size while we wait. Failed attempts are retired before fallback.
Outcome try_streamline(const Request& r, uint64_t generation)
{
    na::slb::dlss::ModeRequest req;
    req.out_w=r.out_w;req.out_h=r.out_h;req.render_w=r.render_w;req.render_h=r.render_h;
    req.own_mode = true;
    req.sl_mode = na::slb::dlss::SlModeForDlssMode(r.mode);
    req.pin_bounds = true;
    req.own_preset = r.preset >= 1 && r.preset <= 15;
    req.preset = req.own_preset ? static_cast<uint32_t>(r.preset) : 0;
    std::unique_lock<std::mutex> action(g_actions);
    if(cancelled(generation))return Outcome::Cancelled;
    const uint64_t id=na::ngx::ArmLiveSizeConfirmation(r.render_w,r.render_h,r.out_w,r.out_h,30,8000);
    req.confirmation=id;
    na::ngx::MarkSizeConfirmationRestored(id);
    na::slb::dlss::SetModeRequest(req);
    action.unlock();
    set_reason("Asking the game's Streamline DLSS for the selected mode...");
    const ULONGLONG end = GetTickCount64() + 1500;
    na::slb::dlss::ModeState st{};
    while (GetTickCount64() < end) {
        if (cancelled(generation)) return Outcome::Cancelled;
        st = na::slb::dlss::GetModeState();
        if (st.size_followed) break; // the game asked for its real output size and got the selected mode's size
        Sleep(10);
    }
    if (!st.size_followed) return Outcome::CannotAct; // the game did not ask again on its own
    return wait_confirmation(id, generation, 7500);
}

Outcome try_unreal(const Request& r, uint64_t generation)
{
    {
        std::lock_guard<std::mutex> action(g_actions);
        if(cancelled(generation))return Outcome::Cancelled;
        if (!na::unreal::RequestScreenPercentage(r.unreal_percent, r.out_w, r.out_h)) return Outcome::CannotAct;
    }
    set_reason("Changing the game's resolution setting (r.ScreenPercentage)...");
    const ULONGLONG end = GetTickCount64() + 10000;
    while (GetTickCount64() < end) {
        if (cancelled(generation)) {
            na::unreal::CancelScreenPercentage();
            return Outcome::Cancelled;
        }
        const na::unreal::SpStatus s = na::unreal::GetScreenPercentageStatus();
        switch (s.state) {
        case na::unreal::SpState::Confirmed: return Outcome::Confirmed;
        case na::unreal::SpState::NotApplied: return Outcome::Failed;
        case na::unreal::SpState::Refused: return Outcome::CannotAct;
        case na::unreal::SpState::GameChanged: return Outcome::Failed;
        case na::unreal::SpState::Cancelled: return Outcome::Cancelled;
        default: break;
        }
        Sleep(50);
    }
    na::unreal::HandBackScreenPercentage();
    return Outcome::Failed;
}

Outcome try_exact(const Request& r, uint64_t generation)
{
    const na::tlou1::Choice choice{r.mode, r.render_w, r.render_h, r.preset};
    const bool ghost = na::tsushima::GetApplyStatus().available;
    const bool ok = ghost ? na::tsushima::RequestApply(choice) : na::tlou1::RequestApply(choice);
    if (!ok) return Outcome::CannotAct;
    set_reason("Using this game's own graphics-settings update...");
    const ULONGLONG end = GetTickCount64() + 20000;
    while (GetTickCount64() < end) {
        if (cancelled(generation)) return Outcome::Cancelled;
        const na::tlou1::ApplyStatus s = ghost ? na::tsushima::GetApplyStatus() : na::tlou1::GetApplyStatus();
        if (s.stage == na::tlou1::Stage::Confirmed) return Outcome::Confirmed;
        if (s.stage == na::tlou1::Stage::Failed) return Outcome::Failed;
        if (s.stage == na::tlou1::Stage::Cancelled) return Outcome::Cancelled;
        Sleep(50);
    }
    return Outcome::CannotAct;
}

Outcome try_rebuild(const Request& r, uint64_t generation)
{
    const uint32_t attempts = na::overlay::GetPictureRebuild().attempts;
    if (!na::overlay::RequestPictureRebuild(r.render_w, r.render_h)) return Outcome::CannotAct;
    set_reason("Asking the game to rebuild its picture...");
    const ULONGLONG end = GetTickCount64() + 12000;
    while (GetTickCount64() < end) {
        if (cancelled(generation)) {
            na::ngx::CancelSizeConfirmation();
            return Outcome::Cancelled;
        }
        const na::overlay::PictureRebuild pr = na::overlay::GetPictureRebuild();
        if (pr.state == 4) return Outcome::CannotAct;
        if (pr.attempts != attempts && pr.state == 3 && pr.confirm_id) {
            const na::ngx::SizeConfirmation c = na::ngx::GetSizeConfirmation();
            if (c.id == pr.confirm_id && c.state == 3) return Outcome::Confirmed;
            if (c.id == pr.confirm_id && c.state == 4) return Outcome::CannotAct;
        }
        Sleep(50);
    }
    return Outcome::CannotAct;
}

void run(const Request& r, uint64_t generation)
{
    Layer order[static_cast<uint32_t>(Layer::Count)];
    Layer learned;
    {
        std::lock_guard<std::mutex> lock(g_m);
        learned = g_status.learned;
    }
    const uint32_t available = AvailableMask();
    const uint32_t n = PlanOrder(available, learned, order, static_cast<uint32_t>(Layer::Count));
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_status.available_mask = available;
        g_status.tried_mask = 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if(cancelled(generation))break;
        const Layer layer = order[i];
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_status.layer = layer;
            g_status.tried_mask |= 1u << static_cast<uint32_t>(layer);
        }
        Outcome o = Outcome::CannotAct;
        switch (layer) {
        case Layer::Streamline: o = try_streamline(r, generation); break;
        case Layer::Unreal: o = try_unreal(r, generation); break;
        case Layer::ExactBuild: o = try_exact(r, generation); break;
        case Layer::Rebuild: o = try_rebuild(r, generation); break;
        default: break;
        }
        if(cancelled(generation))o=Outcome::Cancelled;
        if (o == Outcome::Cancelled) {
            na::slb::dlss::SetModeRequest({});
            na::unreal::CancelScreenPercentage();
            std::lock_guard<std::mutex> lock(g_m);
            g_status.state = State::Cancelled;
            strncpy_s(g_status.reason, "Selection changed; choose Apply again.", _TRUNCATE);
            return;
        }
        if (o == Outcome::Confirmed) {
            remember(layer);
            const na::ngx::SizeConfirmation c = na::ngx::GetSizeConfirmation();
            char why[220];
            if (c.last_w && c.last_h)
                snprintf(why, sizeof(why), "Applied through %s: the game renders %ux%u for the selected mode.", LayerName(layer), c.last_w, c.last_h);
            else
                snprintf(why, sizeof(why), "Applied through %s: the game renders at the selected mode.", LayerName(layer));
            std::lock_guard<std::mutex> lock(g_m);
            if(cancelled(generation))return;
            g_status.state = State::Confirmed;
            strncpy_s(g_status.reason, why, _TRUNCATE);
            return;
        }
        // Retire this attempt before trying another engine path. A persistent
        // API constraint is only retained for a confirmed owner.
        na::slb::dlss::SetModeRequest({});
        na::unreal::CancelScreenPercentage();
        na::ngx::CancelSizeConfirmation();
        if(o==Outcome::Failed)break;
    }
    na::slb::dlss::SetModeRequest({});
    std::lock_guard<std::mutex> lock(g_m);
    if(cancelled(generation))return;
    g_status.state = State::Failed;
    strncpy_s(g_status.reason,
              n ? "None of the available methods changed this game's render size while it runs. The requested live change was not applied."
                : "No method to change this game's render size is available yet (DLSS not started, or not supported here).",
              _TRUNCATE);
}

DWORD WINAPI worker(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Request r;
        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_has_pending) continue;
            g_running=true;
            r = g_pending;
            g_has_pending = false;
            generation = g_generation.load(std::memory_order_acquire);
        }
        run(r, generation);
        {std::lock_guard<std::mutex> lock(g_m);g_running=false;}
    }
}

} // namespace

const char* LayerName(Layer layer)
{
    switch (layer) {
    case Layer::Streamline: return "Streamline DLSS";
    case Layer::Unreal: return "the Unreal resolution setting";
    case Layer::ExactBuild: return "this game's graphics settings";
    case Layer::Rebuild: return "a picture rebuild";
    default: return "none";
    }
}

uint32_t AvailableMask()
{
    uint32_t m = 0;
    const na::slb::dlss::ModeState sl = na::slb::dlss::GetModeState();
    if (sl.optimal_hooked && sl.optimal_calls) m |= 1u << static_cast<uint32_t>(Layer::Streamline);
    if (na::unreal::GetScreenPercentageStatus().available) m |= 1u << static_cast<uint32_t>(Layer::Unreal);
    if (na::tsushima::GetApplyStatus().available || na::tlou1::GetApplyStatus().available) m |= 1u << static_cast<uint32_t>(Layer::ExactBuild);
    m |= 1u << static_cast<uint32_t>(Layer::Rebuild); // always possible to ask; it reports its own failure
    return m;
}

uint32_t PlanOrder(uint32_t available_mask, Layer learned, Layer* out, uint32_t cap)
{
    static const Layer kDefault[] = {Layer::Streamline, Layer::Unreal, Layer::ExactBuild, Layer::Rebuild};
    uint32_t n = 0;
    auto add = [&](Layer l) {
        if(!out || static_cast<uint32_t>(l)>=static_cast<uint32_t>(Layer::Count))return;
        const uint32_t bit = 1u << static_cast<uint32_t>(l);
        if (l == Layer::None || !(available_mask & bit) || n >= cap) return;
        for (uint32_t i = 0; i < n; ++i)
            if (out[i] == l) return;
        out[n++] = l;
    };
    add(learned);
    for (Layer l : kDefault) add(l);
    return n;
}

void Start()
{
    if (g_started.exchange(true)) return;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)) CloseHandle(t);
}

bool RequestApply(const Request& request)
{
    std::lock_guard<std::mutex> lock(g_m);
    if(request.mode>4 || !request.out_w || !request.out_h || !request.render_w || !request.render_h ||
       request.out_w==UINT32_MAX || request.out_h==UINT32_MAX || request.render_w>request.out_w || request.render_h>request.out_h)return false;
    if (!g_wake || g_running || g_status.state == State::Trying || g_has_pending) {
        strncpy_s(g_status.reason, "A previous Apply is still running. Try again in a moment.", _TRUNCATE);
        return false;
    }
    g_generation.fetch_add(1,std::memory_order_acq_rel);
    g_pending = request;
    g_has_pending = true;
    g_status.state = State::Trying;
    g_status.request_id = ++g_next_id;
    g_status.layer = Layer::None;
    strncpy_s(g_status.reason, "Applying...", _TRUNCATE);
    SetEvent(g_wake);
    return true;
}

void Cancel(bool hand_back)
{
    std::lock_guard<std::mutex> action(g_actions);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_has_pending = false;
        if (g_status.state == State::Trying) {
            g_status.state = State::Cancelled;
            strncpy_s(g_status.reason, "Selection changed; choose Apply again.", _TRUNCATE);
        }
    }
    na::slb::dlss::SetModeRequest({});
    na::unreal::CancelScreenPercentage();
    if (hand_back) {
        na::unreal::HandBackScreenPercentage();
    }
}

int FormatStatus(char* out, size_t cap)
{
    if (!out || !cap) return 0;
    const na::slb::dlss::ModeState d = na::slb::dlss::GetModeState();
    const Status ms = GetStatus();
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "picture_mode state=%u layer=%u learned=%u tried=0x%x available=0x%x request=%llu \"%s\""
        " | sl_dlss hooks=%u/%u calls=%llu/%llu gen=%llu gen_calls=%llu rewritten=%llu game=%u sent=%u size=%ux%u bounds=%ux%u-%ux%u label=%u->%u",
        static_cast<uint32_t>(ms.state), static_cast<uint32_t>(ms.layer), static_cast<uint32_t>(ms.learned), ms.tried_mask,
        ms.available_mask, static_cast<unsigned long long>(ms.request_id), ms.reason, d.optimal_hooked, d.set_hooked,
        static_cast<unsigned long long>(d.optimal_calls), static_cast<unsigned long long>(d.set_calls),
        static_cast<unsigned long long>(d.generation), static_cast<unsigned long long>(d.optimal_calls_this_generation),
        static_cast<unsigned long long>(d.rewritten_this_generation), d.game_mode, d.sent_mode, d.optimal_w, d.optimal_h,
        d.min_w, d.min_h, d.max_w, d.max_h, d.set_game_mode, d.set_sent_mode);
    return n < 0 ? static_cast<int>(strlen(out)) : n;
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_status;
}

} // namespace na::mode
