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

add_executable(hermes_batch_tests
    test_batch_runner.cpp
    test_trajectory_compressor.cpp
)

target_link_libraries(hermes_batch_tests
    PRIVATE
        hermes_batch
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_batch_tests
    DISCOVERY_MODE PRE_TEST
)
