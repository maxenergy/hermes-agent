if(NOT TARGET GTest::gtest_main)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(hermes_agent_tests
    test_prompt_builder.cpp
    test_context_compressor.cpp
    test_memory_manager.cpp
    test_iteration_budget.cpp
    test_subdirectory_hints.cpp
    test_title_generator.cpp
    test_auxiliary_client.cpp
    test_ai_agent.cpp
    test_trajectory_saving.cpp
    test_honcho_provider.cpp
    test_context_references.cpp
    test_compression_feedback.cpp
    test_insights.cpp
    test_redact.cpp
    test_retry_utils.cpp
    test_prompt_caching.cpp
    test_rate_limit_tracker.cpp
    test_smart_model_routing.cpp
    test_error_classifier.cpp
    test_context_engine.cpp
    test_memory_provider.cpp
    test_at_references.cpp
    test_memory_manager_ext.cpp
    test_trajectory_helpers.cpp
)

target_link_libraries(hermes_agent_tests
    PRIVATE
        hermes_agent
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_agent_tests
    DISCOVERY_MODE PRE_TEST
)
