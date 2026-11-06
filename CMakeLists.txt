cmake_minimum_required(VERSION 3.25)
project(NeuralAliasAbundance VERSION 0.2.0 LANGUAGES CXX)
# Reconciled 2026-10-23 (task C1, MASTER-PLAN.md). Owner: Claude (single editor).
#  - Every recovered source file is built (the Sept 23 file left 17 of them out).
#  - Every NVIDIA SDK is OPTIONAL: missing SDK = that feature compiled out, build
#    stays green. SDK locations: -DNA_SDK_ROOT=<folder> (default: Cort's
#    Documents\NeuralAliasAbundance-SDKs, or $ENV{NA_SDK_ROOT}).
#  - Tests live in tests/cpu (Astra, add_subdirectory) and tests/host (Claude).
#  - Non-Windows builds get the portable core + CPU tests only (CI Linux job).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_library(abundance_options INTERFACE)
target_include_directories(abundance_options INTERFACE src)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(abundance_options INTERFACE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(abundance_options INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
else()
  target_compile_options(abundance_options INTERFACE -Wall -Wextra -Werror -pedantic)
endif()

# ---------------------------------------------------------------- portable core
add_library(abundance_core STATIC
  src/core/frame_contract.cpp
  src/core/motion_arbiter.cpp)
target_link_libraries(abundance_core PUBLIC abundance_options)

# Astra's CPU regression suite (tests/cpu/CMakeLists.txt is Astra's; it links
# `host_observe` when that target exists, so it is added after the host libs
# on Windows and here on other platforms).
if(NOT WIN32)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpu/CMakeLists.txt")
    add_subdirectory(tests/cpu)
  endif()
  message(STATUS "Non-Windows: portable core + CPU tests only (DX12 host needs Windows/MSVC x64)")
  return()
endif()

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The DX12 host requires MSVC and x64.")
endif()
enable_language(C)

# ------------------------------------------------------------ SDK discovery
# All optional. Never vendored (license): NGX/DLSS SDK, Optical Flow SDK, pclstats.
if(NOT NA_SDK_ROOT)
  if(DEFINED ENV{NA_SDK_ROOT} AND EXISTS "$ENV{NA_SDK_ROOT}")
    set(NA_SDK_ROOT "$ENV{NA_SDK_ROOT}")
  else()
    foreach(cand
        "C:/Users/Cort/Documents/NeuralAliasAbundance-SDKs"
        "$ENV{USERPROFILE}/Documents/NeuralAliasAbundance-SDKs")
      if(EXISTS "${cand}")
        set(NA_SDK_ROOT "${cand}")
        break()
      endif()
    endforeach()
  endif()
endif()
set(NA_SDK_ROOT "${NA_SDK_ROOT}" CACHE PATH "Folder holding the local NVIDIA SDKs (see third_party/README.md)")

set(NA_NGX_SDK_INCLUDE "" CACHE PATH "NVIDIA NGX/DLSS SDK include dir (nvsdk_ngx.h)")
if(NOT NA_NGX_SDK_INCLUDE AND EXISTS "${NA_SDK_ROOT}/nvidia-dlss-sdk/include/nvsdk_ngx.h")
  set(NA_NGX_SDK_INCLUDE "${NA_SDK_ROOT}/nvidia-dlss-sdk/include")
endif()
set(NA_NVOF_INCLUDE "" CACHE PATH "NVIDIA Optical Flow SDK interface dir (nvOpticalFlowD3D12.h)")
if(NOT NA_NVOF_INCLUDE AND EXISTS "${NA_SDK_ROOT}/optical-flow/nvOpticalFlowD3D12.h")
  set(NA_NVOF_INCLUDE "${NA_SDK_ROOT}/optical-flow")
endif()
set(NA_PCL_INCLUDE "" CACHE PATH "Reflex PCL stats dir (pclstats.h)")
if(NOT NA_PCL_INCLUDE AND EXISTS "${NA_SDK_ROOT}/pclstats/pclstats.h")
  set(NA_PCL_INCLUDE "${NA_SDK_ROOT}/pclstats")
endif()
set(NA_HAVE_NGX_SDK OFF)
set(NA_HAVE_NVOF OFF)
set(NA_HAVE_PCL_STATS OFF)
if(EXISTS "${NA_NGX_SDK_INCLUDE}/nvsdk_ngx.h")
  set(NA_HAVE_NGX_SDK ON)
endif()
if(EXISTS "${NA_NVOF_INCLUDE}/nvOpticalFlowD3D12.h")
  set(NA_HAVE_NVOF ON)
endif()
if(EXISTS "${NA_PCL_INCLUDE}/pclstats.h")
  set(NA_HAVE_PCL_STATS ON)
endif()
message(STATUS "NeuralAlias SDKs: root='${NA_SDK_ROOT}'  NGX/DLSS=${NA_HAVE_NGX_SDK}  OpticalFlow=${NA_HAVE_NVOF}  PCL/Reflex=${NA_HAVE_PCL_STATS}  NVAPI=vendored(MIT)")

# ------------------------------------------------------------- third party
# Pins and licenses: third_party/README.md. Own flags: not our /W4 /WX.
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)

add_library(imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_IMPL_WIN32_DISABLE_GAMEPAD) # the overlay feeds gamepad via its XInput hook
target_compile_options(imgui PRIVATE /W3)
target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler)

