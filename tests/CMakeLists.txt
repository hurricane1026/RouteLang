add_executable(test_framework test_framework.cc)
target_include_directories(test_framework PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)

add_executable(test_network test_network.cc)
target_link_libraries(test_network rue_runtime)
target_include_directories(test_network PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)

add_executable(test_integration test_integration.cc)
target_link_libraries(test_integration rue_runtime Threads::Threads)
target_include_directories(test_integration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(test_arena test_arena.cc ${PROJECT_SOURCE_DIR}/src/placement_new.cc)
target_include_directories(test_arena PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)

add_executable(test_expected test_expected.cc ${PROJECT_SOURCE_DIR}/src/placement_new.cc)
target_include_directories(test_expected PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)

add_executable(test_http_parser test_http_parser.cc)
target_link_libraries(test_http_parser rue_runtime)
target_include_directories(test_http_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(test_buffer test_buffer.cc)
target_include_directories(test_buffer PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)

add_custom_target(check
    COMMAND $<TARGET_FILE:test_network>
    COMMAND $<TARGET_FILE:test_integration>
    COMMAND $<TARGET_FILE:test_arena>
    COMMAND $<TARGET_FILE:test_expected>
    COMMAND $<TARGET_FILE:test_http_parser>
    COMMAND $<TARGET_FILE:test_buffer>
    DEPENDS test_network test_integration test_arena test_expected test_http_parser test_buffer
    COMMENT "Running all tests..."
)
