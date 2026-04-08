cmake_minimum_required(VERSION 3.20)

include(CheckIPOSupported)

project(rut
    VERSION 0.1.0
    DESCRIPTION "A strongly-typed DSL and high-performance HTTP gateway runtime"
    LANGUAGES CXX C
)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Core compiler flags per DESIGN.md: no exceptions, no RTTI, no C++ stdlib
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic")
# Use CMake's native LTO support — handles llvm-ar/gcc-ar automatically.
# Applied to both C and C++ so mixed-language targets (bench/llhttp) link correctly.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
check_ipo_supported(RESULT RUE_IPO_SUPPORTED OUTPUT RUE_IPO_OUTPUT LANGUAGES C CXX)
if(RUE_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG")

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")

# LLVM — required for JIT compilation (ORC JIT via C API).
# Only the rut_jit library links LLVM; rut_runtime stays LLVM-free.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Project include paths
include_directories(include)

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(bench)
