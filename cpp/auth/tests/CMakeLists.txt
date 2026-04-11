include(GoogleTest)

add_executable(hermes_auth_tests
    test_credentials.cpp
    test_env_loader.cpp
)

target_link_libraries(hermes_auth_tests
    PRIVATE
        hermes::auth
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_auth_tests
    DISCOVERY_MODE PRE_TEST
)
