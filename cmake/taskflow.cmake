# MIT Taskflow v4.1.0 headers, exact pin45366fe5bc4f2f8ec9aa590b40c504e296886865.
# Do not run upstream project CMake or change our core's C++ standard.
add_library(na_taskflow_headers INTERFACE)
target_include_directories(na_taskflow_headers SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party/taskflow")
