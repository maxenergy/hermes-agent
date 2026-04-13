include(FetchContent)

# Pin GoogleTest — kept out of vcpkg so Phase 0 builds on a clean box.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED TRUE
)

# Prevent GoogleTest from overriding compiler/linker flags on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(HERMES_CORE_TEST_SOURCES
    test_strings.cpp
    test_path.cpp
    test_atomic_io.cpp
    test_env.cpp
    test_time.cpp
    test_redact.cpp
    test_retry.cpp
    test_url_safety.cpp
    test_ansi_strip.cpp
    test_fuzzy.cpp
    test_patch_parser.cpp
    test_async_bridge.cpp
)

add_executable(hermes_core_tests ${HERMES_CORE_TEST_SOURCES})
target_link_libraries(hermes_core_tests
    PRIVATE
        hermes_core
        GTest::gtest_main
)
# Don't apply -Werror to the upstream GoogleTest sources.
target_link_libraries(hermes_core_tests PRIVATE hermes_warnings)

gtest_discover_tests(hermes_core_tests
    DISCOVERY_MODE PRE_TEST
)
