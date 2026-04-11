# External dependency wiring stub.
#
# Phase 0 intentionally leaves this empty: the hermes_core static library
# in ../core depends only on the C++17 standard library, so the Phase 0
# build does not require vcpkg to be installed.
#
# Each package below is declared via find_package with all targets guarded
# by `if(NOT <target>)` so that a missing dependency produces a clear
# status line (and a downstream WARNING if the dep is actually needed)
# rather than a hard configure error. When Phase 1 starts wiring real
# modules they simply `target_link_libraries(... PRIVATE <target>)`.

set(_HERMES_OPTIONAL_DEPS
    nlohmann_json
    yaml-cpp
    spdlog
    fmt
    CLI11
    cpr
    OpenSSL
    SQLite3
    SQLiteCpp
    re2
    ftxui
    inja
    jwt-cpp
    md4c
    Boost
)

foreach(dep IN LISTS _HERMES_OPTIONAL_DEPS)
    find_package(${dep} QUIET CONFIG)
    if(NOT ${dep}_FOUND)
        # Try the MODULE variant for packages shipped as classic FindXxx.cmake.
        find_package(${dep} QUIET MODULE)
    endif()
    if(${dep}_FOUND)
        message(STATUS "hermes/external: ${dep} — found")
    else()
        message(STATUS "hermes/external: ${dep} — NOT found (Phase 1+ dependency)")
    endif()
endforeach()
