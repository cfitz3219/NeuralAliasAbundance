# C3 (MASTER-PLAN Phase 1): reconstructed host / GPU / proxy tests (Claude-owned).
# Device tests try hardware first, then WARP (software D3D12), so most of them
# also run on the hosted CI runner. A test that truly needs NVIDIA hardware or
# the NGX runtime is labelled "nvidia" and only runs in Cort's test sessions.
# Exit code 77 = SKIP (no device), never a fake pass.

function(na_host_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS;ARGS;LABELS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE abundance_options ${T_LIBS})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME host_${name} COMMAND ${name} ${T_ARGS})
  set_tests_properties(host_${name} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "host;${T_LABELS}")
endfunction()

# Fake NGX runtime (the one non-driver name ngx_observe accepts).
add_library(nvngx_stub SHARED ngx_stub.cpp)
set_target_properties(nvngx_stub PROPERTIES PREFIX "")
target_link_libraries(nvngx_stub PRIVATE abundance_options)

na_host_test(hooking_test SOURCES hooking_test.cpp LIBS host_hooking d3d12 dxgi)

na_host_test(proxy_test SOURCES proxy_test.cpp LIBS d3d12 dxgi user32
             ARGS "$<TARGET_FILE:NeuralAliasDXGI>")
add_dependencies(proxy_test NeuralAliasDXGI)

na_host_test(dx12_fixture SOURCES dx12_fixture.cpp LIBS abundance_core d3d12 dxgi d3dcompiler
             ARGS --output "${CMAKE_CURRENT_BINARY_DIR}/fixture-evidence")
target_compile_definitions(dx12_fixture PRIVATE
  ABUNDANCE_SHADER_PATH=L"${PROJECT_SOURCE_DIR}/shaders/reference_scene.hlsl")

# C4 / R-001: GPU timer completion evidence (links the host library for gpu_timing).
na_host_test(gpu_timing_test SOURCES gpu_timing_test.cpp LIBS host_dx12)

na_host_test(ngx_observe_test SOURCES ngx_observe_test.cpp LIBS host_dx12
             ARGS "$<TARGET_FILE:nvngx_stub>")
add_dependencies(ngx_observe_test nvngx_stub)

# Stand-in for the game's renamed original interposer.
add_library(sl_interposer_real SHARED sl_stub.cpp)
set_target_properties(sl_interposer_real PROPERTIES PREFIX "")
target_link_libraries(sl_interposer_real PRIVATE abundance_options)

na_host_test(sl_proxy_test SOURCES sl_proxy_test.cpp LIBS user32
             ARGS "$<TARGET_FILE:NeuralAliasSLProxy>" "$<TARGET_FILE:sl_interposer_real>")
add_dependencies(sl_proxy_test NeuralAliasSLProxy sl_interposer_real)

# Gate 2 R-306: missing real exports must not look like success to 1.x or 2.x
# callers. The minimal stand-in and the test exe share their own folder, so
# the proxy's sl_interposer_real.dll lookup (exe folder first) finds this one.
add_library(sl_interposer_real_min SHARED sl_stub_min.cpp)
set_target_properties(sl_interposer_real_min PROPERTIES PREFIX "" OUTPUT_NAME sl_interposer_real
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slmin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slmin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slmin")
target_link_libraries(sl_interposer_real_min PRIVATE abundance_options)
na_host_test(sl_proxy_missing_test SOURCES sl_proxy_missing_test.cpp LIBS user32
             ARGS "$<TARGET_FILE:NeuralAliasSLProxy>")
set_target_properties(sl_proxy_missing_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/slmin")
add_dependencies(sl_proxy_missing_test NeuralAliasSLProxy sl_interposer_real_min)


na_host_test(capture_test SOURCES capture_test.cpp LIBS host_capture
             ARGS "${CMAKE_CURRENT_BINARY_DIR}/capture-test-out")

na_host_test(overlay_test SOURCES overlay_test.cpp LIBS host_dx12 user32)

# Parameter-object tests need the NVIDIA NGX SDK headers (local SDK folder, or
# the CI job that fetches the public DLSS repo's headers at build time).
if(NA_HAVE_NGX_SDK)
  na_host_test(ngx_params_test SOURCES ngx_params_test.cpp LIBS host_dx12)
  na_host_test(ngx_optimal_test SOURCES ngx_optimal_test.cpp LIBS host_dx12)
endif()

# Argon audit #2: depth formats safe to copy as R32F (no device).
na_host_test(depth_format_test SOURCES depth_format_test.cpp)

# C6: generic DLSS backend. The fake-runtime test needs only the NGX headers
# (runs in CI); the real test needs our own NGX instance (static library from
# the local SDK folder) plus an RTX GPU, so it only runs in Cort's sessions.
if(NA_HAVE_NGX_SDK)
  na_host_test(dlss_backend_test SOURCES dlss_backend_test.cpp LIBS host_dlss dxgi)
endif()
if(NA_HAVE_NGX_LIB)
  na_host_test(dlss_real_test SOURCES dlss_real_test.cpp LIBS host_dlss dxgi
               ARGS "${CMAKE_CURRENT_BINARY_DIR}/dlss-real-test" LABELS "gpu;nvidia")
  if(NA_DLSS_DLL)
    add_custom_command(TARGET dlss_real_test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NA_DLSS_DLL}" "$<TARGET_FILE_DIR:dlss_real_test>")
  endif()
endif()

# C7a: FSR 3.1+ interception against a stand-in FidelityFX runtime DLL.
add_library(ffx_stub SHARED ffx_stub.cpp)
set_target_properties(ffx_stub PROPERTIES PREFIX "")
target_include_directories(ffx_stub SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/ffx-api")
target_link_libraries(ffx_stub PRIVATE abundance_options)
na_host_test(ffx_intercept_test SOURCES ffx_intercept_test.cpp LIBS host_upscalers dxgi
             ARGS "$<TARGET_FILE:ffx_stub>")
add_dependencies(ffx_intercept_test ffx_stub)

# C7b: XeSS interception against a stand-in libxess.dll (built from our ABI).
add_library(xess_stub SHARED xess_stub.cpp)
set_target_properties(xess_stub PROPERTIES PREFIX "")
target_link_libraries(xess_stub PRIVATE abundance_options d3d12)
na_host_test(xess_intercept_test SOURCES xess_intercept_test.cpp LIBS host_upscalers dxgi
             ARGS "$<TARGET_FILE:xess_stub>")
add_dependencies(xess_intercept_test xess_stub)
# Our XeSS declarations vs Intel's official headers (compile-time checks).
if(NA_XESS_INCLUDE AND EXISTS "${NA_XESS_INCLUDE}/xess/xess_d3d12.h")
  na_host_test(xess_abi_test SOURCES xess_abi_test.cpp LIBS d3d12)
  target_include_directories(xess_abi_test SYSTEM PRIVATE "${NA_XESS_INCLUDE}")
endif()

# Gate 2 R-302: watcher registration vs. loader notifications (no deadlock).
na_host_test(module_watch_test SOURCES module_watch_test.cpp LIBS host_upscalers
             ARGS "$<TARGET_FILE:sl_interposer_real>" "$<TARGET_FILE:ffx_stub>")
add_dependencies(module_watch_test sl_interposer_real ffx_stub)
set_tests_properties(host_module_watch_test PROPERTIES TIMEOUT 60)
