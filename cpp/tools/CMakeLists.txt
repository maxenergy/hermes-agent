# hermes::tools — Phase 5 tool registry, toolset resolution, dispatch
# plumbing, and small peripheral helpers.
#
# Depends on hermes::core (logging / utility helpers) and hermes::llm
# (only the ToolSchema struct, for get_definitions()).  nlohmann_json is
# pulled in transitively from hermes::llm.

include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "hermes::tools — fetching nlohmann_json 3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(hermes_tools STATIC
    src/registry.cpp
    src/toolsets.cpp
    src/toolset_distributions.cpp
    src/tool_result.cpp
    src/tool_result_storage.cpp
    src/interrupt.cpp
    src/credential_files.cpp
    src/binary_extensions.cpp
    src/managed_tool_gateway.cpp
    src/env_passthrough.cpp
    src/file_operations.cpp
    src/file_tools.cpp
    src/terminal_tool.cpp
    src/terminal_tool_depth.cpp
    src/discover_tools.cpp
    src/web_tools.cpp
    src/web_tools_depth.cpp
    src/vision_tool.cpp
    src/send_message_tool.cpp
    src/send_message_tool_depth.cpp
    src/skills_tool_depth.cpp
    src/skill_manager_tool_depth.cpp
    src/terminal_tool_depth_ex.cpp
    src/web_tools_depth_ex.cpp
    src/image_generation_tool.cpp
    src/tts_tool.cpp
    src/delegate_tool.cpp
    src/memory_tool.cpp
    src/todo_tool.cpp
    src/clarify_tool.cpp
    src/skills_tool.cpp
    src/session_search_tool.cpp
    src/homeassistant_tool.cpp
    src/simple_ws.cpp
    src/cdp_backend.cpp
    src/browser_backend.cpp
    src/browser_camofox.cpp
    src/browser_tool.cpp
    src/browser_tool_depth.cpp
    src/browser_helpers.cpp
    src/terminal_helpers.cpp
    src/debug_helpers.cpp
    src/tool_backend_helpers.cpp
    src/transcription_tool.cpp
    src/voice_mode_tool.cpp
    src/rl_training_tool.cpp
    src/code_execution_tool.cpp
    src/code_execution_depth.cpp
    src/cronjob_tool.cpp
    src/skill_manager_tool.cpp
    src/mcp_client_tool.cpp
    src/mcp_registration.cpp
    src/mcp_transport.cpp
    src/ffmpeg.cpp
)

target_include_directories(hermes_tools
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_tools PUBLIC cxx_std_17)

target_link_libraries(hermes_tools
    PUBLIC
        hermes::core
        hermes::llm
        hermes::environments
        hermes::state
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes::state
        hermes::skills
        hermes_warnings
)

add_library(hermes::tools ALIAS hermes_tools)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
