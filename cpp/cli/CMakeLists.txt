# hermes::cli — Phase 13 CLI command system, skin engine, hermes entry point.

include(FetchContent)

# Use the same nlohmann_json version as hermes::config (3.11.3) to avoid
# ABI mismatches with system-installed 3.12.x headers.
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ----- Static library --------------------------------------------------------

add_library(hermes_cli_lib STATIC
    src/commands.cpp
    src/skin_engine.cpp
    src/display.cpp
    src/clipboard.cpp
    src/colors.cpp
    src/hermes_cli.cpp
    src/main_entry.cpp
    src/claw_migrate.cpp
)

target_include_directories(hermes_cli_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_cli_lib PUBLIC cxx_std_17)

target_link_libraries(hermes_cli_lib
    PUBLIC
        hermes::core
        hermes::config
        hermes::tools
        hermes::skills
        hermes::cron
        hermes::profile
        hermes::auth
        hermes::agent
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes::gateway
        hermes_warnings
)

add_library(hermes::cli ALIAS hermes_cli_lib)

# ----- Executable -------------------------------------------------------------

add_executable(hermes_cpp src/main.cpp)
target_link_libraries(hermes_cpp PRIVATE hermes_cli_lib hermes_warnings)

# ----- Tests ------------------------------------------------------------------

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
