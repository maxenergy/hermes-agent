include(GoogleTest)

set(HERMES_CONFIG_TEST_SOURCES
    test_default_config.cpp
    test_env_expansion.cpp
    test_load_merge.cpp
    test_managed_system.cpp
    test_migrate.cpp
    test_migration.cpp
)

add_executable(hermes_config_tests ${HERMES_CONFIG_TEST_SOURCES})
target_link_libraries(hermes_config_tests
    PRIVATE
        hermes::config
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_config_tests
    DISCOVERY_MODE PRE_TEST
)
