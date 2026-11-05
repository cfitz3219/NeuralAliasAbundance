# 32-bit game side (DX9 plan step D1, docs/DX9-PLAN.md). Owner: Claude.
#
# Most DX9 games are 32-bit, and DLSS (NGX) is 64-bit only. So the part that
# lives inside the game is built here as 32-bit, on its own, and talks to the
# unchanged 64-bit helper (na_bridge_helper64.exe from the main build).
#
#   cmake -S win32 -B build32 -A Win32
#   cmake --build build32 --config Release
#   ctest --test-dir build32 -C Release -LE gpu
#
# Only portable, SDK-free code comes in here: MinHook (32-bit decoder), the
# bridge frame record, the frame model and camera pick, and the DX9 parts.
cmake_minimum_required(VERSION 3.25)
project(NeuralAlias32 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(NA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(WARNING "win32/ is meant for 32-bit builds (-A Win32); building it as ${CMAKE_SIZEOF_VOID_P}-byte pointers anyway")
endif()

add_library(abundance_options INTERFACE)
target_include_directories(abundance_options INTERFACE "${NA_ROOT}/src")
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(abundance_options INTERFACE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(abundance_options INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
else()
  target_compile_options(abundance_options INTERFACE -Wall -Wextra -Werror -pedantic)
endif()

# MinHook with the decoder that matches the pointer size.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(NA_HDE "${NA_ROOT}/third_party/minhook/src/hde/hde32.c")
else()
  set(NA_HDE "${NA_ROOT}/third_party/minhook/src/hde/hde64.c")
endif()
if(WIN32)
  add_library(minhook STATIC
    "${NA_ROOT}/third_party/minhook/src/buffer.c"
    "${NA_ROOT}/third_party/minhook/src/hook.c"
    "${NA_ROOT}/third_party/minhook/src/trampoline.c"
    "${NA_HDE}")
  target_include_directories(minhook PUBLIC "${NA_ROOT}/third_party/minhook/include")
  target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# The wire format to the 64-bit helper: identical layout at 32 and 64 bits.
add_library(bridge_record STATIC "${NA_ROOT}/src/bridge/frame_record.cpp")
target_link_libraries(bridge_record PUBLIC abundance_options)

# Portable tests, now also proven at 32 bits.
add_executable(bridge_frame_record_test "${NA_ROOT}/tests/bridge/frame_record_test.cpp")
target_link_libraries(bridge_frame_record_test PRIVATE bridge_record)
add_test(NAME bridge_frame_record_32 COMMAND bridge_frame_record_test)

add_executable(dx11_frame_model_test "${NA_ROOT}/tests/host_dx11/frame_model_test.cpp")
target_link_libraries(dx11_frame_model_test PRIVATE abundance_options)
add_test(NAME frame_model_32 COMMAND dx11_frame_model_test)

if(WIN32)
  # MinHook really hooks at 32 bits (hde32 decoder + trampolines).
  add_executable(minhook32_test "${NA_ROOT}/tests/dx9/minhook32_test.cpp")
  target_link_libraries(minhook32_test PRIVATE abundance_options minhook)
  add_test(NAME minhook_hooks_32 COMMAND minhook32_test)
endif()

add_subdirectory("${NA_ROOT}/tests/dx9" tests_dx9)