add_library(nvapi_headers INTERFACE) # MIT (github.com/NVIDIA/nvapi); runtime is the driver's nvapi64.dll
target_include_directories(nvapi_headers SYSTEM INTERFACE third_party/nvapi)

# ------------------------------------------------------ host building blocks
# Small libraries kept separate because tests link them in isolation.
add_library(host_hooking STATIC
  src/host_dx12/hooking/vtable_hook.cpp
  src/host_dx12/hooking/variant_registry.cpp)
target_link_libraries(host_hooking PUBLIC abundance_options d3d12 PRIVATE minhook)
# Microsoft Detours (MIT, v4.0.1, unmodified; third_party/detours): opt-in hooking engine
# ([hooking] engine=detours). Vendor code, built without our /W4 /WX. MSVC only.
if(MSVC)
  add_library(detours STATIC
    third_party/detours/src/detours.cpp third_party/detours/src/modules.cpp third_party/detours/src/disasm.cpp
    third_party/detours/src/image.cpp third_party/detours/src/creatwth.cpp third_party/detours/src/disolx86.cpp
    third_party/detours/src/disolx64.cpp third_party/detours/src/disolia64.cpp third_party/detours/src/disolarm.cpp
    third_party/detours/src/disolarm64.cpp)
  target_include_directories(detours SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/src")
  target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN)
  target_link_libraries(host_hooking PRIVATE detours)
  target_compile_definitions(host_hooking PRIVATE NA_HAVE_DETOURS)
endif()
# P4-4 render-small S1/S2 (Astra): game-facing window size and input mapping.
add_subdirectory(src/host_dx12/native_present)
# P4-4 slider route: read-time render-scale substitution (Fable; installed by host_boot on request).
add_library(host_scale STATIC src/host_dx12/scale/settings_read_hook.cpp src/host_dx12/scale/live_driver.cpp)
target_link_libraries(host_scale PUBLIC abundance_options host_hooking)

add_library(host_observe STATIC src/host_dx12/host_observe.cpp)
target_link_libraries(host_observe PUBLIC abundance_options)

# Layer 2 owns independent reduced scene allocations; the game's native
# resources remain the bypass. Recognition/binding integration is separate.
add_library(host_scene_scale STATIC src/host_dx12/scale/scene_group.cpp src/host_dx12/scale/depth_restore.cpp
  src/host_dx12/scale/gpu_constants.cpp)
target_link_libraries(host_scene_scale PUBLIC abundance_options d3d12 d3dcompiler)

# C8a: H1 observation producers (device/queue hooks emitting ABI events).
add_library(host_observe_producers STATIC
  src/host_dx12/observe/producers.cpp
  src/host_dx12/observe/d3d12_slots_check.cpp)
target_sources(host_observe_producers PRIVATE src/host_dx12/observe/shader_dimensions.cpp)
target_link_libraries(host_observe_producers PUBLIC abundance_options host_observe host_hooking d3d12 d3dcompiler)

add_library(host_settings STATIC src/host_dx12/settings.cpp)
target_link_libraries(host_settings PUBLIC abundance_options PRIVATE shell32 ole32)

