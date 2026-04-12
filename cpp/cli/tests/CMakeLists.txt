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

set(HERMES_CLI_TEST_SOURCES
    test_commands.cpp
    test_skin_engine.cpp
    test_hermes_cli.cpp
    test_display.cpp
    test_main_entry.cpp
    test_cli_commands.cpp
    test_clipboard.cpp
    test_colors.cpp
    test_claw_migrate.cpp
)

add_executable(hermes_cli_tests ${HERMES_CLI_TEST_SOURCES})
target_link_libraries(hermes_cli_tests
    PRIVATE
        hermes::cli
        hermes::core
        hermes::config
        hermes::tools
        nlohmann_json::nlohmann_json
        GTest::gtest_main
        hermes_warnings
)

# Allow test code to use -Wno-unused-parameter for mock overrides.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hermes_cli_tests PRIVATE -Wno-unused-parameter)
endif()

gtest_discover_tests(hermes_cli_tests
    DISCOVERY_MODE PRE_TEST
)
