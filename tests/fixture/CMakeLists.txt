# A3 portable offline reference scene and candidate qualification.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeuralAliasFixture LANGUAGES NONE)
  enable_testing()
endif()
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME na.fixture.reference
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_fixture.py" -v)
set_tests_properties(na.fixture.reference PROPERTIES LABELS "cpu;fixture" TIMEOUT 120)
