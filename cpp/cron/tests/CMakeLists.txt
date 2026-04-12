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

set(HERMES_CRON_TEST_SOURCES
    test_cron_parser.cpp
    test_jobs.cpp
    test_scheduler.cpp
    test_delivery.cpp
)

add_executable(hermes_cron_tests ${HERMES_CRON_TEST_SOURCES})
target_link_libraries(hermes_cron_tests
    PRIVATE
        hermes::cron
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_cron_tests
    DISCOVERY_MODE PRE_TEST
)
