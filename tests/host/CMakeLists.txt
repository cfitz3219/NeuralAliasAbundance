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

na_host_test(capture_test SOURCES capture_test.cpp LIBS host_capture
             ARGS "${CMAKE_CURRENT_BINARY_DIR}/capture-test-out")
