#include <imgui.h>
#include <reshade.hpp>
#include "depth_addon.hpp"
#include "depth_preview.hpp"
#include "raw_snapshot.hpp"
#include "frame_history.hpp"
#include "capture_trigger.hpp"
#include "live_bridge.hpp"
#include "dlaa_preset.hpp"
#include "projection_verifier.hpp"
#include <cstring>
#include "adapters/alien_isolation.hpp"
#include "adapters/alien_neural.hpp"
#include <mutex>
#include <cstdio>
#include <fstream>
#include <string>
#include <stdexcept>
#include <d3d11_1.h>
#include <cmath>
#include <iomanip>
#include <chrono>

namespace {
using namespace reshade;
using Microsoft::WRL::ComPtr;
constexpr GUID camera_tag{0xd3d50bf1,0xf2be,0x4a90,{0x9b,0x73,0x49,0x22,0xab,0x06,0x8e,0x42}};
constexpr GUID taa_output_tag{0xf8a91843,0xd8de,0x483e,{0x83,0x61,0x21,0x02,0x45,0x66,0x19,0x73}};
struct __declspec(uuid("8307be10-48d9-4669-a534-18582de42a72")) DepthState {
    std::recursive_mutex mutex;
    dx11_enhancement::LiveBridge live;
    alien_isolation::NeuralTrial neural;
    uint64_t live_displayed_frame=UINT64_MAX;
    diagnostics::ProjectionSnapshot neural_camera_snapshot,neural_sharpen_constants;
    std::array<float,16> neural_view_projection{};
    bool neural_camera_captured=false;
    uint64_t neural_camera_frame=UINT64_MAX,neural_boundary_frame=UINT64_MAX;
    ComPtr<ID3D11RenderTargetView> neural_scene_pending;
    uint64_t neural_scene_frame=UINT64_MAX;
    diagnostics::TexturePreview live_depth,live_motion;
    diagnostics::ProjectionSnapshot live_projection;
    diagnostics::ProjectionVerifier live_verifier;
    ComPtr<ID3D11Buffer> live_cpu_source;
    UINT live_cpu_offset=0;
    const unsigned char* live_mapped=nullptr;
    std::array<float,16> live_cpu_upload{},live_cpu_frame{};
    uint64_t live_cpu_version=0,live_cpu_used=0,live_cpu_fast=0;
    bool live_cpu_fresh=false;
    ComPtr<ID3D11RenderTargetView> live_motion_source;
    ComPtr<ID3D11Resource> live_target;
    bool live_enabled=false,live_camera=false,live_submitted=false;
    dx11_enhancement::DlaaControlRequest live_request;
    uint64_t live_last_frame=UINT64_MAX,live_last_time=0,live_frames=0;
    double live_projection_ms=0,live_motion_ms=0,live_perf_projection=0,live_perf_motion=0;
    dx11_enhancement::BridgeTimings live_perf{};
    unsigned live_perf_count=0;
    uint64_t live_perf_start=0;
    std::string live_status="Off. Existing anti-aliasing remains active.";

    diagnostics::FrameHistory history;
    diagnostics::HistoryDecision history_decision;
    diagnostics::ResetReason last_reset=diagnostics::ResetReason::first_frame;
    diagnostics::DepthPreview preview;
    bool enabled = false, fresh = false, rendered = false, invert = false;
    bool shader_found = false, first_capture_logged = false, first_image_logged = false;
    bool inspect_requested = true, inspected = false, fit_requested = true, gradient = false;
    diagnostics::DepthStatistics statistics;
    float exponent = 1.0f, minimum = 0, maximum = 1;
    unsigned draws_this_frame = 0, draws_last_frame = 0;
    int occurrence = 0;
    HRESULT status = S_FALSE;
    unsigned long long passes = 0;
    diagnostics::TexturePreview motion;
    diagnostics::MotionStatistics motion_statistics;
    ComPtr<ID3D11RenderTargetView> pending_motion;
    bool motion_enabled = false, motion_fresh = false, motion_rendered = false;
    bool motion_inspect = true, motion_inspected = false, motion_logged = false;
    bool magnitude = false, uv_to_pixels = false;
    bool emphasize_small_motion = true;
    float sample_u = 0.5f, sample_v = 0.5f;
    float motion_gain = 100.0f;
    HRESULT motion_status = S_FALSE;
    diagnostics::RawSnapshot pair_color[2], pair_motion[2];
    diagnostics::RawSnapshot pair_pre_color[2];
    diagnostics::RawSnapshot pair_depth[2];
    diagnostics::ProjectionSnapshot pair_projection[2];
    diagnostics::ProjectionSnapshot pair_dof[2], pair_rgbm[2];
    bool dof_valid[2]{}, rgbm_valid[2]{}, dof_same_depth[2]{}, rgbm_same_color[2]{};
    UINT dof_first[2]{}, rgbm_first[2]{}, dof_seen[2]{}, rgbm_seen[2]{};
    ComPtr<ID3D11Resource> pair_depth_source;
    UINT pair_first_constant[2]{};
    bool pair_pre_fresh=false, pair_projection_fresh=false;
    ComPtr<ID3D11Resource> pair_pre_target;
    bool pair_armed = false, pair_fresh = false;
    diagnostics::CaptureTrigger pair_trigger;
    HRESULT pair_projection_status=S_FALSE, pair_depth_status=S_FALSE;
    unsigned pair_count = 0;
    uint64_t frame = 0, pair_frame[2]{}, pair_due = 0;
    uint64_t pair_time[2]{};
    UINT pair_boundary[2]{};
    std::string pair_status = "No pair captured yet.";
    unsigned trace_remaining = 0, trace_consumers = 0, trace_executes = 0;
    uint64_t trace_armed_frame=0;
    bool trace_matrix_fresh = false;
    ComPtr<ID3D11Buffer> trace_matrix;
    ComPtr<ID3D11Resource> trace_output;
};
HRESULT snapshot_camera_projection(ID3D11DeviceContext* context, diagnostics::ProjectionSnapshot& snapshot, UINT& first) {
    ComPtr<ID3D11Buffer> source;
    ComPtr<ID3D11DeviceContext1> extended;
    first=0;
    UINT constants=0;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended)))) {
        extended->VSGetConstantBuffers1(1,1,&source,&first,&constants);
        if (constants<4) return E_INVALIDARG;
    } else context->VSGetConstantBuffers(1,1,&source);
    if (first>UINT_MAX/16) return E_INVALIDARG;
    return snapshot.capture(context,source.Get(),first*16);
}
HRESULT snapshot_pixel_constants(ID3D11DeviceContext* context, UINT slot, UINT start, UINT count,
    diagnostics::ProjectionSnapshot& snapshot, UINT& first) {
    ComPtr<ID3D11Buffer> source;ComPtr<ID3D11DeviceContext1> extended;
    first=0;UINT constants=0;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended)))) {
        extended->PSGetConstantBuffers1(slot,1,&source,&first,&constants);
        if (constants<start || constants-start<count) return E_INVALIDARG;
    } else context->PSGetConstantBuffers(slot,1,&source);
    if (first>UINT_MAX/16-start) return E_INVALIDARG;
    return snapshot.capture(context,source.Get(),(first+start)*16,count*4);
}
struct __declspec(uuid("184f8c81-d84a-4ec0-acb0-f81941b11af8")) CommandState {
    UINT role = 0;
    bool taa_bound = false, taa_dispatched = false;
    ComPtr<ID3D11Resource> taa_output;
};


