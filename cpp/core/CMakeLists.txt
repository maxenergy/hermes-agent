# nlohmann_json for uds_rpc (JSON-RPC 2.0 framing + payload parsing).
# Reuse the system/vcpkg copy if available; fall back to FetchContent
# v3.11.3 to match hermes::config (avoid ABI drift with 3.12.x headers).
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        message(STATUS "hermes::core: nlohmann_json not found — FetchContent v3.11.3")
        FetchContent_Declare(
            nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

add_library(hermes_core STATIC
    src/strings.cpp
    src/path.cpp
    src/atomic_io.cpp
    src/env.cpp
    src/time.cpp
    src/redact.cpp
    src/logging.cpp
    src/retry.cpp
    src/url_safety.cpp
    src/ansi_strip.cpp
    src/fuzzy.cpp
    src/patch_parser.cpp
    src/uds_rpc.cpp
)

target_include_directories(hermes_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_core PUBLIC cxx_std_17)
target_link_libraries(hermes_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE hermes_warnings
)
if(UNIX)
    target_link_libraries(hermes_core PRIVATE pthread)
endif()

add_library(hermes::core ALIAS hermes_core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
