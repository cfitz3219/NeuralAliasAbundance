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

add_library(host_observe STATIC src/host_dx12/host_observe.cpp)
target_link_libraries(host_observe PUBLIC abundance_options)

add_library(host_settings STATIC src/host_dx12/settings.cpp)
target_link_libraries(host_settings PUBLIC abundance_options PRIVATE shell32 ole32)

add_library(host_capture STATIC src/host_dx12/capture/frame_capture.cpp)
target_link_libraries(host_capture PUBLIC abundance_options d3d12 dxgi)

add_library(core_gpu STATIC src/core/motion_arbiter_gpu.cpp)
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

# ---------------------------------------------------------------- host_dx12
# The in-game host. These modules reference each other freely (overlay <->
# ngx_observe <-> nr/fg/motion <-> latency <-> tlou1), so they form one library.
set(NA_GAME_ADAPTER_LIB "" CACHE STRING "CMake target implementing game_adapter.h; empty = host stub (no adapter)")
add_library(host_dx12 STATIC
  src/host_dx12/ngx_observe/ngx_observe.cpp
  src/host_dx12/ngx_observe/gpu_timing.cpp
  src/host_dx12/overlay/overlay.cpp
  src/host_dx12/overlay/menu.cpp
  src/host_dx12/overlay/route_a_panel.cpp
  src/host_dx12/overlay/presenter.cpp
  src/host_dx12/overlay/freeze_watchdog.cpp
  src/host_dx12/fg/camera.cpp
  src/host_dx12/fg/frame_gen.cpp
  src/host_dx12/fg/preset_profile.cpp
  src/host_dx12/nr/nr_pass.cpp
  src/host_dx12/motion/live_flow.cpp
  src/host_dx12/latency/nvidia_latency.cpp
  src/host_dx12/latency/pcl_stats.cpp
  src/host_dx12/adapters/tlou1/apply.cpp
  src/host_dx12/adapters/tlou1/timing.cpp
  src/host_dx12/host_boot/host_boot.cpp)
if(NA_GAME_ADAPTER_LIB)
  target_link_libraries(host_dx12 PUBLIC "${NA_GAME_ADAPTER_LIB}")
  message(STATUS "Game adapter: ${NA_GAME_ADAPTER_LIB}")
else()
  target_sources(host_dx12 PRIVATE src/host_dx12/host_boot/adapter_stub.cpp)
  message(STATUS "Game adapter: none (host stub)")
endif()
target_link_libraries(host_dx12
  PUBLIC abundance_options host_hooking host_settings host_capture host_nvof core_gpu imgui nvapi_headers
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
add_library(host_dlss STATIC src/host_dx12/dlss/dlss_backend.cpp)
target_link_libraries(host_dlss PUBLIC abundance_options abundance_core d3d12)
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
message(STATUS "DLSS backend (C6): headers=${NA_HAVE_NGX_SDK}  own NGX instance=${NA_HAVE_NGX_LIB}  nvngx_dlss.dll='${NA_DLSS_DLL}'")

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
