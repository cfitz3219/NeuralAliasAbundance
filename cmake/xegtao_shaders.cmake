# Reuse the DXC discovered by ffx_flow_shaders.cmake. Shader text/vendor code
# stays unchanged; generated DXIL headers live only in the build directory.
set(NA_HAVE_XEGTAO OFF)
if(NA_DXC AND EXISTS "${NA_DXC}")
  set(NA_HAVE_XEGTAO ON)
  set(_ao_src "${CMAKE_SOURCE_DIR}/src/host_dx12/effects/xegtao.hlsl")
  set(_ao_vendor "${CMAKE_SOURCE_DIR}/third_party/xegtao")
  set(_ao_out "${CMAKE_BINARY_DIR}/generated/xegtao")
  file(MAKE_DIRECTORY "${_ao_out}")
  set(NA_XEGTAO_GENERATED "")
  foreach(_pair "Extract:EXTRACT" "Prefilter:PREFILTER" "Normals:NORMALS" "Main:MAIN" "Denoise:DENOISE" "Temporal:TEMPORAL" "Compose:COMPOSE")
    string(REPLACE ":" ";" _parts "${_pair}")
    list(GET _parts 0 _entry)
    list(GET _parts 1 _define)
    set(_out "${_ao_out}/${_entry}.h")
    add_custom_command(OUTPUT "${_out}"
      COMMAND "${NA_DXC}" -nologo -T cs_6_0 -E "${_entry}" -O3 -DPASS_${_define}=1
              -I "${_ao_vendor}" -Vn "na_xegtao_${_entry}" -Fh "${_out}" "${_ao_src}"
      DEPENDS "${_ao_src}" "${_ao_vendor}/XeGTAO.h" "${_ao_vendor}/XeGTAO.hlsli"
      COMMENT "DXC: Intel XeGTAO ${_entry}" VERBATIM)
    list(APPEND NA_XEGTAO_GENERATED "${_out}")
  endforeach()
  add_custom_target(xegtao_shaders DEPENDS ${NA_XEGTAO_GENERATED})
endif()
