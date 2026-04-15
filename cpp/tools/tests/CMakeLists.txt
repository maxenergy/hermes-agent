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
    test_toolsets.cpp
    test_toolset_distributions.cpp
    test_tool_result.cpp
    test_tool_result_storage.cpp
    test_budget.cpp
    test_binary_extensions.cpp
    test_credential_files.cpp
    test_managed_tool_gateway.cpp
    test_env_passthrough.cpp
    test_interrupt.cpp
    test_file_tools.cpp
    test_file_operations.cpp
    test_terminal_tool.cpp
    test_terminal_tool_depth.cpp
    test_web_tools.cpp
    test_web_tools_depth.cpp
    test_vision_tool.cpp
    test_send_message_tool.cpp
    test_image_gen_tool.cpp
    test_tts_tool.cpp
    test_delegate_tool.cpp
    test_memory_tool.cpp
    test_todo_tool.cpp
    test_clarify_tool.cpp
    test_skills_tool.cpp
    test_session_search_tool.cpp
    test_homeassistant_tool.cpp
    test_browser_tool.cpp
    test_cdp_backend.cpp
    test_transcription_tool.cpp
    test_voice_mode_tool.cpp
    test_rl_tools.cpp
    test_code_execution.cpp
    test_code_execution_depth.cpp
    test_cronjob_tool.cpp
    test_skill_manager_tool.cpp
    test_mcp_client.cpp
    test_mcp_registration.cpp
    test_mcp_transport.cpp
    test_browser_camofox.cpp
    test_tool_backend_helpers.cpp
    test_debug_helpers.cpp
    test_ffmpeg.cpp
)

target_link_libraries(hermes_tools_tests
    PRIVATE
        hermes::tools
        hermes::core
        hermes::llm
        hermes::state
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_tools_tests
    DISCOVERY_MODE PRE_TEST
)
