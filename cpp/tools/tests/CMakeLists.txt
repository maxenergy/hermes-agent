# Tool tests — depend on hermes::tools and GoogleTest.  GoogleTest is
# pulled in by the upstream test subdirectories; if hermes::tools is the
# only test target being built we fetch it here.

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

add_executable(hermes_tools_tests
    test_registry.cpp
)

target_link_libraries(hermes_tools_tests
    PRIVATE
        hermes::tools
        hermes::core
        hermes::llm
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_tools_tests
    DISCOVERY_MODE PRE_TEST
)
