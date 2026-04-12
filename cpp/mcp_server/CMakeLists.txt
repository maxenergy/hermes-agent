# hermes::mcp_server — Phase 15 MCP server for editor integration.

add_library(hermes_mcp_server STATIC
    src/mcp_server.cpp
)

target_include_directories(hermes_mcp_server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(hermes_mcp_server PUBLIC cxx_std_17)

target_link_libraries(hermes_mcp_server
    PUBLIC
        hermes::state
        hermes::core
    PRIVATE
        hermes_warnings
)

add_library(hermes::mcp_server ALIAS hermes_mcp_server)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
