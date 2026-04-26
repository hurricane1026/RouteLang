# SIMD architecture selection
set(SIMD_ARCH "auto" CACHE STRING "SIMD architecture: auto, scalar, sse2, avx2, avx512, neon, sve")
set_property(CACHE SIMD_ARCH PROPERTY STRINGS auto scalar sse2 avx2 avx512 neon sve)

# Temporarily disable testing while configuring BoringSSL so its internal
# test targets do not affect the rest of the project.
if(DEFINED BUILD_TESTING)
    set(_BORINGSSL_PREV_BUILD_TESTING_DEFINED TRUE)
    set(_BORINGSSL_PREV_BUILD_TESTING "${BUILD_TESTING}")
else()
    set(_BORINGSSL_PREV_BUILD_TESTING_DEFINED FALSE)
endif()
set(BUILD_TESTING OFF)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/boringssl
    ${CMAKE_CURRENT_BINARY_DIR}/third_party/boringssl
    EXCLUDE_FROM_ALL
)
if(_BORINGSSL_PREV_BUILD_TESTING_DEFINED)
    set(BUILD_TESTING "${_BORINGSSL_PREV_BUILD_TESTING}")
else()
    unset(BUILD_TESTING)
endif()
unset(_BORINGSSL_PREV_BUILD_TESTING_DEFINED)
unset(_BORINGSSL_PREV_BUILD_TESTING)
foreach(BORINGSSL_TARGET fipsmodule crypto ssl decrepit pki)
    if(TARGET ${BORINGSSL_TARGET})
        target_compile_options(${BORINGSSL_TARGET} PRIVATE
            -Wno-error
            -Wno-error=pedantic
            -Wno-error=unused-parameter
            -Wno-pedantic
        )
    endif()
endforeach()

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

find_package(Threads REQUIRED)

# zstd — vendored, compress only, stripped strategies (fast + doubleFast only).
# Excludes greedy/lazy/btlazy2/btopt/btultra to keep binary small (~120KB vs ~400KB).
# Level 1-4 use fast, level 5 uses doubleFast. Higher levels are unsupported.
set(ZSTD_DIR ${PROJECT_SOURCE_DIR}/third_party/zstd/lib)
add_library(zstd_compress STATIC
    ${ZSTD_DIR}/compress/zstd_compress.c
    ${ZSTD_DIR}/compress/zstd_compress_literals.c
    ${ZSTD_DIR}/compress/zstd_compress_sequences.c
    ${ZSTD_DIR}/compress/zstd_compress_superblock.c
    ${ZSTD_DIR}/compress/zstd_fast.c
    ${ZSTD_DIR}/compress/zstd_double_fast.c
    ${ZSTD_DIR}/compress/zstd_lazy.c
    ${ZSTD_DIR}/compress/zstd_opt.c
    ${ZSTD_DIR}/compress/zstd_ldm.c
    ${ZSTD_DIR}/compress/zstd_preSplit.c
    ${ZSTD_DIR}/compress/fse_compress.c
    ${ZSTD_DIR}/compress/huf_compress.c
    ${ZSTD_DIR}/compress/hist.c
    ${ZSTD_DIR}/common/zstd_common.c
    ${ZSTD_DIR}/common/entropy_common.c
    ${ZSTD_DIR}/common/error_private.c
    ${ZSTD_DIR}/common/debug.c
    ${ZSTD_DIR}/common/xxhash.c
    ${ZSTD_DIR}/common/fse_decompress.c
)
target_include_directories(zstd_compress PUBLIC ${ZSTD_DIR} ${ZSTD_DIR}/common)
set_target_properties(zstd_compress PROPERTIES LANGUAGE C)
target_compile_options(zstd_compress PRIVATE
    -O2 -DNDEBUG
    -DZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR
    -DZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR
    -DZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR
    -DZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR
    -DZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
    -DZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR
    -ffunction-sections -fdata-sections
)

