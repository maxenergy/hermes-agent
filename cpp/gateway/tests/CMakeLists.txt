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

set(HERMES_GATEWAY_TEST_SOURCES
    test_gateway_config.cpp
    test_session_store.cpp
    test_hooks.cpp
    test_pairing.cpp
    test_status.cpp
    test_gateway_runner.cpp
    test_platform_adapters.cpp
    test_stream_consumer.cpp
    test_channel_directory.cpp
    test_message_mirror.cpp
    test_sticker_cache.cpp
    test_restart.cpp
    test_gateway_lifecycle.cpp
    test_email_imap.cpp
    test_telegram_reactions.cpp
    test_telegram_full.cpp
    test_slack_threads.cpp
    test_discord_threads.cpp
    test_discord_full.cpp
    test_discord_voice.cpp
    test_discord_gateway.cpp
    test_slack_socket_mode.cpp
    test_matrix_e2ee.cpp
    test_media_cache.cpp
    test_safe_fetch.cpp
    test_slack_subcommand_router.cpp
    test_channel_cache.cpp
    test_gateway_polish.cpp
    test_status_introspection.cpp
    test_whatsapp_pairing.cpp
    test_whatsapp_ephemeral.cpp
    test_signal_encryption.cpp
)

add_executable(hermes_gateway_tests ${HERMES_GATEWAY_TEST_SOURCES})
target_link_libraries(hermes_gateway_tests
    PRIVATE
        hermes::gateway
        hermes::core
        hermes::config
        nlohmann_json::nlohmann_json
        GTest::gtest_main
        hermes_warnings
)

# Allow test code to use -Wno-unused-parameter for mock overrides.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hermes_gateway_tests PRIVATE -Wno-unused-parameter)
endif()

gtest_discover_tests(hermes_gateway_tests
    DISCOVERY_MODE PRE_TEST
)
