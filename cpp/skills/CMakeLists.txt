# hermes::skills — Phase 9 skill discovery, loading, slash-command
# injection, and Skills Hub client stub.

include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(STATUS "hermes::skills — fetching nlohmann_json 3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(hermes_skills STATIC
    src/skill_utils.cpp
    src/skill_commands.cpp
    src/skills_hub.cpp
    src/skills_sync.cpp
    src/skills_guard.cpp
    src/skills_tool.cpp
)

target_include_directories(hermes_skills
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_skills PUBLIC cxx_std_17)

target_link_libraries(hermes_skills
    PUBLIC
        hermes::core
        nlohmann_json::nlohmann_json
    PRIVATE
        hermes_llm
        hermes_warnings
)

add_library(hermes::skills ALIAS hermes_skills)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
