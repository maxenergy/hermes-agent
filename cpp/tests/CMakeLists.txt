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

# --- Integration tests ---------------------------------------------------

add_executable(hermes_integration_tests
    integration_tests.cpp
    equivalence_test_framework.cpp
)

target_link_libraries(hermes_integration_tests
    PRIVATE
        hermes::agent
        hermes::tools
        hermes::state
        hermes::cron
        hermes::config
        hermes::llm
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

target_include_directories(hermes_integration_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

gtest_discover_tests(hermes_integration_tests
    DISCOVERY_MODE PRE_TEST
)

# --- Performance benchmarks -----------------------------------------------

add_executable(hermes_benchmarks
    performance_benchmark.cpp
)

target_link_libraries(hermes_benchmarks
    PRIVATE
        hermes::tools
        hermes::state
        hermes::cron
        hermes::config
        hermes::core
        hermes::llm
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_benchmarks
    DISCOVERY_MODE PRE_TEST
)
