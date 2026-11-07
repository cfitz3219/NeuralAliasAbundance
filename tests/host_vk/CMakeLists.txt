# Vulkan layer tests (Claude). A small Vulkan "game" (shadow pass, scene with a
# camera uniform buffer, mid-scene copy, tonemap read, HUD, present) runs under
# the real layer, with copy_test=2 standing in for DLSS. Needs the Vulkan loader
# and a driver (CI: Mesa lavapipe, a CPU Vulkan driver); exit 77 = skipped.
if(NOT TARGET Vulkan::Vulkan)
  message(STATUS "Vulkan layer test: no Vulkan loader found, test not built")
  return()
endif()
add_executable(vk_layer_test vk_layer_test.cpp)
target_include_directories(vk_layer_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
target_link_libraries(vk_layer_test PRIVATE abundance_options Vulkan::Vulkan ${CMAKE_DL_LIBS})
if(MSVC)
  target_compile_options(vk_layer_test PRIVATE /wd4100)
  target_compile_definitions(vk_layer_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_dependencies(vk_layer_test NeuralAliasVulkan)
foreach(mode plain jitter motion flip dynamic)
  add_test(NAME vk_layer_synthetic_game_${mode}
           COMMAND vk_layer_test "$<TARGET_FILE_DIR:NeuralAliasVulkan>" "${CMAKE_CURRENT_BINARY_DIR}" ${mode})
  set_tests_properties(vk_layer_synthetic_game_${mode} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77 LABELS "host")
endforeach()
