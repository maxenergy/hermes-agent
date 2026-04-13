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

# ── HTTP transport: prefer libcurl, fall back to cpr ────────────────────
find_package(CURL QUIET)
find_package(cpr CONFIG QUIET)
set(HERMES_LLM_HAS_CURL FALSE)
set(HERMES_LLM_HAS_CPR FALSE)
if(CURL_FOUND)
    set(HERMES_LLM_HAS_CURL TRUE)
    message(STATUS "hermes/llm: libcurl found — real HTTP transport enabled")
elseif(cpr_FOUND)
    set(HERMES_LLM_HAS_CPR TRUE)
    message(STATUS "hermes/llm: cpr found — real HTTP transport enabled")
else()
    message(STATUS "hermes/llm: no HTTP backend — make_curl_transport() will throw")
endif()

# ── Library target ──────────────────────────────────────────────────────
add_library(hermes_llm STATIC
    src/message.cpp
    src/usage.cpp
    src/model_metadata.cpp
    src/model_normalize.cpp
    src/smart_routing.cpp
    src/models_dev.cpp
    src/prompt_cache.cpp
    src/llm_client.cpp
    src/openai_client.cpp
    src/anthropic_client.cpp
    src/openrouter_client.cpp
    src/error_classifier.cpp
    src/retry_policy.cpp
    src/credential_pool.cpp
    src/codex_models.cpp
    src/runtime_provider.cpp
    src/model_switch.cpp
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

if(HERMES_LLM_HAS_CURL)
    # Static libcurl requires OpenSSL and zlib as transitive deps.
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)

    # CURL::libcurl from vcpkg carries -isystem <vcpkg>/include which
    # contains nlohmann_json 3.12 headers.  That silently overrides the
    # system nlohmann_json 3.11 used everywhere else and causes ABI
    # mismatches.  We only need <curl/curl.h> in llm_client.cpp, so we
    # add CURL's include dirs with explicit -I for just that TU and
    # link the .a directly (bypassing the IMPORTED target's transitive
    # -isystem propagation).
    get_target_property(_curl_incs CURL::libcurl INTERFACE_INCLUDE_DIRECTORIES)
    if(_curl_incs)
        # Per-file INCLUDE_DIRECTORIES keeps curl headers out of other TUs.
        set_property(SOURCE src/llm_client.cpp APPEND PROPERTY
            INCLUDE_DIRECTORIES ${_curl_incs})
    endif()

    # Only llm_client.cpp needs the CURL define.
    set_property(SOURCE src/llm_client.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS HERMES_LLM_HAS_CURL=1)

    # Link libraries by path to avoid IMPORTED targets injecting
    # -isystem <vcpkg>/include into every TU.
    target_link_libraries(hermes_llm PRIVATE
        ${CURL_LIBRARY_RELEASE}
        ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY}
        ${CMAKE_DL_LIBS}
    )
    if(ZLIB_FOUND)
        target_link_libraries(hermes_llm PRIVATE ZLIB::ZLIB)
    endif()
elseif(HERMES_LLM_HAS_CPR)
    target_link_libraries(hermes_llm PRIVATE cpr::cpr)
    target_compile_definitions(hermes_llm PRIVATE HERMES_LLM_HAS_CPR=1)
endif()

add_library(hermes::llm ALIAS hermes_llm)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
