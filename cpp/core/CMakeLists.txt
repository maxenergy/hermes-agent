add_library(hermes_core STATIC
    src/strings.cpp
    src/path.cpp
    src/atomic_io.cpp
    src/env.cpp
    src/time.cpp
    src/redact.cpp
    src/logging.cpp
    src/retry.cpp
    src/url_safety.cpp
    src/ansi_strip.cpp
    src/fuzzy.cpp
    src/patch_parser.cpp
)

target_include_directories(hermes_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_core PUBLIC cxx_std_17)
target_link_libraries(hermes_core PRIVATE hermes_warnings)

add_library(hermes::core ALIAS hermes_core)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
