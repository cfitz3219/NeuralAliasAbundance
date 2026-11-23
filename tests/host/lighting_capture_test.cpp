// Exercise the real asynchronous recorder with a small stand-in scene. Only the
// session-directory provider is substituted, so CTest never touches game captures.
#include "test_support.hpp"
#include <imgui_internal.h>
#include "host_dx12/capture/capture_session.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/overlay/lighting_trials.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace recorder = na::capture::j2;
namespace {
fs::path sandbox;
bool reject_folder = false;
void __stdcall execute(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    queue->ExecuteCommandLists(n, lists);
}
}

namespace na::capture {
const Session& ThisProcess() { static Session session{}; return session; }
void TagDir(const wchar_t*, const char*) {}
bool MakeDir(const wchar_t* kind, wchar_t* out, size_t chars, bool require_new)
{
    if (reject_folder) return false;
    const auto path = sandbox / kind;
    if (!fs::create_directory(path) && require_new) return false;
    return wcscpy_s(out, chars, path.c_str()) == 0;
}
}

namespace na::overlay {
bool trial_menu_open = true;
void SetOpen(bool open) { trial_menu_open = open; }
}

namespace {
// Click through ordinary ImGui input at the centers of the visible buttons.
// The fixture provides the window, the output directory and the overlay-close sink.
void click_trial(recorder::LightingTrial trial, bool dlaa_on = true)
{
    ImGuiIO& io = ImGui::GetIO();
    const char* names[] = {"Still","Turn 1","Turn 2","Slide"};
    const auto draw = [&](bool enabled) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(710, 220));
        ImGui::Begin("Picture", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PushID("Lighting test");
        const ImGuiID wanted = ImGui::GetID(names[static_cast<int>(trial)-1]);
        ImGui::PopID();
        na::overlay::DrawLightingTrials(enabled);
        const bool found = ImGui::GetCurrentContext()->HoveredId == wanted;
        ImGui::End();
        ImGui::Render();
        return found;
    };
    // The responsive row may put the buttons below its description. Locate the
    // actual button by hit-testing the rendered window, then send ordinary input.
    draw(true);
    bool located = false;
    for (int y = 8; y < 220 && !located; y += 12) {
        for (int x = 8; x < 710 && !located; x += 16) {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            located = draw(true);
        }
    }
    CHECK(located);
    if (!located) return;
    draw(dlaa_on);
    io.AddMouseButtonEvent(0, true);
    draw(dlaa_on);
    io.AddMouseButtonEvent(0, false);
    draw(dlaa_on);
}
}

