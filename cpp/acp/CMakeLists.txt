# hermes::acp — Phase 16 ACP adapter for editor integration.

add_library(hermes_acp STATIC
    src/acp_adapter.cpp
    src/permissions.cpp
)

target_include_directories(hermes_acp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_acp PUBLIC cxx_std_17)

target_link_libraries(hermes_acp
    PUBLIC
        hermes::core
    PRIVATE
        hermes_warnings
)

add_library(hermes::acp ALIAS hermes_acp)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
