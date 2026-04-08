# llhttp (Node.js HTTP parser) — compiled as C
add_library(llhttp STATIC
    ${PROJECT_SOURCE_DIR}/third_party/llhttp/src/llhttp.c
    ${PROJECT_SOURCE_DIR}/third_party/llhttp/src/api.c
    ${PROJECT_SOURCE_DIR}/third_party/llhttp/src/http.c
)
target_include_directories(llhttp PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party/llhttp/include
)
# llhttp is C, not C++ — override flags
set_target_properties(llhttp PROPERTIES
    LINKER_LANGUAGE C
)
target_compile_options(llhttp PRIVATE -O2 -DNDEBUG)

# Benchmark executables
add_executable(bench_http_parser bench_http_parser.cc)
target_link_libraries(bench_http_parser rut_runtime llhttp)
target_include_directories(bench_http_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${PROJECT_SOURCE_DIR}/third_party/llhttp/include
)

add_executable(bench_connection bench_connection.cc)
target_link_libraries(bench_connection rut_runtime)
target_include_directories(bench_connection PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
