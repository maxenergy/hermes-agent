include(GoogleTest)

set(HERMES_ENV_TEST_SOURCES
    test_env_filter.cpp
    test_cwd_tracker.cpp
    test_local.cpp
    test_pty.cpp
    test_docker.cpp
    test_ssh.cpp
    test_file_sync.cpp
    test_snapshot_store.cpp
    test_modal_environment.cpp
    test_daytona_environment.cpp
    test_singularity_environment.cpp
    test_managed_modal.cpp
    test_dotfile_sync.cpp
    test_scp_copier.cpp
    test_spawn_via_env.cpp
    test_tool_context.cpp
)

add_executable(hermes_environments_tests ${HERMES_ENV_TEST_SOURCES})
target_link_libraries(hermes_environments_tests
    PRIVATE
        hermes_environments
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_environments_tests
    DISCOVERY_MODE PRE_TEST
)
