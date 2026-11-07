# DX11 bridge tests (Claude). The record test is portable (Linux CI too); the
# round trip needs Windows: it tries hardware, then WARP, and exits 77 (SKIP)
# only when WARP cannot share across DX11/DX12.
add_executable(bridge_frame_record_test frame_record_test.cpp)
target_link_libraries(bridge_frame_record_test PRIVATE bridge_record)
add_test(NAME bridge_frame_record COMMAND bridge_frame_record_test)

if(WIN32)
  add_executable(dx11_bridge_test dx11_bridge_test.cpp)
  target_link_libraries(dx11_bridge_test PRIVATE bridge_dx11)
  add_dependencies(dx11_bridge_test na_bridge_helper64)
  add_test(NAME bridge_dx11_roundtrip
           COMMAND dx11_bridge_test "$<TARGET_FILE:na_bridge_helper64>" "${CMAKE_CURRENT_BINARY_DIR}/bridge-data")
  set_tests_properties(bridge_dx11_roundtrip PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "host")
endif()