int wmain(int argc, wchar_t** argv)
{
    REQUIRE(argc == 2);
    sandbox = fs::path(argv[1]) / std::to_wstring(GetTickCount64());
    fs::create_directories(sandbox);
    test::Device device;
    if (!test::CreateDevice(device)) return test::kSkip;
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto texture = [&](D3D12_RESOURCE_STATES state) {
        return test::CreateTexture(device.device.Get(), 64, 32, fmt, state, D3D12_RESOURCE_FLAG_NONE);
    };
    auto color = texture(read), depth = texture(read), motion = texture(read), output = texture(read);
    auto present = texture(D3D12_RESOURCE_STATE_PRESENT);
    REQUIRE(color && depth && motion && output && present);
    recorder::Planes planes{};
    planes.color = color.Get(); planes.depth = depth.Get(); planes.motion = motion.Get();
    planes.output = output.Get(); planes.output_state = read;
    using Trial = recorder::LightingTrial;
    using Start = recorder::TrialStart;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 400);
    io.DeltaTime = 1.f / 60.f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    click_trial(Trial::Still, false);
    REQUIRE(!recorder::GetStats().capture_busy && na::overlay::trial_menu_open);
    reject_folder = true;
    click_trial(Trial::Still);
    REQUIRE(na::overlay::trial_menu_open);
    REQUIRE(recorder::StartLightingTrial(Trial::Still) == Start::FolderUnavailable);
    REQUIRE(!recorder::GetStats().capture_busy);
    reject_folder = false;
    REQUIRE(recorder::StartLightingTrial(Trial::Slide) == Start::NotApplicable);
    for (const Trial trial : {Trial::Still, Trial::Turn1, Trial::Turn2, Trial::Slide}) {
        recorder::SetFlatMapLightingTest(trial == Trial::Slide);
        na::overlay::trial_menu_open = true;
        click_trial(trial);
        REQUIRE(!na::overlay::trial_menu_open);
        REQUIRE(!recorder::Armed());
        REQUIRE(recorder::GetStats().capture_busy);
        REQUIRE(recorder::StartLightingTrial(Trial::Turn1) == Start::Busy);
        REQUIRE(!recorder::RequestTimestamped(1));
        recorder::Request(1, (sandbox / L"interloper").c_str());
        REQUIRE(!fs::exists(sandbox / L"interloper"));
        REQUIRE(recorder::GetStats().lighting_trial == trial);
        Sleep(recorder::kLightingDelayMs + 10);
        REQUIRE(recorder::Armed());
        recorder::Identity id{};
        id.width = 64; id.height = 32;
        // Ineligible frames must not consume one of the 30 slots.
        test::List invalid; REQUIRE(invalid.Create(device.device.Get()));
        id.presentation_only = true;
        REQUIRE(!recorder::RecordEvaluation(invalid.list.Get(), planes, id));
        id.presentation_only = false; id.evaluation_succeeded = false;
        REQUIRE(!recorder::RecordEvaluation(invalid.list.Get(), planes, id));
        invalid.list->Close(); id.evaluation_succeeded = true;
        const auto limit = GetTickCount64() + recorder::kLightingWindowMs;
        while (recorder::GetStats().recorded < recorder::kLightingFrames && GetTickCount64() < limit) {
            ++id.present_ordinal; id.inputs_frame = id.present_ordinal;
            test::List list; REQUIRE(list.Create(device.device.Get()));
            const auto token = recorder::RecordEvaluation(list.list.Get(), planes, id);
            REQUIRE(SUCCEEDED(list.list->Close()));
            if (!token) { Sleep(5); continue; }
            ID3D12CommandList* submit[] = {list.list.Get()};
            execute(device.queue.Get(), 1, submit);
            recorder::Submitted(token, device.queue.Get(), id.present_ordinal);
            recorder::OnFrameClosed(id.present_ordinal, 0);
            recorder::OnPresent(device.queue.Get(), present.Get(), execute);
            device.WaitIdle();
        }
        while (recorder::GetStats().capture_busy && GetTickCount64() < limit) Sleep(5);
        const auto stats = recorder::GetStats();
        REQUIRE(stats.written == recorder::kLightingFrames);
        REQUIRE(!stats.capture_busy && !stats.errors && !stats.without_present);
        const wchar_t* kinds[] = {L"j2-lighting-still", L"j2-lighting-turn1", L"j2-lighting-turn2", L"j2-lighting-slide"};
        const auto dir = sandbox / kinds[static_cast<int>(trial)-1];
        for (unsigned i = 0; i < recorder::kLightingFrames; ++i) {
            wchar_t prefix[12]; swprintf_s(prefix, L"%05u-", i);
            for (const auto* plane : {L"color", L"depth", L"motion", L"dlssout", L"present"})
                REQUIRE(fs::file_size(dir / (std::wstring(prefix) + plane + L".raw")) == 64u * 32u * 4u);
            REQUIRE(fs::file_size(dir / (std::wstring(prefix) + L"meta.json")) > 0);
        }
    }
    // A request on a scene that never evaluates must let the user retry.
    sandbox /= L"no-evaluation";
    fs::create_directory(sandbox);
    REQUIRE(recorder::StartLightingTrial(Trial::Still) == Start::Started);
    Sleep(recorder::kLightingDelayMs + recorder::kLightingWindowMs + 100);
    REQUIRE(!recorder::GetStats().capture_busy && !recorder::Armed());
    REQUIRE(recorder::GetStats().written == 0);
    recorder::Shutdown();
    ImGui::DestroyContext();
    return test::g_failures ? 1 : 0;
}