add_library(host_capture STATIC src/host_dx12/capture/frame_capture.cpp src/host_dx12/capture/j2_capture.cpp src/host_dx12/capture/capture_session.cpp)
target_link_libraries(host_capture PUBLIC abundance_options d3d12 dxgi PRIVATE shell32 ole32)

add_library(core_gpu STATIC src/core/motion_arbiter_gpu.cpp src/core/camera_fit_gpu.cpp src/core/jitter_probe_gpu.cpp)
target_link_libraries(core_gpu PUBLIC abundance_core d3d12 d3dcompiler)

# Optical flow: real wrapper when the SDK headers exist, otherwise a stub that
# reports "not available" so live_flow latches off with a reason.
if(NA_HAVE_NVOF)
  add_library(host_nvof STATIC src/host_dx12/nvof/flow_expand.cpp src/host_dx12/nvof/nvof_flow.cpp)
  target_include_directories(host_nvof SYSTEM PRIVATE "${NA_NVOF_INCLUDE}")
  target_compile_definitions(host_nvof PUBLIC NA_HAVE_NVOF)
else()
  add_library(host_nvof STATIC src/host_dx12/nvof/flow_expand.cpp src/host_dx12/nvof/nvof_flow_stub.cpp)
endif()
target_link_libraries(host_nvof PUBLIC abundance_options d3d12 d3dcompiler)

# P4-0c: AMD FidelityFX optical flow (compute; inline route and NVOFA fallback). Shaders are
# compiled by DXC at build time (cmake/ffx_flow_shaders.cmake); without DXC it is a stub.
include(cmake/ffx_flow_shaders.cmake)
include(cmake/xegtao_shaders.cmake)
include(cmake/donut_shaders.cmake)
include(cmake/nvrhi.cmake)
add_library(host_ffx_flow STATIC src/host_dx12/ffx_flow/ffx_flow_shaders.cpp src/host_dx12/ffx_flow/ffx_flow.cpp)
target_link_libraries(host_ffx_flow PUBLIC abundance_options d3d12)
if(NA_HAVE_FFX_FLOW)
  target_compile_definitions(host_ffx_flow PUBLIC NA_HAVE_FFX_FLOW)
  target_include_directories(host_ffx_flow PRIVATE "${CMAKE_BINARY_DIR}/generated/ffx_flow")
  add_dependencies(host_ffx_flow ffx_flow_shaders)
endif()

# ---------------------------------------------------------------- host_dx12
# The in-game host. These modules reference each other freely (overlay <->
# ngx_observe <-> nr/fg/motion <-> latency <-> tlou1), so they form one library.
set(NA_GAME_ADAPTER_LIB "" CACHE STRING "CMake target implementing game_adapter.h; empty = host stub (no adapter)")
add_library(host_dx12 STATIC
  src/providers/discovery/upscaler_menu.cpp
  src/host_dx12/ngx_observe/ngx_observe.cpp
  src/host_dx12/ngx_observe/gpu_timing.cpp
  src/host_dx12/overlay/overlay.cpp
  src/host_dx12/overlay/usage_sampler.cpp
  src/host_dx12/overlay/dlss_debug.cpp
  src/host_dx12/overlay/menu.cpp
  src/host_dx12/overlay/route_a_panel.cpp
  src/host_dx12/overlay/presenter.cpp
  src/host_dx12/overlay/freeze_watchdog.cpp
  src/host_dx12/fg/camera.cpp
  src/host_dx12/fg/frame_gen.cpp
  src/host_dx12/fg/preset_profile.cpp
  src/host_dx12/nr/nr_pass.cpp
  src/host_dx12/nr/fg_composition.cpp
  src/host_dx12/nr/fg_color.cpp
  src/host_dx12/nr/sl_consumer.cpp
  src/host_dx12/nr/native_fg.cpp
  src/host_dx12/nr/fg_states.cpp
  src/host_dx12/j2/live_dlaa.cpp
  src/host_dx12/effects/donut.cpp
  src/host_dx12/effects/donut_native.cpp
  src/host_dx12/effects/xegtao.cpp
  src/host_dx12/effects/xegtao_native.cpp
  src/host_dx12/j2/projection_window.cpp
  src/host_dx12/j2/j2_flow.cpp
  src/host_dx12/j2/compute_flow.cpp
  src/host_dx12/j2/game_motion.cpp
  src/host_dx12/j2/nis_sharpen.cpp
  src/host_dx12/j2/nis_scaler.cpp
  src/host_dx12/j2/native_hud.cpp
  src/host_dx12/j2/hud_unblend.cpp
  src/host_dx12/j2/hud_picture.cpp
  src/host_dx12/scale/scoped_bindings.cpp
  src/host_dx12/scale/native_bindings.cpp
  src/host_dx12/scale/auto_scene.cpp
    src/host_dx12/scale/scoped_transfers.cpp
    src/host_dx12/scale/scene_frame.cpp
  src/host_dx12/motion/live_flow.cpp
  src/host_dx12/latency/nvidia_latency.cpp
  src/host_dx12/latency/pcl_stats.cpp
  src/host_dx12/adapters/tlou1/apply.cpp
  src/host_dx12/adapters/tsushima/apply.cpp
  src/host_dx12/adapters/unreal/cvar_probe.cpp
  src/host_dx12/sl_boundary/sl_boundary.cpp
  src/host_dx12/sl_boundary/sl_dlss_mode.cpp
  src/host_dx12/adapters/unreal/screen_percentage.cpp
  src/host_dx12/adapters/mode_control.cpp
  src/host_dx12/adapters/tlou1/timing.cpp
  src/host_dx12/host_boot/host_boot.cpp
  src/host_dx12/host_boot/session.cpp)
