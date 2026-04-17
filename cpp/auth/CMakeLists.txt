# hermes::auth — credential store (.env) + dotenv loader.

add_library(hermes_auth STATIC
    src/credentials.cpp
    src/env_loader.cpp
    src/copilot_oauth.cpp
    src/codex_oauth.cpp
    src/nous_subscription.cpp
    src/qwen_oauth.cpp
    src/qwen_client.cpp
    src/mcp_oauth.cpp
)

target_include_directories(hermes_auth
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_auth PUBLIC cxx_std_17)

find_package(OpenSSL REQUIRED)

target_link_libraries(hermes_auth
    PUBLIC
        hermes::core
        hermes::llm
    PRIVATE
        hermes_warnings
        OpenSSL::Crypto
)

add_library(hermes::auth ALIAS hermes_auth)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
