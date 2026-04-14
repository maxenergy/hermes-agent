# hermes::agent — Phase 4 core orchestration layer.
#
# Composes Phase 1-3 libraries (core/config/state/llm) into the conversation
# loop: PromptBuilder, ContextCompressor, MemoryManager, AIAgent.

if(NOT TARGET yaml-cpp::yaml-cpp)
    find_package(yaml-cpp CONFIG QUIET)
endif()

add_library(hermes_agent STATIC
    src/prompt_builder.cpp
    src/context_compressor.cpp
    src/memory_manager.cpp
    src/memory_provider.cpp
    src/honcho_provider.cpp
    src/subdirectory_hints.cpp
    src/iteration_budget.cpp
    src/title_generator.cpp
    src/auxiliary_client.cpp
    src/ai_agent.cpp
    src/context_references.cpp
    src/compression_feedback.cpp
    src/insights.cpp
    src/redact.cpp
    src/retry_utils.cpp
    src/prompt_caching.cpp
    src/rate_limit_tracker.cpp
    src/smart_model_routing.cpp
    src/error_classifier.cpp
    src/context_engine.cpp
    src/at_references.cpp
    src/trajectory.cpp
    src/prompt_guidance.cpp
    src/insights_format.cpp
    src/usage_pricing.cpp
    src/context_truncate.cpp
    src/aux_provider_alias.cpp
    src/credential_pool.cpp
    src/skill_commands.cpp
    src/session_state.cpp
    src/tool_dispatch_wrapper.cpp
    src/skill_utils.cpp
)

target_include_directories(hermes_agent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_agent PUBLIC cxx_std_17)

target_link_libraries(hermes_agent
    PUBLIC
        hermes::core
        hermes::config
        hermes::state
        hermes::llm
    PRIVATE
        hermes_warnings
        yaml-cpp::yaml-cpp
)

add_library(hermes::agent ALIAS hermes_agent)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
