# Runtime library
add_library(rue_runtime STATIC
    placement_new.cc
    runtime/shard.cc
    runtime/io_uring_backend.cc
    runtime/epoll_backend.cc
    runtime/timer_wheel.cc
    runtime/socket.cc
    runtime/http_parser.cc

)

target_include_directories(rue_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}/include

)

# Compiler library
add_library(rue_compiler STATIC
    compiler/lexer.cc
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
