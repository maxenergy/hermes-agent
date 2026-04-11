# hermes::config — default config + loader (YAML merge, env expansion,
# migration stub, managed-system detection).

# Prefer system/vcpkg packages; fall back to FetchContent if they are
# missing.  yaml-cpp and nlohmann_json are both available via vcpkg
# manifest and most modern package managers.
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    message(STATUS "hermes::config: nlohmann_json not found — FetchContent v3.11.3")
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    include(FetchContent)
    message(STATUS "hermes::config: yaml-cpp not found — FetchContent 0.8.0")
    FetchContent_Declare(
        yaml-cpp
        URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    )
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

add_library(hermes_config STATIC
    src/default_config.cpp
    src/optional_env_vars.cpp
    src/loader.cpp
)

target_include_directories(hermes_config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_config PUBLIC cxx_std_17)

target_link_libraries(hermes_config
    PUBLIC
        hermes::core
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes_warnings
        yaml-cpp::yaml-cpp
)

add_library(hermes::config ALIAS hermes_config)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
