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

# Non-gateway joint tests — all use nlohmann_json v3.11.3 (matches agent/tools/
# skills/auth/state/llm).
add_executable(hermes_joint_tests
    test_agent_with_tools.cpp
    test_skills_pipeline.cpp
    test_delegate_subagent.cpp
    test_qwen_oauth_pipeline.cpp
    test_session_persistence.cpp
)

target_link_libraries(hermes_joint_tests
    PRIVATE
        hermes::agent
        hermes::tools
        hermes::skills
        hermes::auth
        hermes::state
        hermes::llm
        hermes::approval
        hermes::cron
        hermes::config
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hermes_joint_tests PRIVATE -Wno-unused-parameter)
endif()

gtest_discover_tests(hermes_joint_tests DISCOVERY_MODE PRE_TEST)

# Gateway joint tests — hermes::gateway is built against vcpkg's nlohmann_json
# 3.12.0 due to transitive OpenSSL/olm -isystem paths, so keep this TU in its
# own binary to avoid ABI mixing.
add_executable(hermes_joint_gateway_tests
    test_gateway_message_flow.cpp
)

target_link_libraries(hermes_joint_gateway_tests
    PRIVATE
        hermes::gateway
        hermes::core
        hermes::config
        GTest::gtest_main
        hermes_warnings
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hermes_joint_gateway_tests PRIVATE -Wno-unused-parameter)
endif()

gtest_discover_tests(hermes_joint_gateway_tests DISCOVERY_MODE PRE_TEST)
