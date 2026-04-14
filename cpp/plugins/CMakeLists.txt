# hermes::plugins — Phase 17 plugin system (discover, load, manage plugins).

include(FetchContent)

# yaml-cpp — needed for plugin.yaml manifest parsing.
if(NOT TARGET yaml-cpp::yaml-cpp)
    find_package(yaml-cpp CONFIG QUIET)
    if(NOT yaml-cpp_FOUND)
        FetchContent_Declare(
            yaml-cpp
            URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
        )
        set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(yaml-cpp)
    endif()
endif()

# nlohmann_json — state.json persistence.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(hermes_plugins STATIC
    src/plugin_manager.cpp
    src/manifest.cpp
    src/state.cpp
    src/plugins_cli.cpp
)

target_include_directories(hermes_plugins
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_plugins PUBLIC cxx_std_17)

target_link_libraries(hermes_plugins
    PUBLIC
        hermes::core
        hermes::tools
        nlohmann_json::nlohmann_json
    PRIVATE
        ${CMAKE_DL_LIBS}
        yaml-cpp::yaml-cpp
        hermes_warnings
)

add_library(hermes::plugins ALIAS hermes_plugins)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
