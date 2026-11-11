# DX11 hooks tests (Claude). The frame model test is portable (Linux CI too).
# The synthetic-game test needs Windows: a small DX11 "game" (shadow pass,
# scene with a camera constant buffer, tonemap read, HUD, Present) runs under
# the real hooks, with the helper in copy-test mode.
add_executable(dx11_frame_model_test frame_model_test.cpp)
target_link_libraries(dx11_frame_model_test PRIVATE abundance_options)
add_test(NAME dx11_frame_model COMMAND dx11_frame_model_test)

if(WIN32)
  add_executable(dx11_host_test dx11_host_test.cpp)
  target_link_libraries(dx11_host_test PRIVATE host_dx11 d3dcompiler)
  add_dependencies(dx11_host_test na_bridge_helper64)
  add_test(NAME dx11_host_synthetic_game
           COMMAND dx11_host_test "$<TARGET_FILE:na_bridge_helper64>" "${CMAKE_CURRENT_BINARY_DIR}/dx11-data")
  set_tests_properties(dx11_host_synthetic_game PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "host")
endif()
