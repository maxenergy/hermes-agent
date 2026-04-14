include(GoogleTest)

add_executable(hermes_llm_tests
    test_message.cpp
    test_usage.cpp
    test_model_metadata.cpp
    test_model_normalize.cpp
    test_smart_routing.cpp
    test_ollama.cpp
    test_models_dev.cpp
    test_prompt_cache.cpp
    test_llm_client.cpp
    test_error_classifier.cpp
    test_retry_policy.cpp
    test_credential_pool.cpp
    test_codex_models.cpp
    test_runtime_provider.cpp
    test_model_switch.cpp
    test_anthropic_features.cpp
    test_tokenizer.cpp
    test_auxiliary_fallback.cpp
    test_anthropic_depth.cpp
)

target_link_libraries(hermes_llm_tests
    PRIVATE
        hermes_llm
        GTest::gtest_main
        hermes_warnings
)

gtest_discover_tests(hermes_llm_tests
    DISCOVERY_MODE PRE_TEST
)
