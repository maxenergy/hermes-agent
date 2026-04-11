# GoogleTest is provided by the hermes_core tests subdirectory via
# FetchContent. If hermes_state is ever built without hermes_core's
# tests we pull it in again here — FetchContent_MakeAvailable is
# idempotent when the target already exists.
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

set(HERMES_STATE_TEST_SOURCES
    test_session_db.cpp
    test_process_registry.cpp
    test_memory_store.cpp
    test_trajectory.cpp
)

add_executable(hermes_state_tests ${HERMES_STATE_TEST_SOURCES})
target_link_libraries(hermes_state_tests
    PRIVATE
        hermes::state
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_state_tests
    DISCOVERY_MODE PRE_TEST
)