// Read-only observation of the tracked camera buffer's writable mapping.
// Qualification below detects whether other hooks applied jitter before this event.
void camera_map(api::device* device,api::resource resource,uint64_t offset,uint64_t size,api::map_access access,void** data){
    auto* state=device->get_private_data<DepthState>();if(!state)return;
    std::lock_guard lock(state->mutex);
    if(!state->live_enabled || resource.handle!=reinterpret_cast<uintptr_t>(state->live_cpu_source.Get()))return;
    state->live_mapped=nullptr;
    if(access==api::map_access::read_only || !data || !*data || offset>state->live_cpu_offset)return;
    D3D11_BUFFER_DESC desc{};state->live_cpu_source->GetDesc(&desc);
    const uint64_t start=state->live_cpu_offset,relative=start-offset;
    if(start+64>desc.ByteWidth || (size!=UINT64_MAX && (relative>size || size-relative<64)))return;
    state->live_mapped=static_cast<const unsigned char*>(*data)+relative;
}
void camera_unmap(api::device* device,api::resource resource){
    auto* state=device->get_private_data<DepthState>();if(!state)return;
    std::lock_guard lock(state->mutex);
    if(resource.handle!=reinterpret_cast<uintptr_t>(state->live_cpu_source.Get()) || !state->live_mapped)return;
    std::memcpy(state->live_cpu_upload.data(),state->live_mapped,64);state->live_mapped=nullptr;++state->live_cpu_version;
}
void live_failure(DepthState& state,const char* message) {
    state.live_enabled=false;state.live.stop();state.live_target.Reset();
    state.live_camera=state.live_submitted=false;
    state.live_status=std::string("DLAA off; existing AA retained: ")+message;
    log::message(log::level::warning,state.live_status.c_str());
}
void live_submit(DepthState& state,ID3D11DeviceContext* context,ID3D11Resource* target) {
    if(!state.live_enabled || !state.live_camera || !state.live_motion_source)return;
    try {
        if(state.live_submitted)throw std::runtime_error("multiple temporal passes in one frame");
        ComPtr<ID3D11Texture2D> texture;
        if(FAILED(target->QueryInterface(IID_PPV_ARGS(&texture))))throw std::runtime_error("color texture unavailable");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(desc.Format!=DXGI_FORMAT_R11G11B10_FLOAT || desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1 ||
            desc.Width!=state.live_depth.width() || desc.Height!=state.live_depth.height())throw std::runtime_error("unsupported color/depth layout");
        if(!state.live.started()) {
            wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("game path unavailable");
            const auto root=std::filesystem::path(exe).parent_path();
            std::ifstream config(root/L"dx11-dlaa-paths.txt");std::string helper,runtime,cache;
            if(!std::getline(config,helper) || !std::getline(config,runtime) || !std::getline(config,cache))
                throw std::runtime_error("DLAA helper configuration missing");
            state.live.start(context,desc.Width,desc.Height,std::filesystem::u8path(helper),std::filesystem::u8path(runtime),
                std::filesystem::u8path(cache)/std::to_string(GetTickCount64()),state.live_request.preset);
            state.live_last_frame=UINT64_MAX;state.live_last_time=0;
            state.live_status="Starting DLAA helper; existing AA active.";
        }
        if(!state.live.ready())return;
        const auto projection_begin=std::chrono::steady_clock::now();
        std::array<float,16> matrix{};
        const bool trusted=state.live_verifier.trusted();
        if(state.live_cpu_fresh && trusted && state.live_frames%120!=0){
            matrix=state.live_cpu_frame;++state.live_cpu_fast;
        }else{
            if(FAILED(state.live_projection.inspect(context,matrix)))throw std::runtime_error("camera projection read failed");
            if(state.live_cpu_fresh){
                const bool matched=state.live_verifier.observe(state.live_cpu_frame,matrix);
                if(trusted && !matched)log::message(log::level::warning,"[DX11DLAACamera] CPU/GPU verification mismatch; using GPU readback.");
                if(!trusted && state.live_verifier.trusted())log::message(log::level::info,"[DX11DLAACamera] Eight CPU uploads match GPU projection including changing jitter; cached upload path qualified.");
            }else state.live_verifier.reset();
        }
        if(state.neural.needs_camera() && state.neural_camera_captured &&
            SUCCEEDED(state.neural_camera_snapshot.inspect(context,state.neural_view_projection)))
            state.neural_camera_frame=state.frame;
        state.live_projection_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-projection_begin).count();
        const auto motion_begin=std::chrono::steady_clock::now();
        if(FAILED(state.live_motion.capture_motion(context,state.live_motion_source.Get())) ||
            state.live_motion.width()!=desc.Width || state.live_motion.height()!=desc.Height ||
            state.live_motion.format()!=DXGI_FORMAT_R16G16_FLOAT)throw std::runtime_error("motion capture/layout mismatch");
        state.live_motion_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-motion_begin).count();
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11ShaderResourceView> color;
        if(FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&color)))throw std::runtime_error("color view failed");
        dx11_enhancement::BridgeFrame frame;
        frame.source_frame=state.frame;frame.width=desc.Width;frame.height=desc.Height;
        frame.jitter_x=matrix[2]*float(desc.Width)*.5f;frame.jitter_y=-matrix[6]*float(desc.Height)*.5f;
        frame.motion_scale_x=-float(desc.Width);frame.motion_scale_y=-float(desc.Height);
        const auto now=GetTickCount64();const auto elapsed=state.live_last_time?now-state.live_last_time:0;
        frame.reset=state.live_last_frame==UINT64_MAX || state.frame!=state.live_last_frame+1 || elapsed>250;
        frame.frame_ms=elapsed && elapsed<=250?float(elapsed):16.6667f;
        state.live.submit(context,color.Get(),state.live_depth.raw_view(),state.live_motion.raw_view(),frame);
        state.live_target=target;state.live_submitted=true;state.live_last_frame=state.frame;state.live_last_time=now;
    }catch(const std::exception& e){live_failure(state,e.what());}
}
void live_consume(DepthState& state,ID3D11DeviceContext* context) {
    if(!state.live_enabled || !state.live.pending() || !state.live_target)return;
    ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
    context->PSGetShaderResources(0,1,&view);if(view)view->GetResource(&resource);
    if(resource!=state.live_target)return;
    try {
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateRenderTargetView(resource.Get(),nullptr,&target)))throw std::runtime_error("return color view failed");
        state.live.resolve(context,target.Get());++state.live_frames;state.live_displayed_frame=state.frame;
        const auto timing=state.live.timings();
        state.live_perf_projection+=state.live_projection_ms;state.live_perf_motion+=state.live_motion_ms;
        state.live_perf.input_wait_ms+=timing.input_wait_ms;state.live_perf.submit_ms+=timing.submit_ms;
        state.live_perf.output_wait_ms+=timing.output_wait_ms;state.live_perf.return_ms+=timing.return_ms;
        if(!state.live_perf_count)state.live_perf_start=GetTickCount64();
        if(++state.live_perf_count==120){
            char line[512]{};
            std::snprintf(line,sizeof(line),"[DX11DLAAPerf] 120 frames CPU elapsed averages ms: projection=%.3f motionCopy=%.3f inputWait=%.3f submit=%.3f outputWait=%.3f return=%.3f; 119-frame wall span=%llums; not GPU timings; cached camera frames=%llu",
                state.live_perf_projection/120,state.live_perf_motion/120,state.live_perf.input_wait_ms/120,
                state.live_perf.submit_ms/120,state.live_perf.output_wait_ms/120,state.live_perf.return_ms/120,
                GetTickCount64()-state.live_perf_start,state.live_cpu_fast);
            log::message(log::level::info,line);state.live_perf_count=0;state.live_perf={};state.live_perf_projection=state.live_perf_motion=0;
        }
        state.live_status=std::string("Active. Preset ")+dx11_enhancement::dlaa_preset_name(state.live_request.preset)+" requested.";
        if(state.live_frames==1)log::message(log::level::info,"[DX11DLAA] First live result supplied at verified color consumer.");
    }catch(const std::exception& e){live_failure(state,e.what());}
}
void init_device(api::device* device) {
    if (device->get_api() == api::device_api::d3d11 && alien_isolation::is_host())
        device->create_private_data<DepthState>();
}
void destroy_device(api::device* device) {
    if (device->get_private_data<DepthState>()) device->destroy_private_data<DepthState>();
}
void init_commands(api::command_list* commands) {
    if (commands->get_device()->get_private_data<DepthState>()) commands->create_private_data<CommandState>();
}
void destroy_commands(api::command_list* commands) {
    if (commands->get_private_data<CommandState>()) commands->destroy_private_data<CommandState>();
}
void init_pipeline(api::device* device, api::pipeline_layout, uint32_t count,
    const api::pipeline_subobject* objects, api::pipeline pipeline)
{
    auto* state = device->get_private_data<DepthState>();
    if (!state) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (objects[i].type == api::pipeline_subobject_type::compute_shader && objects[i].count == 1) {
            const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
            if (alien_isolation::is_alias_taa_shader(shader.code,shader.code_size)) {
                const UINT tag=5;
                reinterpret_cast<ID3D11ComputeShader*>(pipeline.handle)->SetPrivateData(camera_tag,sizeof(tag),&tag);
                log::message(log::level::info,"[DX11Trace] Pinned Alias TAA compute shader identified.");
            }
        }
        if (objects[i].type == api::pipeline_subobject_type::vertex_shader && objects[i].count == 1) {
            const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
            if (alien_isolation::is_rgbm_vertex_shader(shader.code,shader.code_size)) {
                const UINT tag=4;
                reinterpret_cast<ID3D11VertexShader*>(pipeline.handle)->SetPrivateData(camera_tag,sizeof(tag),&tag);
            }
        }
        if (objects[i].type != api::pipeline_subobject_type::pixel_shader || objects[i].count != 1) continue;
        const auto& shader = *static_cast<const api::shader_desc*>(objects[i].data);
        if(alien_isolation::is_alias_sharpen_shader(shader.code,shader.code_size)){
            const UINT tag=6;
            reinterpret_cast<ID3D11PixelShader*>(pipeline.handle)->SetPrivateData(camera_tag,sizeof(tag),&tag);
            log::message(log::level::info,"[DX11Neural] Pinned Alias sharpen shader identified; candidate post-sharpen/pre-HUD boundary.");
            continue;
        }
        const bool camera = alien_isolation::is_camera_motion_shader(shader.code, shader.code_size);
        const bool consumer = alien_isolation::is_temporal_consumer_shader(shader.code, shader.code_size);
        if (!camera && !consumer) continue;
        auto* native = reinterpret_cast<ID3D11PixelShader*>(pipeline.handle);
        const UINT tag = camera ? 1 : (alien_isolation::is_rgbm_pixel_shader(shader.code,shader.code_size) ? 3 : 2);
        if (FAILED(native->SetPrivateData(camera_tag, sizeof(tag), &tag))) return;
        std::lock_guard lock(state->mutex);
        if (camera) state->shader_found = true;
        log::message(log::level::info, camera ? "[DX11Depth] Known camera-motion shader identified by DXBC signature."
            : "[DX11Motion] Known DoF/RGBM consumer pixel shader identified.");
    }
}
UINT shader_role(ID3D11DeviceChild* shader) {
    UINT size = sizeof(UINT), tag = 0;
    return shader && SUCCEEDED(shader->GetPrivateData(camera_tag, &size, &tag)) ? tag : 0;
}
// Called before the following draw/command-list submission. The identified scene
// draw has completed by this boundary. Never use begin_effects as an image fallback.
void flush_neural_scene(DepthState& state,ID3D11DeviceContext* context){
    if(!state.neural_scene_pending)return;
    auto target=std::move(state.neural_scene_pending);
    if(state.neural_scene_frame!=state.frame)return;
    state.neural_boundary_frame=state.frame;
    state.neural.render(context,target.Get(),state.live_depth.raw_view(),state.live_motion.raw_view(),
        state.frame,state.live_enabled && state.live_displayed_frame==state.frame,
        state.neural_view_projection,state.neural_camera_frame==state.frame);
}
void neural_command_boundary(api::command_list* commands){
    auto* state=commands->get_device()->get_private_data<DepthState>();if(!state)return;
    auto* context=reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(state->mutex);flush_neural_scene(*state,context);
}
// Copies, clears and indirect work can occur between the scene and UI draws too.
bool neural_indirect(api::command_list* c,api::indirect_command,api::resource,uint64_t,uint32_t,uint32_t){
    neural_command_boundary(c);return false;
}
bool neural_clear(api::command_list* c,api::resource_view,const float[4],uint32_t,const api::rect*){
    neural_command_boundary(c);return false;
}
bool neural_copy_region(api::command_list* c,api::resource,uint32_t,const api::subresource_box*,
    api::resource,uint32_t,const api::subresource_box*,api::filter_mode){
    neural_command_boundary(c);return false;
}
void neural_draw_boundary(api::command_list* commands){
    auto* state=commands->get_device()->get_private_data<DepthState>();if(!state)return;
    auto* context=reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    std::lock_guard lock(state->mutex);
    flush_neural_scene(*state,context);
    if(!state->neural.needs_camera() || state->live_displayed_frame!=state->frame ||
        state->neural_boundary_frame==state->frame)return;
    ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
    if(shader_role(shader.Get())!=6)return;
    // The pinned shader also draws Alias Isolation's startup logo. Wait until it fades.
    UINT first=0;std::array<float,16> constants{};
    if(FAILED(snapshot_pixel_constants(context,10,0,1,state->neural_sharpen_constants,first)) ||
        FAILED(state->neural_sharpen_constants.inspect(context,constants)) ||
        !std::isfinite(constants[0]) || constants[0]>0)return;
    D3D11_VIEWPORT viewport{};UINT count=1;context->RSGetViewports(&count,&viewport);
    if(count!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0 ||
        viewport.Width!=float(state->live_depth.width()) || viewport.Height!=float(state->live_depth.height()))return;
    context->OMGetRenderTargets(1,state->neural_scene_pending.ReleaseAndGetAddressOf(),nullptr);
    state->neural_scene_frame=state->frame;
}
void bind_pipeline(api::command_list* commands, api::pipeline_stage stages, api::pipeline pipeline) {
    auto* state = commands->get_private_data<CommandState>();
    if (state && (stages & api::pipeline_stage::pixel_shader) != static_cast<api::pipeline_stage>(0))
        state->role = shader_role(reinterpret_cast<ID3D11PixelShader*>(pipeline.handle));
    if (state && (stages & api::pipeline_stage::compute_shader) != static_cast<api::pipeline_stage>(0))
        state->taa_bound = shader_role(reinterpret_cast<ID3D11ComputeShader*>(pipeline.handle))==5;
}

