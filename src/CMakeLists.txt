# SIMD architecture selection
set(SIMD_ARCH "auto" CACHE STRING "SIMD architecture: auto, scalar, sse2, avx2, avx512, neon, sve")
set_property(CACHE SIMD_ARCH PROPERTY STRINGS auto scalar sse2 avx2 avx512 neon sve)

if(SIMD_ARCH STREQUAL "auto")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(SIMD_ARCH "neon")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        set(SIMD_ARCH "sse2")
    else()
        set(SIMD_ARCH "scalar")
    endif()
endif()

message(STATUS "SIMD backend: ${SIMD_ARCH}")

if(SIMD_ARCH STREQUAL "avx512")
    set(SIMD_SOURCE runtime/simd/avx512.cc)
    set(SIMD_FLAGS "-mavx512f;-mavx512bw")
elseif(SIMD_ARCH STREQUAL "avx2")
    set(SIMD_SOURCE runtime/simd/avx2.cc)
    set(SIMD_FLAGS "-mavx2")
elseif(SIMD_ARCH STREQUAL "sse2")
    set(SIMD_SOURCE runtime/simd/sse2.cc)
    set(SIMD_FLAGS "-msse2")
elseif(SIMD_ARCH STREQUAL "neon")
    set(SIMD_SOURCE runtime/simd/neon.cc)
    set(SIMD_FLAGS "")
elseif(SIMD_ARCH STREQUAL "sve")
    set(SIMD_SOURCE runtime/simd/sve.cc)
    set(SIMD_FLAGS "-march=armv8-a+sve")
elseif(SIMD_ARCH STREQUAL "scalar")
    set(SIMD_SOURCE runtime/simd/scalar.cc)
    set(SIMD_FLAGS "")
else()
    message(FATAL_ERROR "Unknown SIMD_ARCH '${SIMD_ARCH}'. Valid options: auto, scalar, sse2, avx2, avx512, neon, sve")
endif()

# Runtime library
add_library(rue_runtime STATIC
    placement_new.cc
    runtime/shard.cc
    runtime/io_uring_backend.cc
    runtime/epoll_backend.cc
    runtime/timer_wheel.cc
    runtime/socket.cc
    runtime/http_parser.cc
    ${SIMD_SOURCE}
)

target_include_directories(rue_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# SIMD source needs to find simd.h and char_tables.h via "simd.h" include
target_include_directories(rue_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/simd
)

# Apply ISA-specific flags only to the SIMD translation unit
if(SIMD_FLAGS)
    set_source_files_properties(${SIMD_SOURCE}
        PROPERTIES COMPILE_OPTIONS "${SIMD_FLAGS}"
    )
endif()

# Compiler library
add_library(rue_compiler STATIC
    compiler/lexer.cc
    compiler/rir_printer.cc
)

target_include_directories(rue_compiler PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Main executable
add_executable(rue main.cc)
target_link_libraries(rue
    rue_runtime
    rue_compiler
)