if(NA_GAME_ADAPTER_LIB)
  target_link_libraries(host_dx12 PUBLIC "${NA_GAME_ADAPTER_LIB}")
  message(STATUS "Game adapter: ${NA_GAME_ADAPTER_LIB}")
else()
  target_sources(host_dx12 PRIVATE src/host_dx12/host_boot/adapter_stub.cpp)
  message(STATUS "Game adapter: none (host stub)")
endif()
# NVIDIA Image Scaling (MIT, vendored): sharpen-only stage of J2's Sharpness slider.
if(NA_HAVE_XEGTAO)
  target_compile_definitions(host_dx12 PUBLIC NA_HAVE_XEGTAO)
  target_include_directories(host_dx12 PRIVATE "${CMAKE_BINARY_DIR}/generated/xegtao")
  add_dependencies(host_dx12 xegtao_shaders)
endif()
if(NA_HAVE_DONUT)
  target_compile_definitions(host_dx12 PUBLIC NA_HAVE_DONUT)
  target_include_directories(host_dx12 PRIVATE "${CMAKE_BINARY_DIR}/generated/donut")
  add_dependencies(host_dx12 donut_shaders)
endif()

include(cmake/taskflow.cmake)
add_library(host_background STATIC src/host_dx12/background/work.cpp)
set_target_properties(host_background PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(host_background PUBLIC abundance_options host_observe_producers PRIVATE na_taskflow_headers)

add_library(host_gpu_memory STATIC src/host_dx12/gpu/mem_alloc.cpp)
target_link_libraries(host_gpu_memory PUBLIC abundance_options host_observe_producers d3d12ma d3d12 dxgi)
add_library(host_gpu_services STATIC src/host_dx12/gpu/services.cpp)
target_link_libraries(host_gpu_services PUBLIC abundance_options host_settings host_observe_producers host_gpu_memory d3d12)
if(NA_HAVE_NVRHI)
  target_compile_definitions(host_gpu_services PUBLIC NA_HAVE_NVRHI)
  target_link_libraries(host_gpu_services PRIVATE nvrhi nvrhi_d3d12)
endif()
target_link_libraries(host_dx12 PUBLIC host_gpu_services host_background)
target_include_directories(host_dx12 SYSTEM PRIVATE third_party/nis)
target_link_libraries(host_dx12
  PUBLIC abundance_options d3d12ma host_hooking host_native_present host_scale host_settings host_capture host_nvof host_ffx_flow core_gpu imgui nvapi_headers host_dlss host_upscalers host_observe_producers host_scene_scale
         d3d12 dxgi d3dcompiler user32 gdi32 shell32 ole32 bcrypt dbghelp winmm)
if(NA_HAVE_NGX_SDK)
  target_compile_definitions(host_dx12 PUBLIC NA_HAVE_NGX_SDK)
  target_include_directories(host_dx12 SYSTEM PUBLIC "${NA_NGX_SDK_INCLUDE}")
endif()
if(NA_HAVE_PCL_STATS)
  target_compile_definitions(host_dx12 PRIVATE NA_HAVE_PCL_STATS)
  target_include_directories(host_dx12 SYSTEM PRIVATE "${NA_PCL_INCLUDE}")
endif()

# ---------------------------------------------------------------- host_dlss
# C6: generic DLSS Super Resolution / DLAA backend. Game-independent; needs
# the NGX SDK headers to do anything (stub otherwise). Our OWN NGX instance
# additionally needs the NGX static library from the local SDK folder (never
# committed; the public-headers CI job has none, so it builds Attach-only).
set(NA_NGX_LIB "" CACHE FILEPATH "NVIDIA NGX static library (nvsdk_ngx_s.lib, /MT)")
if(NOT NA_NGX_LIB AND EXISTS "${NA_SDK_ROOT}/nvidia-dlss-sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  set(NA_NGX_LIB "${NA_SDK_ROOT}/nvidia-dlss-sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
endif()
set(NA_NGX_LIB_DEBUG "${NA_SDK_ROOT}/nvidia-dlss-sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib")
set(NA_DLSS_DLL "" CACHE FILEPATH "nvngx_dlss.dll copied beside DLSS executables (tests)")
if(NOT NA_DLSS_DLL AND EXISTS "${NA_SDK_ROOT}/nvidia-dlss-sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll")
  set(NA_DLSS_DLL "${NA_SDK_ROOT}/nvidia-dlss-sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll")
endif()
set(NA_HAVE_NGX_LIB OFF)
if(NA_HAVE_NGX_SDK AND NA_NGX_LIB AND EXISTS "${NA_NGX_LIB}")
  set(NA_HAVE_NGX_LIB ON)
endif()
# DRED report (Windows' device-removed diagnostics; diag/dred.hpp). Its own library: the
# DLSS backend and the host both report a removal.
# AMD D3D12 Memory Allocator (MIT, v3.0.1, unmodified; third_party/d3d12ma): vendor code,
# built without our /W4 /WX. Used through gpu/mem_alloc.hpp.
add_library(d3d12ma STATIC third_party/d3d12ma/src/D3D12MemAlloc.cpp)
target_include_directories(d3d12ma SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/d3d12ma/include")
target_link_libraries(d3d12ma PUBLIC d3d12 dxgi)
# AMD FidelityFX Breadcrumbs (MIT, FidelityFX SDK v1.1.4, unmodified; third_party/ffx-breadcrumbs):
# vendor code, built without our /W4 /WX. Our own DX12 backend is in diag/gpu_breadcrumbs.cpp.
add_library(ffx_breadcrumbs STATIC
  third_party/ffx-breadcrumbs/src/components/breadcrumbs/ffx_breadcrumbs.cpp
  third_party/ffx-breadcrumbs/src/shared/ffx_breadcrumbs_list.cpp
  third_party/ffx-breadcrumbs/src/shared/ffx_object_management.cpp
  third_party/ffx-breadcrumbs/src/shared/ffx_assert.cpp
  third_party/ffx-breadcrumbs/src/shared/ffx_message.cpp)
target_include_directories(ffx_breadcrumbs SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-breadcrumbs/include"
                           PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-breadcrumbs/src/shared")
target_compile_definitions(ffx_breadcrumbs PRIVATE UNICODE _UNICODE)
add_library(host_diag STATIC src/host_dx12/diag/dred.cpp src/host_dx12/diag/gpu_breadcrumbs.cpp)
target_link_libraries(host_diag PUBLIC abundance_options d3d12 PRIVATE ffx_breadcrumbs)
add_library(host_dlss STATIC src/host_dx12/dlss/dlss_backend.cpp)
target_link_libraries(host_dlss PUBLIC abundance_options abundance_core d3d12 host_diag)
if(NA_HAVE_NGX_SDK)
  target_compile_definitions(host_dlss PUBLIC NA_HAVE_NGX_SDK)
  target_include_directories(host_dlss SYSTEM PUBLIC "${NA_NGX_SDK_INCLUDE}")
endif()
if(NA_HAVE_NGX_LIB)
  target_sources(host_dlss PRIVATE src/host_dx12/dlss/dlss_own_ngx.cpp)
  target_compile_definitions(host_dlss PUBLIC NA_HAVE_NGX_LIB)
  if(EXISTS "${NA_NGX_LIB_DEBUG}")
    target_link_libraries(host_dlss PUBLIC "$<IF:$<CONFIG:Debug>,${NA_NGX_LIB_DEBUG},${NA_NGX_LIB}>")
  else()
    target_link_libraries(host_dlss PUBLIC "${NA_NGX_LIB}")
  endif()
endif()
set(NA_XESS_INCLUDE "" CACHE PATH "Intel XeSS SDK inc dir (xess/xess_d3d12.h), ABI cross-check only")
if(NOT NA_XESS_INCLUDE AND EXISTS "${NA_SDK_ROOT}/xess/inc/xess/xess_d3d12.h")
  set(NA_XESS_INCLUDE "${NA_SDK_ROOT}/xess/inc" CACHE PATH "Intel XeSS SDK inc dir (xess/xess_d3d12.h), ABI cross-check only" FORCE)
endif()
message(STATUS "DLSS backend (C6): headers=${NA_HAVE_NGX_SDK}  own NGX instance=${NA_HAVE_NGX_LIB}  nvngx_dlss.dll='${NA_DLSS_DLL}'")

# ------------------------------------------------------------- host_upscalers
# C7: FSR 3.1+ (FidelityFX API, MIT headers vendored in third_party/ffx-api)
# and XeSS (our own ABI declarations; Intel's headers are only used, when
# present, to cross-check them) interception. Both feed the C6 backend.
add_library(host_upscalers STATIC
  src/host_dx12/upscalers/module_watch.cpp
  src/host_dx12/upscalers/ffx_intercept.cpp
  src/host_dx12/upscalers/xess_intercept.cpp)
target_include_directories(host_upscalers SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-api")
target_link_libraries(host_upscalers PUBLIC abundance_options host_hooking host_dlss host_settings d3d12)

# -------------------------------------------------------------- deliverables
# Standalone manager / diagnostic exe.
add_executable(NeuralAliasAbundance src/app/main.cpp src/app/window.cpp)
target_link_libraries(NeuralAliasAbundance PRIVATE abundance_core d3d12 dxgi user32 gdi32)

# dxgi.dll proxy (deployed under that name by the deploy script; built as
# NeuralAliasDXGI.dll so it is never auto-loaded by accident). Boots the host.
add_library(NeuralAliasDXGI SHARED src/runtime/dxgi_proxy.cpp src/runtime/dxgi_proxy.def)
target_link_libraries(NeuralAliasDXGI PRIVATE host_dx12)

# sl.interposer forwarding proxy (Route A, Streamline titles such as TLOU).
add_library(NeuralAliasSLProxy SHARED src/host_dx12/sl_proxy/sl_proxy.cpp)
set_target_properties(NeuralAliasSLProxy PROPERTIES OUTPUT_NAME "sl.interposer" PREFIX "")
target_link_libraries(NeuralAliasSLProxy PRIVATE host_dx12)

# -------------------------------------------------------------------- tests
add_test(NAME capability_report COMMAND NeuralAliasAbundance --status)
add_test(NAME standalone_settings COMMAND NeuralAliasAbundance --ui-smoke)
set_tests_properties(capability_report standalone_settings PROPERTIES LABELS "gpu")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpu/CMakeLists.txt")
  add_subdirectory(tests/cpu)   # Astra (A1)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/host/CMakeLists.txt")
  add_subdirectory(tests/host)  # Claude (C3): GPU/host/proxy tests, stubs, fixture
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
  add_subdirectory(tools)       # Claude (C5): nr_replay (local-only, needs SDK lib)
endif()

# Reversible per-application FG preset helper; no profile changes at build/test time.
add_executable(NAProfile src/host_dx12/fg/preset_profile.cpp)
target_compile_definitions(NAProfile PRIVATE NA_PROFILE_CLI)
target_link_libraries(NAProfile PRIVATE host_settings nvapi_headers)
