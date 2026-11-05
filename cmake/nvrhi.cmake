# Pinned MIT backend. No DLSS/NGX model or restricted SDK is fetched here.
option(NA_BUILD_NVRHI "Build optional shared NVRHI GPU infrastructure" ON)
set(NA_HAVE_NVRHI OFF)
if(NA_BUILD_NVRHI)
  include(FetchContent)
  set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_VULKAN OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_DX12 ON CACHE BOOL "" FORCE)
  set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
  set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
  set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
  set(NVRHI_DIRECTX_HEADERS_GIT_TAG d873b344dc540898868697245f100c2a67fe68d9 CACHE STRING "" FORCE)
  FetchContent_Declare(na_nvrhi
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
    GIT_TAG bf0bbd9b666a7297492beb1999a4ddc0dfded143)
  FetchContent_MakeAvailable(na_nvrhi)
  set(NA_HAVE_NVRHI ON)
endif()
