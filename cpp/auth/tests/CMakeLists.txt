include(GoogleTest)

add_executable(hermes_auth_tests
    test_credentials.cpp
    test_env_loader.cpp
    test_env_writer.cpp
    test_copilot_oauth.cpp
    test_nous_subscription.cpp
    test_mcp_oauth.cpp
    test_codex_oauth.cpp
    test_gemini_oauth.cpp
)

target_link_libraries(hermes_auth_tests
    PRIVATE
        hermes::auth
        hermes::core
        hermes::llm
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_auth_tests
    DISCOVERY_MODE PRE_TEST
)