bool trace_dispatch(api::command_list* commands,uint32_t,uint32_t,uint32_t) {
    neural_command_boundary(commands);
    auto* state=commands->get_private_data<CommandState>();
    if (state && state->taa_bound) state->taa_dispatched=true;
    return false;
}
bool trace_copy(api::command_list* commands,api::resource,api::resource destination) {
    neural_command_boundary(commands);
    auto* state=commands->get_private_data<CommandState>();
    if (state && state->taa_dispatched) {
        state->taa_output=reinterpret_cast<ID3D11Resource*>(destination.handle);
        state->taa_dispatched=false;
    }
    return false;
}
void trace_secondary(api::command_list* primary,api::command_list* secondary) {
    auto* device_state=primary->get_device()->get_private_data<DepthState>();
    if (!device_state) return;
    auto* native=reinterpret_cast<IUnknown*>(primary->get_native());
    ComPtr<ID3D11CommandList> recorded;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&recorded)))) {
        // ReShade's after-FinishCommandList event: transfer deferred recording metadata.
        auto* source=secondary->get_private_data<CommandState>();
        if (source && source->taa_output) {
            recorded->SetPrivateDataInterface(taa_output_tag,source->taa_output.Get());
            source->taa_output.Reset();source->taa_dispatched=false;
        }
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&context))) || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    {
        std::lock_guard lock(device_state->mutex);
        flush_neural_scene(*device_state,context.Get());
    }
    ComPtr<ID3D11CommandList> list;
    if (FAILED(reinterpret_cast<IUnknown*>(secondary->get_native())->QueryInterface(IID_PPV_ARGS(&list)))) return;
    ComPtr<ID3D11Resource> output;
    UINT size=sizeof(ID3D11Resource*);
    if (FAILED(list->GetPrivateData(taa_output_tag,&size,output.GetAddressOf()))) return;
    std::lock_guard lock(device_state->mutex);
    live_submit(*device_state,context.Get(),output.Get());
    if (device_state->trace_remaining) {
        device_state->trace_output=output;
        ++device_state->trace_executes;
    }
    if (device_state->pair_armed && device_state->pair_trigger.ready() &&
        device_state->pair_projection_fresh && device_state->pending_motion) {
        auto& state=*device_state;
        const unsigned index=state.pair_count;
        // This callback runs before the original ExecuteCommandList submits TAA.
        HRESULT hr=state.pair_pre_fresh ? E_UNEXPECTED :
            state.pair_pre_color[index].capture_resource(context.Get(),output.Get(),DXGI_FORMAT_R11G11B10_FLOAT);
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D11Resource> motion;
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            state.pending_motion->GetResource(&motion);state.pending_motion->GetDesc(&view);
            hr=state.pair_motion[index].capture_resource(context.Get(),motion.Get(),view.Format);
        }
        if (SUCCEEDED(hr) && (state.pair_pre_color[index].width()!=state.pair_motion[index].width() ||
            state.pair_pre_color[index].height()!=state.pair_motion[index].height())) hr=E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            state.pair_pre_target=output;state.pair_pre_fresh=true;
        } else {
            state.pair_armed=false;
            state.pair_status="Pre-TAA capture rejected: unsupported/mismatched textures or multiple TAA executions.";
            log::message(log::level::warning,state.pair_status.c_str());
        }
    }
}
void capture(api::command_list* commands) {
    auto* command = commands->get_private_data<CommandState>();
    if (!command || !command->role) return;
    auto* state = commands->get_device()->get_private_data<DepthState>();
    if (!state) return;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    // Validate the actual binding as other mods may restore state without a bind event.
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    const UINT role = shader_role(shader.Get());
    if (!role) return;
    std::lock_guard lock(state->mutex);
    if (role == 6) return; // Scene-boundary bookkeeping is separate from depth capture.
    if (role == 3) {
        ComPtr<ID3D11VertexShader> vertex;
        context->VSGetShader(&vertex,nullptr,nullptr);
        if (shader_role(vertex.Get())!=4) return;
    }
    if (role == 2 || role == 3) {
        live_consume(*state,context);
        if (state->pair_armed && state->pair_trigger.ready() && state->pair_pre_fresh) {
            const unsigned i=state->pair_count;
            if (role==2 && state->dof_seen[i]++==0) {
                state->dof_valid[i]=SUCCEEDED(snapshot_pixel_constants(context,0,11,2,state->pair_dof[i],state->dof_first[i]));
                ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
                context->PSGetShaderResources(2,1,&view);if(view)view->GetResource(&resource);
                state->dof_same_depth[i]=resource && resource==state->pair_depth_source;
            }
            if (role==3 && state->rgbm_seen[i]++==0) {
                state->rgbm_valid[i]=SUCCEEDED(snapshot_pixel_constants(context,2,40,1,state->pair_rgbm[i],state->rgbm_first[i]));
                ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11Resource> resource;
                context->PSGetShaderResources(0,1,&view);if(view)view->GetResource(&resource);
                state->rgbm_same_color[i]=resource && resource==state->pair_pre_target;
            }
        }
        if (state->trace_remaining && state->trace_consumers++<8) {
            ComPtr<ID3D11ShaderResourceView> view;
            ComPtr<ID3D11Resource> color;
            context->PSGetShaderResources(0,1,&view);
            if (view) view->GetResource(&color);
            char message[256]{};
            std::snprintf(message,sizeof(message),"[DX11Trace] frame=%llu consumer=%u priorTaaLists=%u sameColor=%u color=%p taaOutput=%p",
                state->frame,role,state->trace_executes,color && color==state->trace_output ? 1u:0u,
                static_cast<void*>(color.Get()),static_cast<void*>(state->trace_output.Get()));
            log::message(log::level::info,message);
        }
        if (state->motion_enabled && state->pending_motion && !state->motion_fresh) {
            state->motion_status = state->motion.capture_motion(context, state->pending_motion.Get());
            state->motion_fresh = SUCCEEDED(state->motion_status);
            if (state->pair_armed && state->pair_trigger.ready() && state->motion_fresh) {
                ComPtr<ID3D11ShaderResourceView> color;
                context->PSGetShaderResources(0, 1, &color);
                const unsigned index = state->pair_count;
                state->pair_time[index] = GetTickCount64();
                state->pair_boundary[index] = role;
                ComPtr<ID3D11Resource> target;
                if (color) color->GetResource(&target);
                const char* reason="post-color copy failed";
                HRESULT hr=E_UNEXPECTED;
                if (!state->pair_projection_fresh) reason="camera projection/depth missing";
                else if (!state->pair_pre_fresh) reason="pre-TAA snapshot missing";
                else if (!target || target!=state->pair_pre_target) reason="pre/post color resource differs";
                else hr=state->pair_color[index].capture(context,color.Get());
                if (SUCCEEDED(hr) && !state->pair_pre_color[index].matches(state->pair_color[index])) {
                    hr=E_INVALIDARG;reason="pre/post color layout differs";
                }
                if (SUCCEEDED(hr) && (state->pair_color[index].width()!=state->motion.width() ||
                    state->pair_color[index].height()!=state->motion.height())) {
                    hr=E_INVALIDARG;reason="color/motion dimensions differ";
                }
                state->pair_fresh = SUCCEEDED(hr);
                if (FAILED(hr)) {
                    state->pair_armed = false;
                    char message[384]{};
                    std::snprintf(message,sizeof(message),"[DX11Pair] Rejected: %s; frame=%llu role=%u index=%u hr=0x%08X projection=0x%08X depth=0x%08X early=%u cameraDraws=%u",
                        reason,state->frame,role,index,static_cast<unsigned>(hr),
                        static_cast<unsigned>(state->pair_projection_status),static_cast<unsigned>(state->pair_depth_status),
                        state->pair_pre_fresh?1u:0u,state->draws_this_frame);
                    state->pair_status = message;
                    log::message(log::level::warning,message);
                }
            }
            if (!state->motion_logged) {
                char message[256]{};
                D3D11_RENDER_TARGET_VIEW_DESC rt{};
                state->pending_motion->GetDesc(&rt);
                std::snprintf(message, sizeof(message), "[DX11Motion] Consumer snapshot: status=0x%08X format=%u size=%ux%u. Encoding and object coverage unvalidated.",
                    static_cast<unsigned>(state->motion_status), static_cast<unsigned>(rt.Format),
                    state->motion.width(), state->motion.height());
                log::message(log::level::info, message);
                state->motion_logged = true;
            }
        }
        return;
    }
    ++state->passes;
    ++state->draws_this_frame;
    if(state->live_enabled) {
        state->live_camera=false;state->live_cpu_fresh=false;
        state->neural_camera_captured=false;state->neural_camera_frame=UINT64_MAX;
        if(state->neural.needs_camera()){
            // Pinned camera PS cb0[0..3]: main ViewProj, maintained by Alias XSC hook.
            // Do NOT use cb2[12..15]: Alias replaces it with current-to-previous reprojection.
            UINT first=0;
            state->neural_camera_captured=SUCCEEDED(snapshot_pixel_constants(context,0,0,4,state->neural_camera_snapshot,first));
        }
        if(state->draws_this_frame!=1)live_failure(*state,"multiple camera-motion passes");
        else {
            ComPtr<ID3D11ShaderResourceView> depth;context->PSGetShaderResources(8,1,&depth);
            UINT first=0;
            state->live_camera=SUCCEEDED(state->live_depth.capture(context,depth.Get())) &&
                state->live_depth.format()==DXGI_FORMAT_R32_FLOAT &&
                SUCCEEDED(snapshot_camera_projection(context,state->live_projection,first));
            ComPtr<ID3D11Buffer> buffer;context->VSGetConstantBuffers(1,1,&buffer);
            if(buffer!=state->live_cpu_source || first*16!=state->live_cpu_offset){
                state->live_cpu_source=buffer;state->live_cpu_offset=first*16;state->live_mapped=nullptr;
                state->live_cpu_version=state->live_cpu_used=0;state->live_verifier.reset();
            }
            if(state->live_cpu_version!=state->live_cpu_used){
                state->live_cpu_frame=state->live_cpu_upload;state->live_cpu_fresh=true;
                state->live_cpu_used=state->live_cpu_version;
            }
            context->OMGetRenderTargets(1,state->live_motion_source.ReleaseAndGetAddressOf(),nullptr);
            if(!state->live_camera)live_failure(*state,"unsupported camera depth/projection");
        }
    }
    if (state->occurrence > 0 &&
        state->draws_this_frame != static_cast<unsigned>(state->occurrence)) return;
    if (state->pair_armed && state->pair_trigger.ready()) {
        const unsigned i=state->pair_count;
        state->dof_valid[i]=state->rgbm_valid[i]=false;
        state->dof_same_depth[i]=state->rgbm_same_color[i]=false;
        state->dof_seen[i]=state->rgbm_seen[i]=0;state->dof_first[i]=state->rgbm_first[i]=0;
        state->pair_depth_source.Reset();
        state->pair_pre_fresh=false;state->pair_pre_target.Reset();
        state->pair_projection_status=snapshot_camera_projection(context,
            state->pair_projection[state->pair_count],state->pair_first_constant[state->pair_count]);
        ComPtr<ID3D11ShaderResourceView> depth;
        context->PSGetShaderResources(8,1,&depth);
        if(depth)depth->GetResource(&state->pair_depth_source);
        state->pair_depth_status=state->pair_depth[state->pair_count].capture(context,depth.Get());
        state->pair_projection_fresh=SUCCEEDED(state->pair_projection_status) && SUCCEEDED(state->pair_depth_status);
    }
    if (state->trace_remaining) {
        state->trace_matrix_fresh=false;
        ComPtr<ID3D11Buffer> source;
        context->VSGetConstantBuffers(1,1,&source);
        if (source) {
            D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
            if (desc.ByteWidth>=64) {
                if (!state->trace_matrix) {
                    D3D11_BUFFER_DESC target{};target.ByteWidth=64;
                    target.Usage=D3D11_USAGE_STAGING;target.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    ComPtr<ID3D11Device> device;context->GetDevice(&device);
                    device->CreateBuffer(&target,nullptr,&state->trace_matrix);
                }
                if (state->trace_matrix) {
                    const D3D11_BOX box{0,0,0,64,1,1};
                    context->CopySubresourceRegion(state->trace_matrix.Get(),0,0,0,0,source.Get(),0,&box);
                    state->trace_matrix_fresh=true;
                }
            }
        }
    }
    if (state->motion_enabled) {
        state->motion_fresh = false;
        state->pair_fresh = false;
        state->pending_motion.Reset();
        context->OMGetRenderTargets(1, &state->pending_motion, nullptr);
    }
    if (!state->enabled) return;
    ComPtr<ID3D11ShaderResourceView> source;
    context->PSGetShaderResources(8, 1, &source);
    state->status = state->preview.capture(context, source.Get());
    state->fresh = SUCCEEDED(state->status);
    if (state->fresh && !state->first_capture_logged) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "[DX11Depth] Copied camera-motion t8 depth: %ux%u. Raw depth, visual validation pending.",
            state->preview.width(), state->preview.height());
        log::message(log::level::info, message);
        state->first_capture_logged = true;
    }
}
bool draw(api::command_list* c, uint32_t, uint32_t, uint32_t, uint32_t) { neural_draw_boundary(c); capture(c); return false; }
bool draw_indexed(api::command_list* c, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { neural_draw_boundary(c); capture(c); return false; }

void finish_pair(DepthState& state, ID3D11DeviceContext* context) {
    if (!state.pair_armed || !state.pair_trigger.ready()) return;
    if (!state.pair_fresh) {
        if (state.pair_count || GetTickCount64() > state.pair_due + 10000) {
            state.pair_armed = false;
            state.pair_status = "Pair cancelled: missing consecutive capture or timeout. Try during gameplay.";
            log::message(log::level::warning,state.pair_status.c_str());
        }
        return;
    }
    state.pair_frame[state.pair_count] = state.frame;
    ++state.pair_count;
    if (state.pair_count != 2) return;
    state.pair_armed = false;
    if (state.pair_frame[1] != state.pair_frame[0]+1 ||
        state.pair_boundary[0] != state.pair_boundary[1] || state.pair_time[1]-state.pair_time[0]>250 ||
        !state.pair_color[0].matches(state.pair_color[1]) || !state.pair_motion[0].matches(state.pair_motion[1]) ||
        !state.pair_depth[0].matches(state.pair_depth[1])) {
        state.pair_status = "Pair cancelled: frame/time gap, consumer boundary or texture layout changed.";
        log::message(log::level::warning,state.pair_status.c_str());
        return;
    }
    try {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr,path,32768);
        if (!length || length>=32768) throw std::runtime_error("executable path");
        const auto root = std::filesystem::path(path).parent_path()/L"dx11-captures";
        std::filesystem::create_directories(root);
        const auto directory = root/("pair-"+std::to_string(GetCurrentProcessId())+"-"+
            std::to_string(GetTickCount64())+"-"+std::to_string(state.frame));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("capture directory");
        for (unsigned i=0;i<2;++i) {
            if (state.pair_depth[i].width()!=state.pair_color[i].width() ||
                state.pair_depth[i].height()!=state.pair_color[i].height() ||
                FAILED(state.pair_depth[i].save(context,directory/("depth"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("depth layout/readback/write");
            if (FAILED(state.pair_color[i].save(context,directory/("color"+std::to_string(i)+".bin"),state.pair_frame[i])) ||
                FAILED(state.pair_motion[i].save(context,directory/("motion"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("texture readback/write");
            if (FAILED(state.pair_pre_color[i].save(context,directory/("precolor"+std::to_string(i)+".bin"),state.pair_frame[i])))
                throw std::runtime_error("pre-TAA readback/write");
        }
        std::ofstream jitter(directory/"jitter.json");jitter<<std::setprecision(9);
        jitter<<"{\"version\":1,\"coordinate_note\":\"source sample coordinates, not final DLAA API signs\",\"frames\":[";
        for (unsigned i=0;i<2;++i) {
            std::array<float,16> values{};
            if (FAILED(state.pair_projection[i].inspect(context,values))) throw std::runtime_error("projection readback");
            for(float value:values) if (!std::isfinite(value)) throw std::runtime_error("nonfinite projection");
            if (i) jitter<<",";
            jitter<<"{\"frame\":"<<state.pair_frame[i]<<",\"first_constant\":"<<state.pair_first_constant[i]
                <<",\"width\":"<<state.pair_pre_color[i].width()<<",\"height\":"<<state.pair_pre_color[i].height()<<",\"matrix\":[";
            for(unsigned j=0;j<16;++j) { if(j)jitter<<",";jitter<<values[j]; }
            jitter<<"],\"source_sample_pixels\":["<<values[2]*state.pair_pre_color[i].width()/2
                <<","<<values[6]*state.pair_pre_color[i].height()/2<<"]}";
        }
        jitter<<"]}\n";jitter.close();if(!jitter)throw std::runtime_error("jitter write");
        std::ofstream constants(directory/"consumer-constants.json");constants<<std::setprecision(9);
        constants<<"{\"version\":1,\"frames\":[";
        for(unsigned i=0;i<2;++i) {
            if(i)constants<<",";
            constants<<"{\"frame\":"<<state.pair_frame[i]<<",\"dof_seen\":"<<state.dof_seen[i]
                <<",\"rgbm_seen\":"<<state.rgbm_seen[i]<<",\"dof_same_depth_resource\":"<<(state.dof_same_depth[i]?"true":"false")
                <<",\"rgbm_same_precolor_resource\":"<<(state.rgbm_same_color[i]?"true":"false")
                <<",\"dof_binding_first_constant\":"<<state.dof_first[i]<<",\"rgbm_binding_first_constant\":"<<state.rgbm_first[i];
            for(unsigned kind=0;kind<2;++kind) {
                constants<<(kind?",\"rgbm_scale\":":",\"dof_inverse\":");
                const bool valid=kind?state.rgbm_valid[i]:state.dof_valid[i];
                if(!valid) { constants<<"null";continue; }
                std::array<float,16> values{};
                if(FAILED((kind?state.pair_rgbm[i]:state.pair_dof[i]).inspect(context,values)))throw std::runtime_error("consumer constants readback");
                constants<<"[";
                for(unsigned j=0;j<(kind?4u:8u);++j) {
                    if(!std::isfinite(values[j]))throw std::runtime_error("nonfinite consumer constants");
                    if(j)constants<<",";constants<<values[j];
                }
                constants<<"]";
            }
            constants<<"}";
        }
        constants<<"]}\n";constants.close();if(!constants)throw std::runtime_error("consumer constants write");
        // This marker is written last. Its absence means the capture is incomplete.
        std::ofstream manifest(directory/"capture.txt.tmp");
        manifest << "DX11 pair v3; diagnostic 0.9.2\nprecolor: before tagged Alias execution; color: matching consumer t0 afterwards; motion: before tagged execution.\n"
            << "depth: camera-motion PS t8 copied with projection, before tagged Alias execution. Raw device depth, not linear distance.\n"
            << "Consumer role: " << state.pair_boundary[0] << "; snapshot interval ms: "
            << state.pair_time[1]-state.pair_time[0] << "\n"
            << "Two consecutive presented frames; no CPU readback between their GPU snapshots.\n"
            << "jitter.json: same-frame camera VS b1 projection, binding offsets respected; API sign convention pending.\n"
            << "No claim of linear/HDR color or verified motion encoding. Camera cuts not automatically detected.\n";
        manifest.close();if (!manifest) throw std::runtime_error("manifest write");
        std::filesystem::rename(directory/"capture.txt.tmp",directory/"capture.txt");
        state.pair_status = "Saved "+directory.string();
        log::message(log::level::info,state.pair_status.c_str());
    } catch (...) {
        state.pair_status = "Pair save failed; any partial folder without capture.txt must not be analyzed.";
        log::message(log::level::error,state.pair_status.c_str());
    }
    for (unsigned i=0;i<2;++i) { state.pair_color[i]={};state.pair_motion[i]={};state.pair_pre_color[i]={};state.pair_projection[i]={};state.pair_depth[i]={};state.pair_dof[i]={};state.pair_rgbm[i]={}; }
    state.pair_pre_target.Reset();
}

void sync_neural_controls(api::effect_runtime* runtime,DepthState& state){
    const auto t=runtime->find_technique("DX11_Neural.fx","NeuralRendering");
    dx11_enhancement::NeuralSettings s;
    bool valid=t.handle!=0;
    if(valid){
        int32_t model=0,style=0,skin=1;
        valid=runtime->get_annotation_int_from_technique(t,"dx11_nr_model",&model,1) &&
            runtime->get_annotation_int_from_technique(t,"dx11_nr_style",&style,1) &&
            runtime->get_annotation_int_from_technique(t,"dx11_nr_auto_skin",&skin,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_intensity",&s.intensity,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_structure",&s.structure,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_tone",&s.tone,1) &&
            runtime->get_annotation_float_from_technique(t,"dx11_nr_skin",&s.skin,1);
        s.model=static_cast<unsigned>(model);s.style=static_cast<unsigned>(style);s.auto_skin=static_cast<unsigned>(skin);
    }
    state.neural.request(valid && runtime->get_effects_state() && runtime->get_technique_state(t),s);
}
void begin_effects(api::effect_runtime* runtime, api::command_list* commands, api::resource_view, api::resource_view) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->rendered = false;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(commands->get_native());
    sync_neural_controls(runtime,*state);
    // Too late to guarantee a clean image here: discard an unconsumed boundary.
    state->neural_scene_pending.Reset();
    if(state->neural_boundary_frame!=state->frame)state->neural.missing_stage();
    finish_pair(*state,context);
    if (state->trace_remaining && !state->pair_armed && state->trace_matrix_fresh) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(state->trace_matrix.Get(),0,D3D11_MAP_READ,0,&mapped))) {
            const auto* f=static_cast<const float*>(mapped.pData);
            char message[640]{};
            std::snprintf(message,sizeof(message),"[DX11Trace] frame=%llu cameraVSb1_first16=[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g] candidateOnly",
                state->frame,f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7],f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]);
            context->Unmap(state->trace_matrix.Get(),0);
            log::message(log::level::info,message);
        }
    }
    state->motion_rendered = false;
    if (state->motion_enabled && state->motion_fresh) {
        if (state->motion_inspect && !state->pair_armed) {
            state->motion_inspect = false;
            state->motion_inspected = state->motion.inspect_motion(context, state->motion_statistics,
                state->sample_u, state->sample_v) == S_OK;
            if (state->motion_inspected) {
                char message[320]{};
                const auto& s = state->motion_statistics;
                std::snprintf(message, sizeof(message),
                    "[DX11Motion] Raw XY: X=[%.9g,%.9g] Y=[%.9g,%.9g] finite=%llu nonfinite=%llu size=%ux%u format=%u",
                    s.minimum_x,s.maximum_x,s.minimum_y,s.maximum_y,s.finite,s.nonfinite,
                    state->motion.width(), state->motion.height(), static_cast<unsigned>(state->motion.format()));
                log::message(log::level::info, message);
                std::snprintf(message, sizeof(message),
                    "[DX11Motion] Selected pixel (%u,%u): raw XY=(%.9g,%.9g). Display gain/boost not applied.",
                    s.sample_column,s.sample_row,s.sample_x,s.sample_y);
                log::message(log::level::info, message);
            } else log::message(log::level::warning, "[DX11Motion] Inspection failed or no finite samples.");
        }
        state->motion_status = state->motion.render_motion(context, state->motion_gain, state->magnitude,
            state->uv_to_pixels, state->emphasize_small_motion);
        state->motion_rendered = SUCCEEDED(state->motion_status);
    }
    if (!state->enabled || !state->fresh) return;
    if (state->inspect_requested && !state->pair_armed) {
        state->inspect_requested = false;
        const HRESULT result = state->preview.inspect(context, state->statistics);
        state->inspected = result == S_OK;
        if (state->inspected) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                "[DX11Depth] Raw values min=%.9g max=%.9g finite=%llu nonfinite=%llu draws=%u capture=%d",
                state->statistics.minimum, state->statistics.maximum, state->statistics.finite,
                state->statistics.nonfinite, state->draws_this_frame, state->occurrence);
            log::message(log::level::info, message);
            if (state->fit_requested && state->statistics.maximum > state->statistics.minimum) {
                state->minimum = state->statistics.minimum;
                state->maximum = state->statistics.maximum;
                state->exponent = 1;
            }
        } else log::message(log::level::warning, "[DX11Depth] Raw-value inspection failed or had no finite samples.");
        state->fit_requested = false;
    }
    state->status = state->preview.render(context, state->exponent, state->invert,
        state->minimum, state->maximum, state->gradient);
    state->rendered = SUCCEEDED(state->status);
    if (state->rendered && !state->first_image_logged) {
        log::message(log::level::info, "[DX11Depth] Grayscale preview generated; open ReShade > DX11 Depth Diagnostic.");
        state->first_image_logged = true;
    }
}
void sync_dlaa_controls(api::effect_runtime* runtime,DepthState& state) {
    const auto technique=runtime->find_technique("DX11_DLAA.fx","DLAA");
    int32_t preset=0;
    const bool valid=technique.handle && runtime->get_annotation_int_from_technique(technique,"dx11_dlaa_preset",&preset,1);
    const bool enabled=valid && runtime->get_effects_state() && runtime->get_technique_state(technique);
    if(!state.live_request.update(enabled,static_cast<uint32_t>(preset)))return;
    state.neural.reset(); // DLAA changes invalidate the neural temporal input history.
    state.live.stop();state.live_target.Reset();state.live_camera=state.live_submitted=false;
    state.live_enabled=state.live_request.enabled;
    state.live_last_frame=UINT64_MAX;state.live_last_time=0;state.live_frames=0;
    state.live_verifier.reset();state.live_cpu_fresh=false;state.live_cpu_fast=0;
    state.live_perf_count=0;state.live_perf={};state.live_perf_projection=state.live_perf_motion=0;
    state.live_status=state.live_enabled?"Waiting for gameplay.":"Off.";
    char line[192]{};
    std::snprintf(line,sizeof(line),"[DX11DLAA] ReShade control: %s; requested preset=%u (%s).",
        state.live_enabled?"on":"off",state.live_request.preset,dx11_enhancement::dlaa_preset_name(state.live_request.preset));
    log::message(log::level::info,line);
}
void end_frame(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    if(state->live.pending())live_failure(*state,"matching output consumer missing");
    state->live_camera=state->live_submitted=false;state->live_target.Reset();state->live_motion_source.Reset();
    diagnostics::FrameObservation observation;
    observation.frame=state->frame;observation.time_ms=GetTickCount64();
    observation.width=state->motion.width();observation.height=state->motion.height();
    observation.depth_format=static_cast<uint32_t>(state->preview.format());
    observation.motion_format=static_cast<uint32_t>(state->motion.format());
    observation.enabled=state->enabled && state->motion_enabled;
    // Diagnostic continuity only: no claim about color, exposure, camera cuts,
    // object coverage or numerical validity from successful texture copies.
    observation.usable=state->fresh && state->motion_fresh &&
        state->preview.width()==observation.width && state->preview.height()==observation.height;
    const auto previous=state->history_decision;
    state->history_decision=state->history.observe(observation);
    const auto& decision=state->history_decision;
    if (decision.reset) state->last_reset=decision.reason;
    if (decision.reset || previous.usable!=decision.usable) {
        char message[256]{};
        std::snprintf(message,sizeof(message),"[DX11History] frame=%llu generation=%llu candidateUsable=%u reset=%u reason=%s; diagnostic metadata only",
            state->frame,decision.generation,decision.usable ? 1u:0u,decision.reset ? 1u:0u,
            diagnostics::reset_name(decision.reason));
        log::message(log::level::info,message);
    }
    state->fresh = state->rendered = false;
    state->motion_fresh = state->motion_rendered = false;
    state->pending_motion.Reset();
    state->draws_last_frame = state->draws_this_frame;
    state->draws_this_frame = 0;
    if (state->trace_remaining && state->frame>state->trace_armed_frame) --state->trace_remaining;
    ++state->frame;
    if(state->pair_armed) state->pair_trigger.boundary(GetTickCount64());
    state->pair_projection_status=state->pair_depth_status=S_FALSE;
    state->trace_executes=state->trace_consumers=0;
    state->trace_output.Reset();state->trace_matrix_fresh=false;
    state->pair_fresh = false;
    state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
    sync_dlaa_controls(runtime,*state);
    sync_neural_controls(runtime,*state);
}
void reset_swapchain(api::swapchain* swapchain, bool) {
    auto* state = swapchain->get_device()->get_private_data<DepthState>();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->live.stop();state->live_target.Reset();state->live_camera=state->live_submitted=false;
    state->neural.reset();state->live_displayed_frame=UINT64_MAX;
    state->neural_scene_pending.Reset();state->neural_camera_captured=false;
    state->neural_camera_frame=state->neural_scene_frame=state->neural_boundary_frame=UINT64_MAX;
    state->live_last_frame=UINT64_MAX;state->live_status="Display reset; DLAA helper will restart if enabled.";
    state->history.invalidate(diagnostics::ResetReason::display_reset);
    state->trace_remaining=0;state->trace_matrix.Reset();state->trace_output.Reset();
    state->fresh = state->rendered = false;
    if (state->pair_armed) state->pair_status = "Pair cancelled: swapchain reset (display resources changed).";
    state->pair_armed = state->pair_fresh = false;
    state->pair_count = 0;
    for (unsigned i=0;i<2;++i) { state->pair_color[i]={};state->pair_motion[i]={};state->pair_pre_color[i]={};state->pair_projection[i]={};state->pair_depth[i]={};state->pair_dof[i]={};state->pair_rgbm[i]={}; }
    state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
    state->preview.reset();
    state->pending_motion.Reset();
    state->motion.reset();
    state->motion_fresh = state->motion_rendered = state->motion_inspected = false;
    state->motion_inspect = true;
    state->inspected = false;
    state->inspect_requested = state->fit_requested = true;
}
void dlaa_overlay(api::effect_runtime* runtime) {
    auto* state=runtime->get_device()->get_private_data<DepthState>();
    if(!state){ImGui::TextUnformatted("DLAA is unavailable in this application.");return;}
    std::lock_guard lock(state->mutex);
    ImGui::TextWrapped("%s",state->live_status.c_str());
    ImGui::TextWrapped("%s",state->neural.status().c_str());
    if(state->live_request.enabled && !state->live_enabled)
        ImGui::TextWrapped("Uncheck DLAA in Home, then check it again to retry.");
}
#ifdef DX11_DIAGNOSTIC_UI
void overlay(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) { ImGui::TextUnformatted("Alien: Isolation depth adapter is inactive in this program."); return; }
    std::lock_guard lock(state->mutex);
    ImGui::TextWrapped("%s",state->live_status.c_str());
    ImGui::Text("DLAA frames displayed: %llu",state->live_frames);
    ImGui::Text("Camera upload cache frames: %llu",state->live_cpu_fast);
    ImGui::Separator();
    ImGui::TextUnformatted("Raw depth diagnostic - not linear distance");
    ImGui::Checkbox("Capture depth", &state->enabled);
    ImGui::Checkbox("Invert grayscale", &state->invert);
    ImGui::Checkbox("Show test gradient (not game depth)", &state->gradient);
    if (ImGui::Button("Inspect values and fit contrast")) state->inspect_requested = state->fit_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset contrast")) { state->minimum = 0; state->maximum = 1; state->exponent = 1; }
    ImGui::InputFloat("Black value", &state->minimum, 0, 0, "%.9f");
    ImGui::InputFloat("White value", &state->maximum, 0, 0, "%.9f");
    if (ImGui::InputInt("Capture occurrence (0 = last)", &state->occurrence)) {
        if (state->occurrence < 0) state->occurrence = 0;
        state->fresh = state->rendered = state->inspected = false;
        state->inspect_requested = state->fit_requested = true;
    }
    ImGui::Text("Matching draws last frame: %u", state->draws_last_frame);
    if (state->inspected) {
        ImGui::Text("Last inspected raw range: %.9g to %.9g", state->statistics.minimum, state->statistics.maximum);
        if (state->statistics.minimum == state->statistics.maximum)
            ImGui::TextWrapped("The inspected depth snapshot is flat: all finite pixels have the same value. Contrast cannot recover shapes. Try inspecting during gameplay or selecting another capture occurrence.");
    }
    ImGui::SliderFloat("Contrast exponent", &state->exponent, 0.01f, 4.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::Text("Camera-motion shader: %s | observed draws: %llu",
        state->shader_found ? "identified" : "not found yet", state->passes);
    if (!state->enabled) ImGui::TextUnformatted("Capture disabled. No depth image is being copied.");
    else if (state->rendered) {
        ImGui::Text("%u x %u | current frame | scene-depth validation pending",
            state->preview.width(), state->preview.height());
        if (state->gradient) ImGui::TextUnformatted("TEST GRADIENT ONLY - this image is not scene depth.");
        const float width = (ImGui::GetContentRegionAvail().x > 32) ? ImGui::GetContentRegionAvail().x : 32;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state->preview.image())),
            ImVec2(width, width * static_cast<float>(state->preview.height()) / state->preview.width()));
    } else {
        ImGui::TextUnformatted("No current-frame depth preview. Load a save and enable game motion blur.");
        if (FAILED(state->status)) ImGui::Text("Capture/render status: 0x%08X", static_cast<unsigned>(state->status));
    }
    ImGui::TextWrapped("Look for walls, doors and objects lining up with the scene. Contrast changes only this preview. Motion candidates are in the separate DX11 Motion Diagnostic panel.");
}

