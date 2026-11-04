# Vulkan host layer (src/host_vk, docs/VULKAN-HOOKS.md). Built on Windows and
# Linux. The layer needs only the Vulkan headers (the loader loads it; it never
# links the loader). The synthetic-game test needs the loader and a Vulkan
# driver (CI: Mesa lavapipe), so it is added only where the loader is found.
set(NA_VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include dir (vulkan/vulkan.h); found or fetched when empty")
find_package(Vulkan QUIET)
if(NA_VULKAN_INCLUDE)
  add_library(na_vulkan_headers INTERFACE)
  target_include_directories(na_vulkan_headers SYSTEM INTERFACE "${NA_VULKAN_INCLUDE}")
elseif(TARGET Vulkan::Headers)
  add_library(na_vulkan_headers INTERFACE)
  target_link_libraries(na_vulkan_headers INTERFACE Vulkan::Headers)
else()
  # Khronos headers (Apache-2.0 / MIT), fetched at configure time, never committed.
  include(FetchContent)
  FetchContent_Declare(na_vk_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.275
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(na_vk_headers)
  add_library(na_vulkan_headers INTERFACE)
  target_link_libraries(na_vulkan_headers INTERFACE Vulkan::Headers)
endif()

set(NA_VK_DLSS_SRC src/host_vk/vk_dlss_stub.cpp)
if(WIN32 AND NA_HAVE_NGX_LIB)
  set(NA_VK_DLSS_SRC src/host_vk/vk_dlss_ngx.cpp)
endif()
add_library(NeuralAliasVulkan MODULE src/host_vk/vk_layer.cpp ${NA_VK_DLSS_SRC})
target_link_libraries(NeuralAliasVulkan PRIVATE abundance_options na_vulkan_headers ${CMAKE_DL_LIBS})
set_target_properties(NeuralAliasVulkan PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(NeuralAliasVulkan PRIVATE Threads::Threads)
endif()
if(WIN32 AND NA_HAVE_NGX_LIB)
  target_include_directories(NeuralAliasVulkan SYSTEM PRIVATE "${NA_NGX_SDK_INCLUDE}")
  if(EXISTS "${NA_NGX_LIB_DEBUG}")
    target_link_libraries(NeuralAliasVulkan PRIVATE "$<IF:$<CONFIG:Debug>,${NA_NGX_LIB_DEBUG},${NA_NGX_LIB}>")
  else()
    target_link_libraries(NeuralAliasVulkan PRIVATE "${NA_NGX_LIB}")
  endif()
endif()
if(WIN32 AND NA_HAVE_NGX_SDK)
  # Compile-checks the NGX Vulkan code whenever the SDK headers exist (CI's SDK job).
  add_library(host_vk_ngx_check STATIC src/host_vk/vk_dlss_ngx.cpp)
  target_link_libraries(host_vk_ngx_check PRIVATE abundance_options bridge_record na_vulkan_headers)
  target_include_directories(host_vk_ngx_check SYSTEM PRIVATE "${NA_NGX_SDK_INCLUDE}")
endif()

# The layer's manifest beside it. Registered as an implicit layer by
# scripts/deploy-vulkan.ps1; tests enable it by name through VK_LAYER_PATH.
if(WIN32)
  set(NA_VK_LIB "./NeuralAliasVulkan.dll")
else()
  set(NA_VK_LIB "./libNeuralAliasVulkan.so")
endif()
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:NeuralAliasVulkan>/NeuralAliasVulkan.json" CONTENT
"{
  \"file_format_version\": \"1.2.0\",
  \"layer\": {
    \"name\": \"VK_LAYER_NEURALALIAS_host\",
    \"type\": \"GLOBAL\",
    \"library_path\": \"${NA_VK_LIB}\",
    \"api_version\": \"1.3.275\",
    \"implementation_version\": \"1\",
    \"description\": \"NeuralAlias: DLSS for Vulkan games. Does nothing unless NeuralAliasHost.ini beside the game has [vulkan] enabled=1.\",
    \"functions\": { \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\" },
    \"disable_environment\": { \"DISABLE_NEURALALIAS_VK\": \"1\" }
  }
}
")
