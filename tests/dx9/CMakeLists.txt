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