void motion_overlay(api::effect_runtime* runtime) {
    auto* state = runtime->get_device()->get_private_data<DepthState>();
    if (!state) { ImGui::TextUnformatted("Alien: Isolation motion adapter is inactive."); return; }
    std::lock_guard lock(state->mutex);
    ImGui::Text("Candidate continuity: %s | history generation: %llu",
        state->history_decision.usable ? "available" : "unavailable",state->history_decision.generation);
    ImGui::Text("Last reset: %s",diagnostics::reset_name(state->last_reset));
    ImGui::TextWrapped("Tracks depth/motion availability only. Camera cuts and full reconstruction readiness are not yet detected.");
    if (ImGui::Button("Request diagnostic history reset"))
        state->history.invalidate(diagnostics::ResetReason::manual);
    ImGui::TextWrapped("Motion candidate: camera-motion output sampled at the DoF/RGBM boundary. Direction, units and moving-object coverage still need validation.");
    ImGui::Checkbox("Capture motion", &state->motion_enabled);
    ImGui::Checkbox("Movement strength (grayscale)", &state->magnitude);
    ImGui::Checkbox("Emphasize small motion (display only)", &state->emphasize_small_motion);
    ImGui::Checkbox("Assume UV units and scale by image size (experimental)", &state->uv_to_pixels);
    ImGui::SliderFloat("Display gain", &state->motion_gain, 0.01f, 10000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (!state->pair_armed && ImGui::Button("Capture pre/post-TAA pair in 3 seconds")) {
        state->pair_armed = true;state->pair_fresh = false;state->pair_count = 0;
        state->pair_pre_fresh=state->pair_projection_fresh=false;state->pair_pre_target.Reset();
        state->pair_due = GetTickCount64()+3000;
        state->pair_trigger.arm(state->pair_due);
        state->motion_enabled = true;
        state->pair_status = "Armed. Close overlay and turn slowly; saving may briefly pause the game.";
        log::message(log::level::info,"[DX11Pair] User armed two-frame capture with three-second delay.");
    }
    ImGui::TextWrapped("%s",state->pair_status.c_str());
    if (ImGui::Button("Trace TAA order and camera constants (32 frames)")) {
        state->trace_remaining=32;
        state->trace_armed_frame=state->frame;
        log::message(log::level::info,"[DX11Trace] User requested 32-frame trace. Matrix values are candidates, not validated jitter.");
    }
    ImGui::Text("Trace frames remaining: %u",state->trace_remaining);
    if (ImGui::Button("Inspect motion values")) state->motion_inspect = true;
    if (state->motion_inspected) {
        const auto& s = state->motion_statistics;
        ImGui::Text("Last snapshot: X %.9g to %.9g | Y %.9g to %.9g", s.minimum_x,s.maximum_x,s.minimum_y,s.maximum_y);
        ImGui::Text("Finite vectors: %llu | invalid: %llu", s.finite,s.nonfinite);
        ImGui::Text("Selected pixel (%u,%u): raw X %.9g | Y %.9g",
            s.sample_column,s.sample_row,s.sample_x,s.sample_y);
    }
    if (!state->motion_enabled) ImGui::TextUnformatted("Motion capture disabled.");
    else if (!state->motion_rendered) {
        ImGui::TextUnformatted("No current-frame motion image. Load a save; keep motion blur enabled.");
        if (FAILED(state->motion_status)) ImGui::Text("Capture/render status: 0x%08X", static_cast<unsigned>(state->motion_status));
    } else {
        ImGui::Text("%u x %u | current-frame candidate | format %u", state->motion.width(), state->motion.height(), static_cast<unsigned>(state->motion.format()));
        const float width = ImGui::GetContentRegionAvail().x > 32 ? ImGui::GetContentRegionAvail().x : 32;
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state->motion.image())),
            ImVec2(width, width * static_cast<float>(state->motion.height()) / state->motion.width()));
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto origin = ImGui::GetItemRectMin();
            const auto size = ImGui::GetItemRectSize();
            const auto mouse = ImGui::GetMousePos();
            state->sample_u = (mouse.x - origin.x) / size.x;
            state->sample_v = (mouse.y - origin.y) / size.y;
            state->motion_inspect = true;
        }
    }
    ImGui::TextWrapped("Click an object in this preview to inspect that screen position on the next captured frame. Values above are a snapshot, not live. Small-motion emphasis makes tiny values easier to see; it also emphasizes noise. Raw data is unchanged.");
    ImGui::TextWrapped("Color view: neutral gray is zero; red increases with positive X and green with positive Y. Strength view: black is zero and brighter means larger magnitude. Magenta marks invalid numbers. Gain affects the display only.");
}
#endif
}
void diagnostics::register_depth_events() {
    register_event<addon_event::map_buffer_region>(camera_map);
    register_event<addon_event::unmap_buffer_region>(camera_unmap);
    register_event<addon_event::init_device>(init_device);
    register_event<addon_event::destroy_device>(destroy_device);
    register_event<addon_event::init_command_list>(init_commands);
    register_event<addon_event::destroy_command_list>(destroy_commands);
    register_event<addon_event::init_pipeline>(init_pipeline);
    register_event<addon_event::bind_pipeline>(bind_pipeline);
    register_event<addon_event::dispatch>(trace_dispatch);
    register_event<addon_event::copy_resource>(trace_copy);
    register_event<addon_event::copy_texture_region>(neural_copy_region);
    register_event<addon_event::clear_render_target_view>(neural_clear);
    register_event<addon_event::draw_or_dispatch_indirect>(neural_indirect);
    register_event<addon_event::execute_secondary_command_list>(trace_secondary);
    register_event<addon_event::draw>(draw);
    register_event<addon_event::draw_indexed>(draw_indexed);
    register_event<addon_event::reshade_begin_effects>(begin_effects);
    register_event<addon_event::reshade_present>(end_frame);
    register_event<addon_event::destroy_swapchain>(reset_swapchain);
    register_overlay("DLAA status", dlaa_overlay);
#ifdef DX11_DIAGNOSTIC_UI
    register_overlay("DX11 Depth Diagnostic", overlay);
    register_overlay("DX11 Motion Diagnostic", motion_overlay);
#endif
}

