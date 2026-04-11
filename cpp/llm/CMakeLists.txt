# hermes_llm — provider-agnostic LLM client abstraction.
#
# Dependencies:
#   * nlohmann_json — JSON manipulation for OpenAI / Anthropic wire formats.
#     Tries find_package first, falls back to FetchContent v3.11.3.
#   * cpr (optional) — HTTP transport. If not found, make_curl_transport()
#     throws at runtime. FakeHttpTransport keeps tests self-contained.

include(FetchContent)

# ── nlohmann_json ───────────────────────────────────────────────────────
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "hermes/llm: nlohmann_json not found — fetching v3.11.3")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ── cpr (optional) ──────────────────────────────────────────────────────
find_package(cpr CONFIG QUIET)
set(HERMES_LLM_HAS_CPR FALSE)
if(cpr_FOUND)
    set(HERMES_LLM_HAS_CPR TRUE)
    message(STATUS "hermes/llm: cpr found — real HTTP transport enabled")
else()
    message(STATUS "hermes/llm: cpr NOT found — make_curl_transport() will throw")
endif()

# ── Library target ──────────────────────────────────────────────────────
add_library(hermes_llm STATIC
    src/message.cpp
    src/usage.cpp
    src/model_metadata.cpp
    src/models_dev.cpp
    src/prompt_cache.cpp
    src/llm_client.cpp
    src/openai_client.cpp
    src/anthropic_client.cpp
    src/openrouter_client.cpp
    src/error_classifier.cpp
    src/retry_policy.cpp
)

target_include_directories(hermes_llm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_llm PUBLIC cxx_std_17)

target_link_libraries(hermes_llm
    PUBLIC
        hermes::core
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes_warnings
)

if(HERMES_LLM_HAS_CPR)
    target_link_libraries(hermes_llm PRIVATE cpr::cpr)
    target_compile_definitions(hermes_llm PRIVATE HERMES_LLM_HAS_CPR=1)
endif()

add_library(hermes::llm ALIAS hermes_llm)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
