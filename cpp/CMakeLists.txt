cmake_minimum_required(VERSION 3.20)

# NOTE: Phase 0 intentionally does NOT auto-activate the vcpkg toolchain.
# `hermes_core` has no external dependencies, so the default configure
# must succeed on a clean Linux box with zero vcpkg install time.
#
# To opt into the vcpkg-provided dependencies (Phase 1+ modules), set
# `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`
# on the first configure.

project(hermes_cpp
    VERSION 0.2.0
    LANGUAGES CXX
    DESCRIPTION "C++17 reimplementation of the Hermes Agent backend")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Shared warning flags applied to all first-party targets via the
# hermes_warnings INTERFACE library.
add_library(hermes_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hermes_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
include(CTest)

add_subdirectory(core)
add_subdirectory(approval)
add_subdirectory(config)
add_subdirectory(profile)
add_subdirectory(state)
add_subdirectory(external)
add_subdirectory(llm)
add_subdirectory(auth)
add_subdirectory(environments)
add_subdirectory(agent)
add_subdirectory(tools)
add_subdirectory(skills)
add_subdirectory(cron)
add_subdirectory(gateway)
add_subdirectory(cli)
add_subdirectory(batch)
add_subdirectory(plugins)
add_subdirectory(mcp_server)
add_subdirectory(acp)
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Install hooks: ship built-in and optional skills alongside the binary.
# These live at the Python repo root (one level up from cpp/) and are
# copied into $CMAKE_INSTALL_PREFIX/share/hermes/ at install time.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/../skills)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/../skills
            DESTINATION share/hermes)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/../optional-skills)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/../optional-skills
            DESTINATION share/hermes)
endif()
# Self-contained copy of the built-in skill collection shipped with the
# C++ binary. Installed to share/hermes/builtins so the skill index can
# discover it even when the Python repo tree is absent (e.g. Termux,
# container, Windows).
if(EXISTS ${CMAKE_SOURCE_DIR}/skills/builtins)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/skills/builtins/
            DESTINATION share/hermes/builtins
            FILES_MATCHING PATTERN "SKILL.md")
endif()
