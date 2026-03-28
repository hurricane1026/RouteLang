add_executable(test_framework test_framework.cc)
target_include_directories(test_framework PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_framework COMMAND test_framework)
set_tests_properties(test_framework PROPERTIES WILL_FAIL TRUE)

add_executable(test_network test_network.cc)
target_link_libraries(test_network rue_runtime)
target_include_directories(test_network PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_network COMMAND test_network)
set_tests_properties(test_network PROPERTIES LABELS "unit;network")

find_package(Threads REQUIRED)

add_executable(test_integration test_integration.cc)
target_link_libraries(test_integration rue_runtime Threads::Threads)
target_include_directories(test_integration PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_integration COMMAND test_integration)
set_tests_properties(test_integration PROPERTIES LABELS "integration;network")

add_executable(test_arena test_arena.cc ${PROJECT_SOURCE_DIR}/src/placement_new.cc)
target_include_directories(test_arena PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_arena COMMAND test_arena)
set_tests_properties(test_arena PROPERTIES LABELS "unit")

add_executable(test_expected test_expected.cc ${PROJECT_SOURCE_DIR}/src/placement_new.cc)
target_include_directories(test_expected PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_expected COMMAND test_expected)
set_tests_properties(test_expected PROPERTIES LABELS "unit")

add_executable(test_http_parser test_http_parser.cc)
target_link_libraries(test_http_parser rue_runtime)
target_include_directories(test_http_parser PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_http_parser COMMAND test_http_parser)
set_tests_properties(test_http_parser PROPERTIES LABELS "unit")

add_executable(test_buffer test_buffer.cc)
target_include_directories(test_buffer PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_buffer COMMAND test_buffer)
set_tests_properties(test_buffer PROPERTIES LABELS "unit")

add_executable(test_types test_types.cc)
target_include_directories(test_types PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_types COMMAND test_types)
set_tests_properties(test_types PROPERTIES LABELS "unit")

add_executable(test_rir test_rir.cc ${PROJECT_SOURCE_DIR}/src/placement_new.cc)
target_link_libraries(test_rir rue_compiler)
target_include_directories(test_rir PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
)
add_test(NAME test_rir COMMAND test_rir)
set_tests_properties(test_rir PROPERTIES LABELS "unit")

add_executable(test_drain test_drain.cc)
target_link_libraries(test_drain rue_runtime)
target_include_directories(test_drain PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_drain COMMAND test_drain)
set_tests_properties(test_drain PROPERTIES LABELS "unit")

add_executable(test_access_log test_access_log.cc)
target_link_libraries(test_access_log rue_runtime)
target_include_directories(test_access_log PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_access_log COMMAND test_access_log)
set_tests_properties(test_access_log PROPERTIES LABELS "unit")

add_executable(test_metrics test_metrics.cc)
target_link_libraries(test_metrics rue_runtime)
target_include_directories(test_metrics PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_metrics COMMAND test_metrics)
set_tests_properties(test_metrics PROPERTIES LABELS "unit")

add_executable(test_shard_control test_shard_control.cc)
target_link_libraries(test_shard_control rue_runtime Threads::Threads)
target_include_directories(test_shard_control PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/testing
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_shard_control COMMAND test_shard_control)
set_tests_properties(test_shard_control PROPERTIES LABELS "unit")

add_custom_target(check
    COMMAND $<TARGET_FILE:test_network>
    COMMAND $<TARGET_FILE:test_integration>
    COMMAND $<TARGET_FILE:test_arena>
    COMMAND $<TARGET_FILE:test_expected>
    COMMAND $<TARGET_FILE:test_http_parser>
    COMMAND $<TARGET_FILE:test_buffer>
    COMMAND $<TARGET_FILE:test_types>
    COMMAND $<TARGET_FILE:test_rir>
    COMMAND $<TARGET_FILE:test_drain>
    COMMAND $<TARGET_FILE:test_access_log>
    COMMAND $<TARGET_FILE:test_metrics>
    COMMAND $<TARGET_FILE:test_shard_control>
    DEPENDS test_network test_integration test_arena test_expected test_http_parser test_buffer test_types test_rir test_drain test_access_log test_metrics test_shard_control
    COMMENT "Running all tests..."
)
