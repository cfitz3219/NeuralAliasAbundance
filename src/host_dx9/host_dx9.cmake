# DX9 host (src/host_dx9/, docs/DX9-PLAN.md). Included by the main build (64-bit)
# and by win32/ (32-bit). Needs NA_ROOT and the minhook + abundance_options targets.
if(WIN32)
  # d3d9.dll proxy, built as NeuralAliasD3D9.dll so it is never loaded by
  # accident; deploy-dx9.ps1 puts it beside the game as d3d9.dll.
  add_library(NeuralAliasD3D9 SHARED
    "${NA_ROOT}/src/host_dx9/d3d9_proxy.cpp"
    "${NA_ROOT}/src/host_dx9/dx9_hooks.cpp"
    "${NA_ROOT}/src/host_dx9/d3d9_proxy.def")
  target_link_libraries(NeuralAliasD3D9 PRIVATE abundance_options minhook d3d12 dxgi user32)
endif()
