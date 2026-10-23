cmake_minimum_required(VERSION 3.25)
project(NeuralAliasAbundance VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This initial DX12 host requires Windows, MSVC and x64.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_library(abundance_options INTERFACE)
target_compile_options(abundance_options INTERFACE /W4 /WX /permissive- /utf-8)
target_compile_definitions(abundance_options INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(abundance_options INTERFACE src)
add_library(abundance_core STATIC src/core/frame_contract.cpp)
target_link_libraries(abundance_core PUBLIC abundance_options)
add_executable(NeuralAliasAbundance src/app/main.cpp src/app/window.cpp)
target_link_libraries(NeuralAliasAbundance PRIVATE abundance_core d3d12 dxgi user32 gdi32)
add_library(NeuralAliasDXGI SHARED src/runtime/dxgi_proxy.cpp src/runtime/dxgi_proxy.def)
target_link_libraries(NeuralAliasDXGI PRIVATE abundance_options)
# Vendored MinHook (third_party/README.md has pins/licenses). C sources; the
# strict /W4 /WX interface options are C++-oriented, so it gets its own flags.
enable_language(C)
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)

# Route A: shared hooking core (see OBSERVATION-API.md).
add_library(host_hooking STATIC src/host_dx12/hooking/vtable_hook.cpp)
target_link_libraries(host_hooking PUBLIC abundance_options PRIVATE minhook)
add_executable(hooking_test tests/hooking_test.cpp)
target_link_libraries(hooking_test PRIVATE host_hooking d3d12 dxgi)
add_test(NAME hooking_core COMMAND hooking_test)
set_tests_properties(hooking_core PROPERTIES TIMEOUT 60 LABELS "gpu")
add_executable(host_observe_abi_test tests/host_observe_abi_test.cpp)
target_link_libraries(host_observe_abi_test PRIVATE abundance_options)
add_test(NAME host_observe_abi COMMAND host_observe_abi_test)
add_library(host_observe STATIC src/host_dx12/host_observe.cpp)
target_link_libraries(host_observe PUBLIC abundance_options)
add_executable(host_observe_framing_test tests/host_observe_framing_test.cpp)
target_link_libraries(host_observe_framing_test PRIVATE host_observe)
add_test(NAME host_observe_framing COMMAND host_observe_framing_test)
set_tests_properties(host_observe_framing PROPERTIES TIMEOUT 60)

# Route A runtime settings (ini <-> overlay <-> NGX overrides).
add_library(host_settings STATIC src/host_dx12/settings.cpp)
target_link_libraries(host_settings PUBLIC abundance_options PRIVATE shell32 ole32)

# Vendored Dear ImGui (third_party/README.md). Own flags: not our /W4 /WX.
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

# Route A: phase-1 frame capture (docs/NR-INSERTION-DESIGN.md).
add_library(host_capture STATIC src/host_dx12/capture/frame_capture.cpp)
target_link_libraries(host_capture PUBLIC abundance_options d3d12 dxgi)
add_executable(capture_test tests/capture_test.cpp)
target_link_libraries(capture_test PRIVATE host_capture)
add_test(NAME frame_capture COMMAND capture_test "${CMAKE_CURRENT_BINARY_DIR}/capture-test-out")
set_tests_properties(frame_capture PROPERTIES TIMEOUT 90 LABELS "gpu")

# Route A: NGX-layer observation (see src/host_dx12/ngx_observe/).
add_library(ngx_observe STATIC
  src/host_dx12/ngx_observe/ngx_observe.cpp
  src/host_dx12/ngx_observe/gpu_timing.cpp)
target_link_libraries(ngx_observe PUBLIC abundance_options host_hooking host_settings host_capture d3d12)
add_executable(gpu_timing_test tests/gpu_timing_test.cpp)
target_link_libraries(gpu_timing_test PRIVATE ngx_observe)
add_test(NAME gpu_timing COMMAND gpu_timing_test)
set_tests_properties(gpu_timing PROPERTIES TIMEOUT 60 LABELS "gpu")

# Route A: in-game overlay (NeuralAlias "Signal Graphite", docs/UI-STYLE.md).
add_library(host_overlay STATIC
  src/host_dx12/overlay/overlay.cpp
  src/host_dx12/overlay/menu.cpp
  src/host_dx12/overlay/route_a_panel.cpp)
target_link_libraries(host_overlay PUBLIC abundance_options host_hooking host_settings ngx_observe host_capture imgui PRIVATE shell32 ole32)
add_executable(overlay_test tests/overlay_test.cpp)
target_link_libraries(overlay_test PRIVATE host_overlay user32)
add_test(NAME overlay_gpu COMMAND overlay_test)
set_tests_properties(overlay_gpu PROPERTIES TIMEOUT 90 LABELS "gpu")
# Locally pinned NVIDIA NGX SDK headers enable parameter reading. Never
# committed; builds stay green without them (EvalParams.valid stays 0).
set(NA_NGX_SDK_INCLUDE "" CACHE PATH "Local NVIDIA NGX SDK include dir")
if(NOT NA_NGX_SDK_INCLUDE)
  foreach(cand
      "${CMAKE_CURRENT_SOURCE_DIR}/../dx11-enhancement/local/nvidia-dlss-sdk/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../../../outputs/dx11-enhancement/local/nvidia-dlss-sdk/include")
    if(EXISTS "${cand}/nvsdk_ngx_params.h")
      set(NA_NGX_SDK_INCLUDE "${cand}")
      break()
    endif()
  endforeach()
endif()
if(NA_NGX_SDK_INCLUDE)
  message(STATUS "NGX SDK headers found: ${NA_NGX_SDK_INCLUDE}")
  target_compile_definitions(ngx_observe PRIVATE NA_HAVE_NGX_SDK)
  target_include_directories(ngx_observe PRIVATE "${NA_NGX_SDK_INCLUDE}")
  add_executable(ngx_params_test tests/ngx_params_test.cpp)
  target_link_libraries(ngx_params_test PRIVATE ngx_observe)
  target_compile_definitions(ngx_params_test PRIVATE NA_HAVE_NGX_SDK)
  target_include_directories(ngx_params_test PRIVATE "${NA_NGX_SDK_INCLUDE}")
  add_test(NAME ngx_params COMMAND ngx_params_test)
  add_executable(ngx_optimal_test tests/ngx_optimal_test.cpp)
  target_link_libraries(ngx_optimal_test PRIVATE ngx_observe)
  target_compile_definitions(ngx_optimal_test PRIVATE NA_HAVE_NGX_SDK)
  target_include_directories(ngx_optimal_test PRIVATE "${NA_NGX_SDK_INCLUDE}")
  add_test(NAME ngx_optimal_settings COMMAND ngx_optimal_test)
  # Phase-2 offline NR replay tool (local-only: needs the SDK static lib and
  # the private NR runtime; never redistributed).
  get_filename_component(NA_NGX_SDK_ROOT "${NA_NGX_SDK_INCLUDE}" DIRECTORY)
  set(NA_NGX_SDK_LIB "${NA_NGX_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
  if(EXISTS "${NA_NGX_SDK_LIB}")
    add_executable(nr_replay tools/nr_replay.cpp)
    target_include_directories(nr_replay SYSTEM PRIVATE "${NA_NGX_SDK_INCLUDE}")
    target_compile_definitions(nr_replay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(nr_replay PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_libraries(nr_replay PRIVATE d3d12 dxgi ole32 windowscodecs "${NA_NGX_SDK_LIB}")
    set(NA_NR_RUNTIME "${NA_NGX_SDK_ROOT}/../neural-runtime-310.8.0")
    set(NA_ALIEN_FRAMES "${NA_NGX_SDK_ROOT}/../replay-88730/prepared")
    if(EXISTS "${NA_NR_RUNTIME}/nvngx_dlssnr.dll" AND EXISTS "${NA_ALIEN_FRAMES}/color0.raw")
      add_test(NAME nr_replay_alien COMMAND nr_replay alien "${NA_ALIEN_FRAMES}" "${NA_NR_RUNTIME}"
        "${CMAKE_CURRENT_BINARY_DIR}/nr-cache" "${CMAKE_CURRENT_BINARY_DIR}/nr-replay-alien" 1920 1200)
      set_tests_properties(nr_replay_alien PROPERTIES TIMEOUT 180 LABELS "gpu;local-only")
    endif()
  endif()
else()
  message(STATUS "NGX SDK headers not found; parameter reading disabled")
endif()
add_library(nvngx_stub SHARED tests/ngx_stub.cpp)
set_target_properties(nvngx_stub PROPERTIES PREFIX "")
target_link_libraries(nvngx_stub PRIVATE abundance_options)
add_executable(ngx_observe_test tests/ngx_observe_test.cpp)
target_link_libraries(ngx_observe_test PRIVATE ngx_observe)
add_dependencies(ngx_observe_test nvngx_stub)
add_test(NAME ngx_observation COMMAND ngx_observe_test
  "$<TARGET_FILE:nvngx_stub>" "${CMAKE_CURRENT_BINARY_DIR}/ngx-observe-test.log")
set_tests_properties(ngx_observation PROPERTIES TIMEOUT 60)

# Route A: sl.interposer forwarding proxy (see src/host_dx12/sl_proxy/).
add_library(NeuralAliasSLProxy SHARED src/host_dx12/sl_proxy/sl_proxy.cpp)
set_target_properties(NeuralAliasSLProxy PROPERTIES OUTPUT_NAME "sl.interposer" PREFIX "")
target_link_libraries(NeuralAliasSLProxy PRIVATE abundance_options ngx_observe host_overlay host_settings user32)
add_library(sl_interposer_real SHARED tests/sl_stub.cpp)
set_target_properties(sl_interposer_real PROPERTIES PREFIX "")
target_link_libraries(sl_interposer_real PRIVATE abundance_options)
add_executable(sl_proxy_test tests/sl_proxy_test.cpp)
target_link_libraries(sl_proxy_test PRIVATE abundance_options)
add_dependencies(sl_proxy_test NeuralAliasSLProxy sl_interposer_real nvngx_stub)
add_test(NAME sl_proxy_forwarding COMMAND sl_proxy_test
  "$<TARGET_FILE:NeuralAliasSLProxy>" "$<TARGET_FILE:nvngx_stub>"
  "${CMAKE_CURRENT_BINARY_DIR}/sl-proxy-observe-test.log")
set_tests_properties(sl_proxy_forwarding PROPERTIES TIMEOUT 30)
add_executable(proxy_test tests/proxy_test.cpp)
target_link_libraries(proxy_test PRIVATE abundance_options d3d12 dxgi user32)
add_dependencies(proxy_test NeuralAliasDXGI)
add_executable(abundance_dx12_fixture tests/dx12_fixture.cpp)
target_link_libraries(abundance_dx12_fixture PRIVATE abundance_core d3d12 dxgi d3dcompiler)
target_compile_definitions(abundance_dx12_fixture PRIVATE ABUNDANCE_SHADER_PATH=L"${CMAKE_CURRENT_SOURCE_DIR}/shaders/reference_scene.hlsl")
enable_testing()
foreach(test IN ITEMS temporal_jitter frame_history upload_cache)
  add_executable(${test}_test tests/${test}_test.cpp)
  target_link_libraries(${test}_test PRIVATE abundance_options)
  add_test(NAME ${test} COMMAND ${test}_test)
endforeach()
add_executable(frame_contract_test tests/frame_contract_test.cpp)
target_link_libraries(frame_contract_test PRIVATE abundance_core)
add_test(NAME frame_contract COMMAND frame_contract_test)
add_test(NAME dx12_reference_scene COMMAND abundance_dx12_fixture --output "${CMAKE_CURRENT_BINARY_DIR}/evidence")
set_tests_properties(dx12_reference_scene PROPERTIES TIMEOUT 60 LABELS "gpu")
add_test(NAME capability_report COMMAND NeuralAliasAbundance --status)
add_test(NAME standalone_settings COMMAND NeuralAliasAbundance --ui-smoke)
add_test(NAME dxgi_forwarding COMMAND proxy_test "$<TARGET_FILE:NeuralAliasDXGI>")
set_tests_properties(dxgi_forwarding PROPERTIES TIMEOUT 30)
