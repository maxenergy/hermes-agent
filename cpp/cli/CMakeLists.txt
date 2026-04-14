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

# yaml-cpp is needed by hook_discovery.cpp.  hermes::config keeps yaml-cpp
# PRIVATE, so we re-resolve it here.
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

# ----- Static library --------------------------------------------------------

add_library(hermes_cli_lib STATIC
    src/commands.cpp
    src/skin_engine.cpp
    src/display.cpp
    src/clipboard.cpp
    src/colors.cpp
    src/hermes_cli.cpp
    src/main_entry.cpp
    src/doctor.cpp
    src/claw_migrate.cpp
    src/hook_discovery.cpp
    src/ftxui_editor.cpp
    src/curses_ui.cpp
    src/rl_commands.cpp
    src/update_prompt.cpp
    src/providers_cmd.cpp
    src/model_switch_cmd.cpp
    src/auth_commands.cpp
    src/mcp_config.cpp
    src/models_cmd.cpp
    src/claw_cmd.cpp
    src/cron_cmd.cpp
    src/gateway_cmd.cpp
    src/logs_cmd.cpp
    src/uninstall_cmd.cpp
    src/callbacks.cpp
)

# Optional FTXUI binding for the multi-line editor. The FtxuiEditor state
# machine compiles regardless; this option only gates the live-terminal
# rendering, which requires the ftxui package. Default OFF so the default
# build does not pull in an additional dep.
option(HERMES_TUI_FTXUI "Enable FTXUI-based live terminal rendering for HermesCLI" OFF)
if(HERMES_TUI_FTXUI)
    find_package(ftxui CONFIG QUIET)
    if(ftxui_FOUND)
        target_compile_definitions(hermes_cli_lib PRIVATE HERMES_HAS_FTXUI=1)
        target_link_libraries(hermes_cli_lib PRIVATE
            ftxui::screen ftxui::dom ftxui::component)
        message(STATUS "hermes::cli: FTXUI terminal renderer enabled")
    else()
        message(WARNING
            "HERMES_TUI_FTXUI=ON but ftxui package was not found; "
            "falling back to plain-readline rendering.")
    endif()
endif()

target_include_directories(hermes_cli_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_cli_lib PUBLIC cxx_std_17)

find_package(SQLite3 REQUIRED)

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
        hermes::batch
        hermes::environments
        hermes::plugins
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes::gateway
        hermes::state
        hermes_warnings
        yaml-cpp::yaml-cpp
        SQLite::SQLite3
)

add_library(hermes::cli ALIAS hermes_cli_lib)

# ----- Executable -------------------------------------------------------------

add_executable(hermes_cpp src/main.cpp)
target_link_libraries(hermes_cpp PRIVATE hermes_cli_lib hermes_warnings)

# ----- Tests ------------------------------------------------------------------

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
