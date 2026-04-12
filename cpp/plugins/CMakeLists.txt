# hermes::plugins — Phase 17 plugin system (discover, load, manage plugins).

add_library(hermes_plugins STATIC
    src/plugin_manager.cpp
)

target_include_directories(hermes_plugins
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_plugins PUBLIC cxx_std_17)

target_link_libraries(hermes_plugins
    PUBLIC
        hermes::core
        hermes::tools
    PRIVATE
        ${CMAKE_DL_LIBS}
        hermes_warnings
)

add_library(hermes::plugins ALIAS hermes_plugins)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
