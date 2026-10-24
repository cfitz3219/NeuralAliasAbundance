# A3 portable offline reference scene and candidate qualification.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeuralAliasFixture LANGUAGES NONE)
  enable_testing()
endif()
find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(STATUS "A3: Python 3.8+ unavailable; fixture tests skipped (host build remains available)")
  return()
endif()
add_test(NAME na.fixture.reference
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_fixture.py" -v)
set_tests_properties(na.fixture.reference PROPERTIES LABELS "cpu;fixture" TIMEOUT 120)
