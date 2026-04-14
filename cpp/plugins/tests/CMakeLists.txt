if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

set(HERMES_PLUGINS_TEST_SOURCES
    test_plugin_manager.cpp
    test_rebuild_lookups.cpp
)

add_executable(hermes_plugins_tests ${HERMES_PLUGINS_TEST_SOURCES})
target_link_libraries(hermes_plugins_tests
    PRIVATE
        hermes::plugins
        hermes::cli
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_plugins_tests
    DISCOVERY_MODE PRE_TEST
)

# CLI-specific plugin tests — standalone binary so it does not transitively
# pull in hermes::cli (and therefore hermes::gateway), which keeps the
# plugin subsystem buildable in isolation.
add_executable(hermes_plugins_cli_tests test_plugins_cli.cpp)
target_link_libraries(hermes_plugins_cli_tests
    PRIVATE
        hermes::plugins
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_plugins_cli_tests
    DISCOVERY_MODE PRE_TEST
)
