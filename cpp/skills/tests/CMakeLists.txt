include(GoogleTest)

set(HERMES_SKILLS_TEST_SOURCES
    test_skill_utils.cpp
    test_skill_commands.cpp
    test_skills_hub.cpp
)

add_executable(hermes_skills_tests ${HERMES_SKILLS_TEST_SOURCES})
target_link_libraries(hermes_skills_tests
    PRIVATE
        hermes::skills
        hermes::core
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_skills_tests
    DISCOVERY_MODE PRE_TEST
)
