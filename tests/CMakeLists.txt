add_executable(test_framework test_framework.cc)
target_include_directories(test_framework PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_executable(test_network test_network.cc)
target_link_libraries(test_network rue_runtime)
target_include_directories(test_network PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(test_integration test_integration.cc)
target_link_libraries(test_integration rue_runtime)
target_include_directories(test_integration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(check
    COMMAND test_network
    COMMAND test_integration
    DEPENDS test_network test_integration
    COMMENT "Running all tests..."
)
