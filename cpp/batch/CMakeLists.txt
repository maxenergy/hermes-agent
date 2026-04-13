# hermes::batch — Phase 14 batch processing and trajectory compression.

add_library(hermes_batch STATIC
    src/batch_runner.cpp
    src/trajectory_compressor.cpp
    src/hf_schema.cpp
    src/swe_runner.cpp
)

target_include_directories(hermes_batch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_batch PUBLIC cxx_std_17)

target_link_libraries(hermes_batch
    PUBLIC
        hermes::agent
        hermes::llm
        hermes::environments
    PRIVATE
        hermes_warnings
)

add_library(hermes::batch ALIAS hermes_batch)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
