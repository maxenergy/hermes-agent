add_library(hermes_environments STATIC
    src/env_filter.cpp
    src/cwd_tracker.cpp
    src/session_snapshot.cpp
    src/local.cpp
    src/docker.cpp
    src/ssh.cpp
    src/file_sync.cpp
    src/snapshot_store.cpp
    src/modal.cpp
    src/daytona.cpp
    src/singularity.cpp
    src/managed_modal.cpp
    src/dotfile_sync.cpp
    src/scp_copier.cpp
    src/spawn_via_env.cpp
    src/tool_context.cpp
    src/agent_loop.cpp
    src/hermes_base_env.cpp
    src/opd_helpers.cpp
    src/web_research_helpers.cpp
)

target_include_directories(hermes_environments
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_environments PUBLIC cxx_std_17)
target_link_libraries(hermes_environments
    PUBLIC hermes::core hermes::llm
    PRIVATE hermes_warnings
)

# pthread is needed for std::thread (watchdog, cancellation).
find_package(Threads REQUIRED)
target_link_libraries(hermes_environments PUBLIC Threads::Threads)

# libutil provides forkpty() on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(hermes_environments PRIVATE util)
endif()

add_library(hermes::environments ALIAS hermes_environments)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
