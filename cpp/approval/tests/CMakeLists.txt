include(GoogleTest)

add_executable(hermes_approval_tests
    test_danger_patterns.cpp
    test_command_scanner.cpp
)

target_link_libraries(hermes_approval_tests
    PRIVATE
        hermes::approval
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_approval_tests
    DISCOVERY_MODE PRE_TEST
)
