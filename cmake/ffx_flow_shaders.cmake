# P4-0c: AMD FidelityFX optical flow (third_party/ffx-opticalflow, MIT), compiled at build time.
#
# The passes use Shader Model 6 wave intrinsics, so the run-time FXC compiler (D3DCompile) cannot
# build them. DXC from the Windows SDK compiles each pass to DXIL as a C header (-Fh), into the
# build folder only (never committed). Flags mirror AMD's own DX12 backend, base variant
# (sdk/src/backends/dx12/CMakeLists.txt: -T cs_6_2 -DFFX_HALF=0 -DFFX_HLSL_SM=62), plus the
# embedded root signature so we need no FidelityFX runtime.
#
# Result: NA_HAVE_FFX_FLOW (ON/OFF) and NA_FFX_FLOW_GENERATED (list of headers). Without DXC the
# feature compiles out to a stub that reports unavailable, exactly like the NVOF stub.

set(NA_DXC "" CACHE FILEPATH "dxc.exe for the AMD optical-flow shaders (default: newest Windows SDK)")
if(NOT NA_DXC)
  set(_na_pf86 "ProgramFiles(x86)")
  file(GLOB _na_dxc_candidates
    "$ENV{${_na_pf86}}/Windows Kits/10/bin/10.*/x64/dxc.exe"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
  if(_na_dxc_candidates)
    list(SORT _na_dxc_candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET _na_dxc_candidates 0 NA_DXC)
  else()
    find_program(NA_DXC_FOUND dxc)
    if(NA_DXC_FOUND)
      set(NA_DXC "${NA_DXC_FOUND}")
    endif()
  endif()
endif()

set(NA_HAVE_FFX_FLOW OFF)
set(NA_FFX_FLOW_GENERATED "")
set(_na_ffx_root "${CMAKE_SOURCE_DIR}/third_party/ffx-opticalflow")
set(_na_ffx_out "${CMAKE_BINARY_DIR}/generated/ffx_flow")

if(NA_DXC AND EXISTS "${NA_DXC}")
  set(NA_HAVE_FFX_FLOW ON)
  file(MAKE_DIRECTORY "${_na_ffx_out}")
  file(GLOB_RECURSE _na_ffx_deps "${_na_ffx_root}/include/*.h")
  # Pass file stem -> short name used for the generated header and the C array.
  set(_na_ffx_passes
    "prepare_luma_pass:prepare_luma"
    "compute_luminance_pyramid_pass:luminance_pyramid"
    "generate_scd_histogram_pass:scd_histogram"
    "compute_scd_divergence_pass:scd_divergence"
    "compute_optical_flow_advanced_pass_v5:search"
    "filter_optical_flow_pass_v5:filter"
    "scale_optical_flow_advanced_pass_v5:scale")
  foreach(_pair IN LISTS _na_ffx_passes)
    string(REPLACE ":" ";" _pair "${_pair}")
    list(GET _pair 0 _stem)
    list(GET _pair 1 _short)
    set(_src "${_na_ffx_root}/shaders/dx12/ffx_opticalflow_${_stem}.hlsl")
    foreach(_hdr 0 1)
      set(_name "na_ffx_of_${_short}_hdr${_hdr}")
      set(_out "${_na_ffx_out}/${_name}.h")
      add_custom_command(OUTPUT "${_out}"
        COMMAND "${NA_DXC}" -nologo -T cs_6_2 -E CS -O3
                -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_HALF=0 -DFFX_HLSL_SM=62
                -DFFX_OPTICALFLOW_EMBED_ROOTSIG=1
                -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=${_hdr}
                -Wno-for-redefinition -Wno-ambig-lit-shift
                -I "${_na_ffx_root}/include/FidelityFX/gpu"
                -I "${_na_ffx_root}/include/FidelityFX/gpu/opticalflow"
                -Vn "${_name}" -Fh "${_out}" "${_src}"
        DEPENDS "${_src}" ${_na_ffx_deps}
        COMMENT "DXC: AMD optical flow ${_short} (hdr=${_hdr})"
        VERBATIM)
      list(APPEND NA_FFX_FLOW_GENERATED "${_out}")
    endforeach()
  endforeach()
  add_custom_target(ffx_flow_shaders DEPENDS ${NA_FFX_FLOW_GENERATED})
endif()

message(STATUS "NeuralAlias AMD optical flow (P4-0c): DXC='${NA_DXC}'  shaders=${NA_HAVE_FFX_FLOW}")
