# hermes::approval — danger command scanner, session approval state,
# website policy, skills guard, MCP credential stripping. Phase 6.

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    message(STATUS "hermes::approval: nlohmann_json not found — FetchContent v3.11.3")
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Optional re2 — falls back to std::regex when absent. Kept available so a
# future pattern that std::regex can't express can be guarded with
# HERMES_APPROVAL_HAS_RE2.
find_package(re2 CONFIG QUIET)

add_library(hermes_approval STATIC
    src/danger_patterns.cpp
    src/command_scanner.cpp
    src/session_state.cpp
    src/gateway_queue.cpp
    src/website_policy.cpp
    src/skills_guard.cpp
    src/osv_check.cpp
    src/mcp_credential_strip.cpp
    src/tirith_security.cpp
)

target_include_directories(hermes_approval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_approval PUBLIC cxx_std_17)

target_link_libraries(hermes_approval
    PUBLIC
        hermes::core
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes_llm
        hermes_warnings
)

if(re2_FOUND)
    target_compile_definitions(hermes_approval PRIVATE HERMES_APPROVAL_HAS_RE2)
    target_link_libraries(hermes_approval PRIVATE re2::re2)
    message(STATUS "hermes::approval: re2 found — enabling HERMES_APPROVAL_HAS_RE2")
endif()

add_library(hermes::approval ALIAS hermes_approval)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
