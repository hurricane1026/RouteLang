add_executable(test_network test_network.cc)
target_link_libraries(test_network rue_runtime)
target_include_directories(test_network PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Remove -fno-rtti for tests (EventLoopBase uses virtual)
# Actually we need it for dynamic_cast-free code. The virtuals work without RTTI.

add_custom_target(check
    COMMAND test_network
    DEPENDS test_network
    COMMENT "Running tests..."
)