# Runtime library
add_library(rut_runtime STATIC
    placement_new.cc
    runtime/arena.cc
    runtime/callbacks.cc
    runtime/shard.cc
    runtime/io_uring_backend.cc
    runtime/epoll_backend.cc
    runtime/timer_wheel.cc
    runtime/socket.cc
    runtime/tls.cc
    runtime/http_parser.cc
    runtime/chunked_parser.cc
    runtime/access_log.cc
    runtime/traffic_capture.cc
    runtime/cpu_caps.cc
    runtime/route_art.cc
    runtime/route_byte_radix.cc
    runtime/route_dispatch.cc
    runtime/route_hash_first_seg.cc
    runtime/route_hash_full.cc
    runtime/route_select.cc
    runtime/route_trie.cc
    # SIMD LinearScan variants — ALL four are always compiled
    # regardless of host arch. Each .cc file has a #if-guard around
    # the SIMD intrinsics; on non-applicable hosts (e.g. AVX-512 on
    # ARM, NEON on x86) the file compiles to a delegating stub. The
    # picker uses runtime CpuCaps to install the right one. This is
    # different from the parser-side ${SIMD_SOURCE} pattern (which
    # picks one ISA at build time) — route dispatch needs runtime
    # selection because the SAME binary may be distributed to many
    # different CPUs in mesh mode.
    runtime/simd/route_linear_sse2.cc
    runtime/simd/route_linear_avx2.cc
    runtime/simd/route_linear_avx512.cc
    runtime/simd/route_linear_neon.cc
    ${SIMD_SOURCE}
)

target_include_directories(rut_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(rut_runtime
    zstd_compress
    ssl
    crypto
    Threads::Threads
)

# SIMD source needs to find simd.h and char_tables.h via "simd.h" include
target_include_directories(rut_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/simd
)

# Apply ISA-specific flags only to the SIMD translation unit
if(SIMD_FLAGS)
    set_source_files_properties(${SIMD_SOURCE}
        PROPERTIES COMPILE_OPTIONS "${SIMD_FLAGS}"
    )
endif()

# Per-arch flags for the runtime-selected SIMD LinearScan dispatches.
# On x86_64 we hand each .cc the matching `-m<isa>` flag so its
# intrinsics compile; the .cc itself has a `#if defined(__ISA__)`
# guard so that non-applicable hosts (where the flag has no effect
# or is rejected) fall through to a stub.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set_source_files_properties(runtime/simd/route_linear_sse2.cc
        PROPERTIES COMPILE_OPTIONS "-msse2"
    )
    set_source_files_properties(runtime/simd/route_linear_avx2.cc
        PROPERTIES COMPILE_OPTIONS "-mavx2"
    )
    set_source_files_properties(runtime/simd/route_linear_avx512.cc
        PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw"
    )
endif()
# NEON is mandatory in the ARMv8 baseline — no extra flag needed on
# aarch64. The route_linear_neon.cc file's #ifdef __aarch64__ does
# the right thing on x86 hosts (compiles as stub, no NEON intrinsics
# referenced).

# Compiler library
add_library(rut_compiler STATIC
    compiler/lexer.cc
    compiler/parser.cc
    compiler/analyze.cc
    compiler/mir_build.cc
    compiler/lower_rir.cc
    compiler/rir_printer.cc
)

target_include_directories(rut_compiler PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# JIT library — LLVM ORC JIT integration via C API.
# Only this library links LLVM. rut_runtime stays LLVM-free so shard
# threads never transitively depend on LLVM headers or libraries.
add_library(rut_jit STATIC
    jit/jit_engine.cc
    jit/codegen.cc
    jit/runtime_helpers.cc
)

target_include_directories(rut_jit PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)

# Prefer the monolithic shared LLVM dylib when the packaged config exposes it.
# This avoids depending on broken per-component static archives in environments
# where LLVM is installed in shared mode.
if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
    set(LLVM_LIBS LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS core orcjit orcshared orctargetprocess native support)
endif()
target_link_libraries(rut_jit ${LLVM_LIBS})

# JIT .cc files may need LLVM definitions that conflict with -fno-rtti.
# The LLVM C API is pure C so no RTTI issues, but LLVM's cmake may add
# -fno-rtti to definitions. Add LLVM definitions selectively.
target_compile_definitions(rut_jit PRIVATE ${LLVM_DEFINITIONS})

# rut_jit also links rut_runtime for runtime_helpers (uses HttpParser, Connection)
target_link_libraries(rut_jit rut_runtime)

# Offline simulate library — JIT-backed capture validation with no event loop.
add_library(rut_simulate STATIC
    sim/simulate_engine.cc
)
target_include_directories(rut_simulate PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(rut_simulate PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(rut_simulate
    rut_runtime
    rut_compiler
    rut_jit
)

# Main executable
add_executable(rut main.cc)
target_link_libraries(rut
    rut_runtime
    rut_compiler
    rut_jit
)

add_executable(rut-simulate sim/main.cc)
target_link_libraries(rut-simulate
    rut_simulate
)
