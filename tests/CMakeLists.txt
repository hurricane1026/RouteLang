add_executable(test_framework test_framework.cc)
target_include_directories(test_framework PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_executable(test_network test_network.cc)
target_link_libraries(test_network rue_runtime)
target_include_directories(test_network PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(Threads REQUIRED)

add_executable(test_integration test_integration.cc)
target_link_libraries(test_integration rue_runtime Threads::Threads)
target_include_directories(test_integration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(test_arena test_arena.cc)
target_include_directories(test_arena PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_executable(test_expected test_expected.cc)
target_include_directories(test_expected PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_custom_target(check
    COMMAND test_network
    COMMAND test_integration
    COMMAND test_arena
    COMMAND test_expected
    DEPENDS test_network test_integration test_arena test_expected
    COMMENT "Running all tests..."
)
