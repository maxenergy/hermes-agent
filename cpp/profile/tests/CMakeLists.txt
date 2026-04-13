include(GoogleTest)

add_executable(hermes_profile_tests
    test_profile.cpp
    test_profile_bootstrap.cpp
)

target_link_libraries(hermes_profile_tests
    PRIVATE
        hermes::profile
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_profile_tests
    DISCOVERY_MODE PRE_TEST
)
