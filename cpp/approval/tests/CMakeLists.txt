include(GoogleTest)

add_executable(hermes_approval_tests
    test_danger_patterns.cpp
    test_command_scanner.cpp
    test_session_state.cpp
    test_gateway_queue.cpp
    test_website_policy.cpp
    test_skills_guard.cpp
    test_mcp_credential_strip.cpp
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
