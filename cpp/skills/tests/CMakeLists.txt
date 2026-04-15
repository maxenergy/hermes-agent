include(GoogleTest)

set(HERMES_SKILLS_TEST_SOURCES
    test_skill_utils.cpp
    test_skill_commands.cpp
    test_skills_hub.cpp
    test_skills_sync.cpp
    test_skills_builtins_complete.cpp
    test_skills_guard.cpp
    test_skills_tool.cpp
)

add_executable(hermes_skills_tests ${HERMES_SKILLS_TEST_SOURCES})
target_link_libraries(hermes_skills_tests
    PRIVATE
        hermes::skills
        hermes::core
        hermes_llm
        GTest::gtest_main
        hermes_warnings
)

# Paths baked into the parity test so it can compare the C++ builtins
# directory against the upstream Python skills/ tree.
target_compile_definitions(hermes_skills_tests PRIVATE
    HERMES_BUILTINS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/skills/builtins"
    HERMES_PYTHON_SKILLS_DIR="${CMAKE_SOURCE_DIR}/../skills"
)

gtest_discover_tests(hermes_skills_tests
    DISCOVERY_MODE PRE_TEST
)
