# The NA1 enhancer's compute programs (src/host_dx12/effects/na1.hlsl), compiled at build time by
# the Windows SDK's FXC as cs_5_0, one header per entry point, so that a shader mistake fails the
# build with FXC's own "file(line,col): error X....: ..." message instead of failing silently in a
# game. Reuses the FXC found by cmake/referee_shaders.cmake (included before this file) and looks
# for it the same way when that file did not run. FXC is a host tool: the same x64 program serves
# the Win32 configuration.
#
# Sets NA_HAVE_NA1 and the custom target na1_shaders; the root CMakeLists.txt gives host_dx12 the
# define, the include directory and the dependency (the headers must exist before na1.cpp compiles).
set(NA_HAVE_NA1 OFF)
set(_na1_fxc "${NA_REFEREE_FXC}")
if(NOT _na1_fxc)
  find_program(NA_NA1_FXC NAMES fxc)
  if(NOT NA_NA1_FXC)
    set(_na1_program_files "ProgramFiles(x86)")
    file(GLOB _na1_fxc_candidates
      "$ENV{${_na1_program_files}}/Windows Kits/10/bin/10.*/x64/fxc.exe"
      "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(_na1_fxc_candidates)
      list(SORT _na1_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
      list(GET _na1_fxc_candidates 0 NA_NA1_FXC)
    endif()
  endif()
  set(_na1_fxc "${NA_NA1_FXC}")
endif()
if(_na1_fxc)
  set(NA_HAVE_NA1 ON)
  set(_na1_src "${CMAKE_SOURCE_DIR}/src/host_dx12/effects/na1.hlsl")
  set(_na1_out "${CMAKE_BINARY_DIR}/generated/na1")
  file(MAKE_DIRECTORY "${_na1_out}")
  set(NA_NA1_GENERATED "")
  foreach(_entry Prep BlurX BlurY Down Compose Measure)
    set(_out "${_na1_out}/na1_${_entry}.h")
    # /Gis: strict IEEE arithmetic, so the card adds and divides in the order the processor
    # reference does (the same flags the referee's programs are built with).
    add_custom_command(OUTPUT "${_out}"
      COMMAND "${_na1_fxc}" /nologo /T cs_5_0 /E "${_entry}" /O3 /Gis
              /Fh "${_out}" /Vn "na_na1_${_entry}" "${_na1_src}"
      DEPENDS "${_na1_src}"
      COMMENT "FXC: NA1 ${_entry}"
      VERBATIM)
    list(APPEND NA_NA1_GENERATED "${_out}")
  endforeach()
  add_custom_target(na1_shaders DEPENDS ${NA_NA1_GENERATED})
else()
  message(STATUS "Windows SDK FXC unavailable: the NA1 enhancer is built as a stub that says so")
endif()
