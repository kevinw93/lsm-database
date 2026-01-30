cmake_minimum_required(VERSION 3.25)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LSM_BUILD_TESTS "Build unit tests" ON)
option(LSM_BUILD_BENCHMARKS "Build benchmarks" ON)
option(LSM_WERROR "Treat compiler warnings as errors" OFF)
set(LSM_SANITIZER "" CACHE STRING "Sanitizer build: '', 'asan', or 'tsan'")
set_property(CACHE LSM_SANITIZER PROPERTY STRINGS "" "asan" "tsan")

# Global so GoogleTest is instrumented too; TSan needs every TU instrumented.
if(LSM_SANITIZER STREQUAL "asan")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
elseif(LSM_SANITIZER STREQUAL "tsan")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT LSM_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown LSM_SANITIZER '${LSM_SANITIZER}'")
endif()

add_library(lsm_warnings INTERFACE)
target_compile_options(lsm_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast)
if(LSM_WERROR)
  target_compile_options(lsm_warnings INTERFACE -Werror)
endif()

add_library(lsm
  src/db/dbformat.cc
  src/status.cc
  src/util/coding.cc
  src/util/crc32c.cc
)
target_include_directories(lsm
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lsm PRIVATE lsm_warnings)

if(LSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LSM_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
