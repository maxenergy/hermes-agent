# hermes::gateway — Phase 11 gateway core: runner, session store, hooks,
# pairing, status/locks, and config.

include(FetchContent)

# ----- Dependencies ------------------------------------------------------

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "hermes::gateway — fetching nlohmann_json 3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    message(STATUS "hermes::gateway — OpenSSL NOT found; SHA256 hashing will fail at link time")
endif()

# ----- Library -----------------------------------------------------------

add_library(hermes_gateway STATIC
    src/gateway_config.cpp
    src/session_store.cpp
    src/hooks.cpp
    src/pairing.cpp
    src/status.cpp
    src/gateway_runner.cpp
)

target_include_directories(hermes_gateway
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_gateway PUBLIC cxx_std_17)

target_link_libraries(hermes_gateway
    PUBLIC
        hermes::core
        hermes::config
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes_warnings
)

# OpenSSL::Crypto must be PUBLIC: its -isystem path affects which
# nlohmann_json headers the compiler finds (vcpkg vs system).  If this
# were PRIVATE, downstream consumers would compile against a different
# nlohmann ABI version and fail to link.
if(OpenSSL_FOUND)
    target_link_libraries(hermes_gateway PUBLIC OpenSSL::Crypto)
endif()

add_library(hermes::gateway ALIAS hermes_gateway)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
