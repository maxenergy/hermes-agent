# hermes::state — Phase 2 persistence layer (SessionDB, ProcessRegistry,
# MemoryStore, TrajectoryWriter).

include(FetchContent)

# ----- Dependencies ------------------------------------------------------

# nlohmann_json: prefer an installed package; fall back to a FetchContent
# download when nothing is available.
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "hermes::state — fetching nlohmann_json 3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# SQLite3: system package is the only sensible source.
find_package(SQLite3 QUIET)

# SQLiteCpp: installed package first; otherwise FetchContent with the
# bundled SQLite build that ships FTS5 by default.
if(NOT TARGET SQLiteCpp)
    find_package(SQLiteCpp CONFIG QUIET)
endif()
if(NOT TARGET SQLiteCpp)
    message(STATUS "hermes::state — fetching SQLiteCpp 3.3.1")
    set(SQLITECPP_INTERNAL_SQLITE ON CACHE BOOL "" FORCE)
    set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
    set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
    set(SQLITECPP_RUN_DOXYGEN OFF CACHE BOOL "" FORCE)
    set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SQLiteCpp
        GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
        GIT_TAG 3.3.1
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(SQLiteCpp)
endif()

# ----- Library -----------------------------------------------------------

add_library(hermes_state STATIC
    src/session_db.cpp
    src/process_registry.cpp
    src/memory_store.cpp
    src/trajectory.cpp
    src/checkpoint_manager.cpp
)

target_include_directories(hermes_state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_state PUBLIC cxx_std_17)

target_link_libraries(hermes_state
    PUBLIC
        hermes::core
        nlohmann_json::nlohmann_json
    PRIVATE
        SQLiteCpp
        hermes_warnings
)

# SQLiteCpp's public headers pull in <sqlite3.h>; link the system lib
# when found so downstream consumers of hermes::state do not need to
# hunt for it themselves.
if(TARGET SQLite::SQLite3)
    target_link_libraries(hermes_state PRIVATE SQLite::SQLite3)
elseif(SQLite3_FOUND)
    target_link_libraries(hermes_state PRIVATE ${SQLite3_LIBRARIES})
    target_include_directories(hermes_state PRIVATE ${SQLite3_INCLUDE_DIRS})
endif()

add_library(hermes::state ALIAS hermes_state)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
