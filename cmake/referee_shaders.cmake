set(referee_program_files "ProgramFiles(x86)")
file(GLOB referee_fxc_candidates
  "$ENV{${referee_program_files}}/Windows Kits/10/bin/10.*/x64/fxc.exe"
  "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT referee_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
find_program(NA_REFEREE_FXC NAMES fxc)
if(NOT NA_REFEREE_FXC AND referee_fxc_candidates)
  list(GET referee_fxc_candidates 0 NA_REFEREE_FXC)
endif()
if(NA_REFEREE_FXC)
  set(referee_source "${CMAKE_CURRENT_SOURCE_DIR}/src/host_dx12/capture/referee_shader.hlsl")
  set(referee_output "${CMAKE_CURRENT_BINARY_DIR}/generated/referee")
  file(MAKE_DIRECTORY "${referee_output}")
  foreach(entry thumb tiles points)
    set(output "${referee_output}/referee_${entry}.h")
    add_custom_command(OUTPUT "${output}"
      COMMAND "${NA_REFEREE_FXC}" /nologo /T cs_5_0 /E "${entry}" /O3 /Gis
              /Fh "${output}" /Vn "referee_${entry}" "${referee_source}"
      DEPENDS "${referee_source}"
      COMMENT "FXC: referee ${entry}"
      VERBATIM)
    target_sources(host_capture PRIVATE "${output}")
  endforeach()
  target_include_directories(host_capture PRIVATE "${referee_output}")
  target_compile_definitions(host_capture PUBLIC NA_HAVE_REFEREE_SHADERS)
else()
  message(STATUS "Windows SDK FXC unavailable: referee GPU diagnostics disabled")
endif()
