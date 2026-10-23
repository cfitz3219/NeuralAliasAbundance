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
