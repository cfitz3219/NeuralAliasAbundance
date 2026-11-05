# DX9 tests (Claude, docs/DX9-PLAN.md). Windows only.
# D0: d3d9on12_probe proves a DX9 device's colour AND depth can be read through
# D3D9On12. It also builds on its own so CI can make a 32-bit copy:
#   cmake -S tests/dx9 -B build32 -A Win32
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
  project(na_dx9_tests CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
  set(NA_DX9_STANDALONE ON)
endif()

# D3: the DX9 ears on synthetic frames (portable: Linux too, and 32-bit).
add_executable(dx9_ears_test dx9_ears_test.cpp)
if(TARGET abundance_options)
  target_link_libraries(dx9_ears_test PRIVATE abundance_options)
else()
  target_include_directories(dx9_ears_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
endif()
add_test(NAME dx9_ears COMMAND dx9_ears_test)

# D5: the vertex-shader jitter patch, run on a small shader VM (portable).
add_executable(dx9_vs_patch_test vs_patch_test.cpp)
target_include_directories(dx9_vs_patch_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME dx9_vs_patch COMMAND dx9_vs_patch_test)

if(WIN32)
  add_executable(d3d9on12_probe d3d9on12_probe.cpp)
  target_compile_definitions(d3d9on12_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(d3d9on12_probe PRIVATE /W4 /permissive-)
  endif()
  target_link_libraries(d3d9on12_probe PRIVATE d3d9 d3d12 dxgi user32)
  add_test(NAME dx9_d3d9on12_probe COMMAND d3d9on12_probe)
  set_tests_properties(dx9_d3d9on12_probe PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "host")
endif()

# D2: a small DX9 game through the d3d9.dll proxy, off (pure forward) and on
# (D3D9On12, fullscreen -> borderless, Reset, run log).
if(WIN32 AND TARGET NeuralAliasD3D9)
  add_executable(d3d9_proxy_test d3d9_proxy_test.cpp)
  target_compile_definitions(d3d9_proxy_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_include_directories(d3d9_proxy_test PRIVATE "${NA_ROOT}/src")
  target_link_libraries(d3d9_proxy_test PRIVATE user32 d3dcompiler)
  add_dependencies(d3d9_proxy_test NeuralAliasD3D9)
  if(TARGET na_bridge_helper64)
    add_dependencies(d3d9_proxy_test na_bridge_helper64)
  endif()
  # The 64-bit helper: from this build, or (32-bit build) given with -DNA_HELPER64=<path>.
  if(TARGET na_bridge_helper64)
    set(NA_DX9_HELPER "$<TARGET_FILE:na_bridge_helper64>")
  elseif(DEFINED NA_HELPER64)
    set(NA_DX9_HELPER "${NA_HELPER64}")
  else()
    set(NA_DX9_HELPER "")
  endif()
  foreach(mode off on copy)
    add_test(NAME dx9_proxy_${mode}
             COMMAND d3d9_proxy_test "$<TARGET_FILE:NeuralAliasD3D9>" "${CMAKE_CURRENT_BINARY_DIR}/proxy-data" ${mode} "${NA_DX9_HELPER}")
    set_tests_properties(dx9_proxy_${mode} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "host")
  endforeach()
endif()
